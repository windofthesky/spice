/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009-2015 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifndef CHAR_DEVICE_H_
#define CHAR_DEVICE_H_

#include "spice.h"
#include "red-channel.h"
#include "migration-protocol.h"

/*
 * Shared code for char devices, mainly for flow control.
 *
 * How to use the api:
 * ==================
 * device attached: call spice_char_device_state_create
 * device detached: call spice_char_device_state_destroy/reset
 *
 * client connected and associated with a device: spice_char_device_client_add
 * client disconnected: spice_char_device_client_remove
 *
 * Writing to the device
 * ---------------------
 * Write the data into RedCharDeviceWriteBuffer:
 * call spice_char_device_write_buffer_get in order to get an appropriate buffer.
 * call spice_char_device_write_buffer_add in order to push the buffer to the write queue.
 * If you choose not to push the buffer to the device, call
 * spice_char_device_write_buffer_release
 *
 * reading from the device
 * -----------------------
 *  The callback read_one_msg_from_device (see below) should be implemented
 *  (using sif->read).
 *  When the device is ready, this callback is called, and is expected to
 *  return one message which is addressed to the client, or NULL if the read
 *  hasn't completed.
 *
 * calls triggered from the device (qemu):
 * --------------------------------------
 * spice_char_device_start
 * spice_char_device_stop
 * spice_char_device_wakeup (for reading from the device)
 */

/*
 * Note about multiple-clients:
 * Multiclients are currently not supported in any of the character devices:
 * spicevmc does not allow more than one client (and at least for usb, it should stay this way).
 * smartcard code is not compatible with more than one reader.
 * The server and guest agent code doesn't distinguish messages from different clients.
 * In addition, its current flow control code (e.g., tokens handling) is wrong and doesn't
 * take into account the different clients.
 *
 * Nonetheless, the following code introduces some support for multiple-clients:
 * We track the number of tokens for all the clients, and we read from the device
 * if one of the clients have enough tokens. For the clients that don't have tokens,
 * we queue the messages, till they receive tokens, or till a timeout.
 *
 * TODO:
 * At least for the agent, not all the messages from the device will be directed to all
 * the clients (e.g., copy from guest to a specific client). Thus, support for
 * client-specific-messages should be added.
 * In addition, we should have support for clients that are being connected
 * in the middle of a message transfer from the agent to the clients.
 *
 * */

struct RedsState;

/* buffer that is used for writing to the device */
typedef struct RedCharDeviceWriteBuffer {
    RingItem link;
    int origin;
    RedClient *client; /* The client that sent the message to the device.
                          NULL if the server created the message */

    uint8_t *buf;
    uint32_t buf_size;
    uint32_t buf_used;
    uint32_t token_price;
    uint32_t refs;
} RedCharDeviceWriteBuffer;

typedef void RedCharDeviceMsgToClient;

typedef struct SpiceCharDeviceCallbacks {
    /*
     * Messages that are addressed to the client can be queued in case we have
     * multiple clients and some of them don't have enough tokens.
     */

    /* reads from the device till reaching a msg that should be sent to the client,
     * or till the reading fails */
    RedCharDeviceMsgToClient* (*read_one_msg_from_device)(SpiceCharDeviceInstance *sin,
                                                            void *opaque);
    RedCharDeviceMsgToClient* (*ref_msg_to_client)(RedCharDeviceMsgToClient *msg,
                                                   void *opaque);
    void (*unref_msg_to_client)(RedCharDeviceMsgToClient *msg,
                                void *opaque);
    void (*send_msg_to_client)(RedCharDeviceMsgToClient *msg,
                               RedClient *client,
                               void *opaque); /* after this call, the message is unreferenced */

    /* The cb is called when a predefined number of write buffers were consumed by the
     * device */
    void (*send_tokens_to_client)(RedClient *client, uint32_t tokens, void *opaque);

    /* The cb is called when a server (self) message that was addressed to the device,
     * has been completely written to it */
    void (*on_free_self_token)(void *opaque);

    /* This cb is called if it is recommanded that a client will be removed
     * due to slow flow or due to some other error.
     * The called instance should disconnect the client, or at least the corresponding channel */
    void (*remove_client)(RedClient *client, void *opaque);
} SpiceCharDeviceCallbacks;

SpiceCharDeviceState *spice_char_device_state_create(SpiceCharDeviceInstance *sin,
                                                     struct RedsState *reds,
                                                     uint32_t client_tokens_interval,
                                                     uint32_t self_tokens,
                                                     SpiceCharDeviceCallbacks *cbs,
                                                     void *opaque);

void spice_char_device_state_reset_dev_instance(SpiceCharDeviceState *dev,
                                                SpiceCharDeviceInstance *sin);
void spice_char_device_state_destroy(SpiceCharDeviceState *dev);

void *spice_char_device_state_opaque_get(SpiceCharDeviceState *dev);

/* only one client is supported */
void spice_char_device_state_migrate_data_marshall(SpiceCharDeviceState *dev,
                                                  SpiceMarshaller *m);
void spice_char_device_state_migrate_data_marshall_empty(SpiceMarshaller *m);

int spice_char_device_state_restore(SpiceCharDeviceState *dev,
                                    SpiceMigrateDataCharDevice *mig_data);

/*
 * Resets write/read queues, and moves that state to being stopped.
 * This routine is a workaround for a bad tokens management in the vdagent
 * protocol:
 *  The client tokens' are set only once, when the main channel is initialized.
 *  Instead, it would have been more appropriate to reset them upon AGEN_CONNECT.
 *  The client tokens are tracked as part of the SpiceCharDeviceClientState. Thus,
 *  in order to be backwartd compatible with the client, we need to track the tokens
 *  event when the agent is detached. We don't destroy the char_device state, and
 *  instead we just reset it.
 *  In addition, there is a misshandling of AGENT_TOKENS message in spice-gtk: it
 *  overrides the amount of tokens, instead of adding the given amount.
 *
 *  todo: change AGENT_CONNECT msg to contain tokens count.
 */
void spice_char_device_reset(SpiceCharDeviceState *dev);

/* max_send_queue_size = how many messages we can read from the device and enqueue for this client,
 * when we have tokens for other clients and no tokens for this one */
int spice_char_device_client_add(SpiceCharDeviceState *dev,
                                 RedClient *client,
                                 int do_flow_control,
                                 uint32_t max_send_queue_size,
                                 uint32_t num_client_tokens,
                                 uint32_t num_send_tokens,
                                 int wait_for_migrate_data);

void spice_char_device_client_remove(SpiceCharDeviceState *dev,
                                     RedClient *client);
int spice_char_device_client_exists(SpiceCharDeviceState *dev,
                                    RedClient *client);

void spice_char_device_start(SpiceCharDeviceState *dev);
void spice_char_device_stop(SpiceCharDeviceState *dev);
SpiceServer* spice_char_device_get_server(SpiceCharDeviceState *dev);

/** Read from device **/

void spice_char_device_wakeup(SpiceCharDeviceState *dev);

void spice_char_device_send_to_client_tokens_add(SpiceCharDeviceState *dev,
                                                 RedClient *client,
                                                 uint32_t tokens);


void spice_char_device_send_to_client_tokens_set(SpiceCharDeviceState *dev,
                                                 RedClient *client,
                                                 uint32_t tokens);
/** Write to device **/

RedCharDeviceWriteBuffer *spice_char_device_write_buffer_get(SpiceCharDeviceState *dev,
                                                             RedClient *client, int size);
RedCharDeviceWriteBuffer *spice_char_device_write_buffer_get_server_no_token(
    SpiceCharDeviceState *dev, int size);

/* Either add the buffer to the write queue or release it */
void spice_char_device_write_buffer_add(SpiceCharDeviceState *dev,
                                        RedCharDeviceWriteBuffer *write_buf);
void spice_char_device_write_buffer_release(SpiceCharDeviceState *dev,
                                            RedCharDeviceWriteBuffer *write_buf);

/* api for specific char devices */

SpiceCharDeviceState *spicevmc_device_connect(struct RedsState *reds,
                                              SpiceCharDeviceInstance *sin,
                                              uint8_t channel_type);
void spicevmc_device_disconnect(struct RedsState *reds,
                                SpiceCharDeviceInstance *char_device);

SpiceCharDeviceInterface *spice_char_device_get_interface(SpiceCharDeviceInstance *instance);

#endif // CHAR_DEVICE_H_
