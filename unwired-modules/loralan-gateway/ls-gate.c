/*
 * Copyright (C) 2016 Unwired Devices
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file		ls_end_device.c
 * @brief       Implementation of LoRa-Star stack for end-device
 * @author      Eugene Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "random.h"
#include "assert.h"
#include "thread.h"

#include "ls-init-device.h"
#include "ls-mac-types.h"
#include "ls-mac.h"
#include "ls-gate.h"

#include "rtctimers-millis.h"

#include <stdint.h>

#define ENABLE_DEBUG (1)
#include "debug.h"

static msg_t msg_ping;
static msg_t msg_rx1_expired;

static void schedule_tx(ls_gate_channel_t *ch) {
	/* Can send next frame only if channel is doing nothing */
	if (ch->state != LS_GATE_CHANNEL_STATE_IDLE) {
		puts("ls-gate: frame enqueued until channel is free");
		return;
	}

	msg_t msg;
	msg.content.ptr = (void *) ch;

	msg_try_send(&msg, ((ls_gate_t *)ch->_internal.gate)->_internal.uq_thread_pid);
}

static void prepare_sx1276(ls_gate_channel_t *ch)
{
    ls_setup_sx1276(ch->_internal.sx1276, ch->dr, ch->frequency);
    DEBUG("ls-gate: SX1276 configured\n");
}

static int send_frame_f(ls_gate_channel_t *ch, ls_frame_t *frame)
{
    assert(ch != NULL);

    ls_gate_t *ls = (ls_gate_t *) ch->_internal.gate;

    /* Send frame */
    size_t header_size = sizeof(ls_header_t) + sizeof(ls_payload_len_t);
    size_t payload_size = 0;

    /* The JOIN_ACK frame must be encrypted with the special join key */
    ls_gate_node_t *node;

    uint8_t mic_key[AES_BLOCK_SIZE];
    uint8_t aes_key[AES_BLOCK_SIZE];

    switch (frame->header.type) {
        case LS_DL_JOIN_ACK:
        case LS_DL_INVITE:
        case LS_DL_BROADCAST:
            ls_encrypt_frame(ls->settings.join_key, ls->settings.join_key, frame, &payload_size);
            break;

        case LS_DL_ACK:
            node = ls_devlist_get(&ls->devices, frame->header.dev_addr);

            ls_derive_keys(node->nonce[node->num_nonces - 1], node->app_nonce, node->addr, mic_key, NULL);
            ls_encrypt_frame(mic_key, mic_key, frame, &payload_size);
            break;

        default:
            node = ls_devlist_get(&ls->devices, frame->header.dev_addr);

            ls_derive_keys(node->nonce[node->num_nonces - 1], node->app_nonce, node->addr, mic_key, aes_key);
            ls_encrypt_frame(mic_key, aes_key, frame, &payload_size);
    }
    
    /* REG_LR_MODEMSTAT doesn't seems to work properly
    int delay_ms = 5 + ((100 + 10*ch->dr) >> ch->dr);
    int total_time = 0;
    while (sx1276_get_modem_status(ch->_internal.sx1276) != SX1276_MODEM_CLEAR) {
        DEBUG("ls-gate: modem is in RX, postpone transmission\n");
        rtctimers_millis_sleep(delay_ms);
        total_time += delay_ms;
        if (total_time > 1000) {
            DEBUG("ls-gate: force switching from RX to TX\n");
            break;
        }
    }
    */

    /* Capture channel */
    mutex_lock(&ch->_internal.channel_mutex);

    puts("ls-gate: state = TX");
    ch->state = LS_GATE_CHANNEL_STATE_TX;

    /* Prepare transceiver */
    prepare_sx1276(ch);

    /* Send frame into LoRa PHY */
    sx1276_send(ch->_internal.sx1276, (uint8_t *) frame, header_size + payload_size);
    
    DEBUG("ls-gate: frame sent\n");

    mutex_unlock(&ch->_internal.channel_mutex);

    return LS_GATE_OK;
}

static bool enqueue_frame_f(ls_gate_channel_t *ch, ls_frame_t *frame) {
	bool res = !ls_frame_fifo_push(&ch->_internal.ul_fifo, frame);

	schedule_tx(ch);
    
    DEBUG("ls-gate: frame scheduled\n");

	return res;
}

static bool enqueue_frame(ls_gate_channel_t *ch, ls_addr_t to, ls_type_t type, uint8_t *buf, size_t buflen) {
    ls_frame_t *frame = &ch->_internal.current_frame;
    ls_assemble_frame(to, type, buf, buflen, frame);

    return enqueue_frame_f(ch, frame);
}

static inline void close_rx_windows(ls_gate_channel_t *ch) {
	xtimer_remove(&ch->_internal.rx_window1);
    puts("ls-gate: state = IDLE");
	ch->state = LS_GATE_CHANNEL_STATE_IDLE;
    
    DEBUG("ls-gate: RX window closed\n");

	/* Free channel for TX */
	//mutex_unlock(&ch->_internal.channel_mutex);
}

static inline void open_rx_windows(ls_gate_channel_t *ch) {
	/* Launch RX window timeout timer */
	msg_rx1_expired.content.ptr = (void *) ch;
	xtimer_set_msg(&ch->_internal.rx_window1, LS_GATE_RX1_LENGTH, &msg_rx1_expired, ((ls_gate_t *)ch->_internal.gate)->_internal.tim_thread_pid);

	/* Switch transceiver to RX mode */
	prepare_sx1276(ch);
	sx1276_set_rx(ch->_internal.sx1276, ch->_internal.sx1276->settings.lora.rx_timeout);

    /* Capture channel on RX */
    //mutex_lock(&ch->_internal.channel_mutex);
    puts("ls-gate: state = RX");
    ch->state = LS_GATE_CHANNEL_STATE_RX;

	DEBUG("ls-gate: rx1 window opened\n");
}

static inline void send_join_ack(ls_gate_t *ls, ls_gate_channel_t *ch, uint64_t dev_id, ls_addr_t addr, uint32_t app_nonce)
{
    ls_join_ack_t ack = { .addr = addr, .dev_id = dev_id, .app_nonce = app_nonce };

    enqueue_frame(ch, addr, LS_DL_JOIN_ACK, (uint8_t *) &ack, sizeof(ls_join_ack_t));
}

static inline void send_ack(ls_gate_t *ls, ls_gate_channel_t *ch, ls_addr_t addr)
{
	enqueue_frame(ch, addr, LS_DL_ACK, NULL, 0);
}

static void device_join_req(ls_gate_t *ls, ls_gate_channel_t *ch, uint64_t dev_id, uint64_t app_id, uint32_t dev_nonce, ls_node_class_t node_class)
{
    DEBUG("ls-gate: join request from %08x%08x\n", (unsigned int) (dev_id >> 32), (unsigned int) (dev_id & 0xFFFFFFFF));
    
    /* Check node acceptance */
    if (ls->accept_node_join_cb != NULL) {
        if (!ls->accept_node_join_cb(dev_id, app_id)) {
            DEBUG("ls-gate: join request rejected\n");
            return;
        }
    }

    /* Check that nonce doesn't repeated */
    ls_gate_devices_t *devlist = &ls->devices;

    if (!ls_devlist_check_nonce(devlist, dev_id, dev_nonce)) {
        DEBUG("ls-gate: nonce rejected\n");
        return;
    }

    ls_gate_node_t *node;
    if (!ls_devlist_is_added(devlist, dev_id)) {
        /* Add node to the devices list */
        node = ls_devlist_add(devlist, dev_id, app_id, dev_nonce, ch);
        DEBUG("ls-gate: node added to devlist\n");
    }
    else {
        node = add_nonce(devlist, dev_id, dev_nonce);
        DEBUG("ls-gate: nonce added\n");
    }
    
    if (!node) {
        DEBUG("ls-gate: error adding node\n");
        return;
    }

    /* Set node's class */
    node->node_class = node_class;

    /* Update node's channel */
    node->node_ch = ch;

    /* Update node's last seen time */
    node->last_seen = ls->_internal.ping_count;

    /* Call join handler which returns an app nonce from the application side */
    node->app_nonce = ls->node_joined_cb(node);

    /* Reset last frame ID counter */
    node->last_fid = 0;

    /* Send join ACK */
    DEBUG("ls-gate: send join ack\n");
    send_join_ack(ls, ch, dev_id, node->addr, node->app_nonce);
}

static void app_data_recv(ls_gate_t *ls, ls_gate_channel_t *ch, ls_gate_node_t *node, ls_frame_t *frame, uint8_t *aes_key)
{
    DEBUG("ls-gate: app data frame received\n");

    /* Decrypt frame payload */
    DEBUG("ls-gate: decrypt frame payload\n");
    ls_decrypt_frame_payload(aes_key, &frame->payload);

    /* Call handler callback */
    DEBUG("ls-gate: call handler callback\n");
    ls->app_data_received_cb(node, ch, frame->payload.data, frame->payload.len, frame->header.status);
}

static bool frame_recv(ls_gate_t *ls, ls_gate_channel_t *ch, ls_frame_t *frame)
{
    DEBUG("ls-gate: frame received\n");
    
    /* Validate node address and get corresponding node record */
    ls_gate_node_t *node = NULL;
    if (frame->header.dev_addr != LS_ADDR_UNDEFINED) {
    	node = ls_devlist_get(&ls->devices, frame->header.dev_addr);

    	if (node == NULL) {
    		DEBUG("ls-gate: node must be joined\n");
    		return false;
    	}
    }

    /* Update node's last seen time */
    node->last_seen = ls->_internal.ping_count;

    /* Derive cryptographic keys */
    uint8_t mic_key[AES_BLOCK_SIZE];
    uint8_t aes_key[AES_BLOCK_SIZE];

    if (node) {
        ls_derive_keys(node->nonce[node->num_nonces - 1], node->app_nonce, node->addr, mic_key, aes_key);

        /* Validate frame MIC */
        if (!ls_validate_frame_mic(mic_key, frame)) {
            DEBUG("ls-gate: MIC validation failed\n");
            return false;
        }
    }

    switch (frame->header.type) {
    	case LS_UL_UNC_ACK: { /* Unconfirmed data with ack for previous data */
            /* Node must be defined */
            if (!node) {
            	DEBUG("ls-gate: node undefined\n");
            	return false;
            }

            DEBUG("ls-gate: unconfirmed data with ack for previous data\n");

            if (frame->header.fid >= (uint8_t) (node->last_fid + 1)) {
            	/* Update frame ID */
                DEBUG("ls-gate: update frame ID\n");
            	node->last_fid = frame->header.fid;

                /*
                 * Process as acknowledge frame
                 */
            	if (ls->app_data_ack_cb != NULL) {
                    ls->app_data_ack_cb(node, ch);
                }

                /* Decrease pending frames counter */
                DEBUG("ls-gate: decrease pending frames counter\n");
                if (node->node_class == LS_ED_CLASS_A) {
    				if (node->num_pending) {
    					node->num_pending--;
    				}
                }

                /*
                 * Process as app. data frame
                 */
    			app_data_recv(ls, ch, node, frame, aes_key);
                DEBUG("ls-gate: data processed\n");
            } else {
            	DEBUG("ls-gate: frame dropped: %d != %d\n", frame->header.fid, (uint8_t) (node->last_fid + 1));
            	return false;
            }

			return true;
    	}

        case LS_UL_ACK: {
            /* Node must be defined */
            if (!node) {
            	DEBUG("ls-gate: node undefined\n");
            	return false;
            }

            DEBUG("ls-gate: ack received\n");

            /*
             * Process acknowledge frame only if it haven't sent twice (frame ID duplicated).
             */
            if (frame->header.fid >= (uint8_t) (node->last_fid + 1)) {
            	/* Update frame ID */
                DEBUG("ls-gate: update frame ID\n");
            	node->last_fid = frame->header.fid;

                if (ls->app_data_ack_cb != NULL) {
                    ls->app_data_ack_cb(node, ch);
                }

                /* Decrease pending frames counter */
                DEBUG("ls-gate: decrease pending frames counter\n");
                if (node->node_class == LS_ED_CLASS_A) {
    				if (node->num_pending) {
    					node->num_pending--;
    				}
                }
            } else {
            	DEBUG("ls-gate: frame dropped: %d != %d\n", frame->header.fid, (uint8_t) (node->last_fid + 1));
            	return false;
            }

            return true;
        }

        case LS_UL_CONF: { /* Uplink data confirmed */
            /* Node must be defined */
            if (!node) {
            	DEBUG("ls-gate: node undefined\n");
            	return false;
            }

            DEBUG("ls-gate: uplink data confirmed\n");

            /*
             * Process received application data frame only if it wasn't sent twice (frame ID duplicated).
             * Confirmation of data reception will be sent in any case
             */
            if ((uint8_t) frame->header.fid >= (uint8_t) (node->last_fid + 1)) {
            	app_data_recv(ls, ch, node, frame, aes_key);

            	/* Update frame ID */
            	node->last_fid = frame->header.fid;
            } else {
            	DEBUG("ls-gate: frame dropped: %d != %d\n", (uint8_t) frame->header.fid, (uint8_t) (node->last_fid + 1));
            	/* As intended, send confirmation of reception even if frame is dropped */
            	/*return false;*/
            }

            /*
             * Plain ACK if there's no data pending for this node
             * Otherwise, ask upper level to give us a frame to send as acknowledge to the node
             */
            if (node->num_pending == 0) {
                DEBUG("ls-gate: ack sent to node\n");
            	send_ack(ls, ch, frame->header.dev_addr);
            } else {
                DEBUG("ls-gate: pending data requested\n");
            	if (ls->pending_frames_req)
            		ls->pending_frames_req(node);
            }

            return true;
        }

        case LS_UL_UNC: /* Uplink data unconfirmed */
            /* Node must be defined */
            if (!node)
            	return false;

            DEBUG("ls-gate: uplink data unconfirmed\n");

            app_data_recv(ls, ch, node, frame, aes_key);

            return true;

        case LS_UL_JOIN_REQ: /* Join request */
            DEBUG("ls-gate: join request received\n");
            /* Address must be undefined */
            if (frame->header.dev_addr != LS_ADDR_UNDEFINED) {
                DEBUG("ls-gate: undefined address\n");
                return false;
            }

            /* Check packet size */
            if (frame->payload.len != sizeof(ls_join_req_t)) {
                DEBUG("ls-gate: wrong payload length\n");
                return false;
            }

            if (!ls_validate_frame_mic(ls->settings.join_key, frame)) {
                DEBUG("ls-gate: MIC validation failed\n");
                return false;
            }

            ls_decrypt_frame_payload(ls->settings.join_key, &frame->payload);
            
            DEBUG("ls-gate: frame payload decrypted\n");

            ls_join_req_t req;
            memcpy(&req, &frame->payload.data, sizeof(ls_join_req_t));

            uint64_t dev_id = req.dev_id;
            uint64_t app_id = req.app_id;
            uint32_t dev_nonce = req.dev_nonce;
            ls_node_class_t node_class = req.node_class;

            device_join_req(ls, ch, dev_id, app_id, dev_nonce, node_class);
            
            DEBUG("ls-gate: join request processed\n");

            return true;

        default:
            /* Not interested in somehow received downlink frames */
            DEBUG("ls-gate: skip downlink frame\n");
            return false;
    }
}

static void sx1276_handler(void *arg, sx1276_event_type_t event_type)
{
    assert(arg != NULL);

    sx1276_t *dev = (sx1276_t *) arg;
    ls_gate_channel_t *ch = (ls_gate_channel_t *) dev->callback_arg;
    ls_gate_t *ls = (ls_gate_t *) ch->_internal.gate;

    sx1276_rx_packet_t *packet = (sx1276_rx_packet_t *) &dev->_internal.last_packet;

    switch (event_type) {
        case SX1276_RX_DONE: {
            printf("RX: %u bytes, | RSSI: %d\n", packet->size, packet->rssi_value);
            DEBUG("ls-gate: state = IDLE\n");
            ch->state = LS_GATE_CHANNEL_STATE_IDLE;
            
            ch->last_rssi = packet->rssi_value;

            /* Copy packet's data as a frame to our stack */
            ls_frame_t *frame = (ls_frame_t *) packet->content;

            /* Check frame format */
            if (ls_validate_frame(packet->content, packet->size)) {
                if (!frame_recv(ls, ch, frame)) {
                    DEBUG("ls-gate: ls-gate: well-formed frame discarded\n");
                }
            }
            else {
                DEBUG("ls-gate: ls-gate: malformed data discarded\n");
            }
        }
        break;

        case SX1276_RX_ERROR_CRC:
            DEBUG("ls-gate: CRC error\n");
            DEBUG("ls-gate: state = IDLE\n");
            ch->state = LS_GATE_CHANNEL_STATE_IDLE;
            break;

        case SX1276_TX_DONE:
            DEBUG("ls-gate: TX done\n");
            open_rx_windows(ch);

            break;

        case SX1276_RX_TIMEOUT:
            DEBUG("ls-gate: RX timeout\n");
            DEBUG("ls-gate: state = IDLE\n");
            ch->state = LS_GATE_CHANNEL_STATE_IDLE;
            break;

        case SX1276_TX_TIMEOUT:
        	DEBUG("ls-gate: TX timeout");
            prepare_sx1276(ch);
            sx1276_set_rx(ch->_internal.sx1276, ch->_internal.sx1276->settings.lora.rx_timeout);
            break;
            
        case SX1276_VALID_HEADER:
            puts("ls-gate: header received, switch to RX state");
            ch->state = LS_GATE_CHANNEL_STATE_RX;
            break;

        default:
            DEBUG("ls-gate: received event #%d\n", (int) event_type);
            break;
    }
}

/**
 * Uplink frame queue handler thread body.
 */
static void *uq_handler(void *arg)
{
    assert(arg != NULL);

    //ls_gate_t *ls = (ls_gate_t *) arg;
    msg_t msg_queue[LS_UQ_MSG_QUEUE_SIZE] = {};
    msg_init_queue(msg_queue, LS_UQ_MSG_QUEUE_SIZE);

    msg_t msg;

    puts("ls-gate: uplink frame queue handler thread started"); // XXX: debug

    while (1) {
        msg_receive(&msg);

        ls_gate_channel_t *ch = (ls_gate_channel_t *) msg.content.ptr;
        ls_frame_fifo_t *fifo = &ch->_internal.ul_fifo;

        if (ls_frame_fifo_empty(fifo)) {
            continue;
        }

        /* Get frame from queue top */
        ls_frame_t *f;
        ls_frame_t frame;
        if (!ls_frame_fifo_pop(fifo, &frame)) {
            continue;
        }

        f = &frame;

		/* Update frame's FID to the last one and advance it */
		f->header.fid = 0;

        DEBUG("ls-gate: >mhdr=0x%02X, mic=0x%04X, addr=0x%02X, type=0x%02X, fid=0x%02X [%d left]\n",
                (unsigned int) f->header.mhdr,
                (unsigned int) f->header.mic, (unsigned int) f->header.dev_addr,
                (unsigned int) f->header.type,
                (unsigned int) f->header.fid,
                ls_frame_fifo_size(fifo));

        /* Send frame into LoRa PHY */
        send_frame_f(ch, f);
    }

    return NULL;
}

static void *tim_handler(void *arg)
{
    assert(arg != NULL);

    ls_gate_t *ls = (ls_gate_t *) arg;

    msg_t queue[LS_TIM_MSG_QUEUE_SIZE] = {};
    msg_init_queue(queue, LS_TIM_MSG_QUEUE_SIZE);

    msg_t msg;

    puts("ls-gate: timeouts handler thread created");

    while (1) {
        msg_receive(&msg);

        ls_gate_tim_cmd_t cmd = (ls_gate_tim_cmd_t) msg.type;

        switch (cmd) {
            case LS_GATE_PING:
                ls->_internal.ping_count++;

				/* Kick inactive devices */
				for (int i = 0; i < LS_GATE_MAX_NODES; i++) {
					if (!ls->devices.nodes_free_list[i]) {
						ls_gate_node_t *node = &ls->devices.nodes[i];

						/* Don't kick static nodes */
						if (node->is_static)
							continue;

						int diff = ls->_internal.ping_count - node->last_seen;

						if (diff >= LS_MAX_PING_DIFFERENCE) {
							/* Kick node */
                            DEBUG("ls-gate: remove node from devlist");
							ls_devlist_remove_device(&ls->devices, i);

							/* Notify application code about kicked node */
							if (ls->node_kicked_cb != NULL) {
								ls->node_kicked_cb(node);
							}
						}
					}
				}

                /* Restart timer */
                xtimer_set_msg(&ls->_internal.ping_timer, LS_PING_TIMEOUT, &msg_ping, ls->_internal.tim_thread_pid);
                break;

            case LS_GATE_RX1_EXPIRED: {
            	ls_gate_channel_t *ch = (ls_gate_channel_t *) msg.content.ptr;
            	if (!ch)
            		break;


            	/* RX window expired, if there are frames awaiting in queue, schedule TX operation */
            	if (!ls_frame_fifo_empty(&ch->_internal.ul_fifo)) {
            		puts("ls-gate: rx1 window expired, sending next frame from queue");

            		close_rx_windows(ch);
            		schedule_tx(ch);
            	} else {
                    puts("ls-gate: state = IDLE");
            		ch->state = LS_GATE_CHANNEL_STATE_IDLE;
            		puts("ls-gate: rx1 window expired, staying in RX, but IDLE");
            	}
            }
            break;

        }
    }

    return NULL;
}

/**
 * @brief Creates timeout timers message handler thread.
 */
static bool create_tim_handler_thread(ls_gate_t *ls)
{
    puts("ls-gate: creating timeouts handler thread...");

    kernel_pid_t pid_tim = thread_create(ls->_internal.tim_thread_stack, LS_TIM_HANDLER_STACKSIZE, THREAD_PRIORITY_MAIN - 2,
                                         THREAD_CREATE_STACKTEST, tim_handler, ls,
                                         "timeouts thread");

    if (pid_tim <= KERNEL_PID_UNDEF) {
        puts("ls-gate: creation of timer handler thread failed");
        return false;
    }

    ls->_internal.tim_thread_pid = pid_tim;

    return true;
}

/**
 * @brief Creates uplink queue handler thread
 */
static bool create_uq_handler_thread(ls_gate_t *ls)
{
    puts("ls-gate: creating uplink queue handler thread...");

    kernel_pid_t pid_uq = KERNEL_PID_UNDEF;
    
    pid_uq = thread_create(ls->_internal.uq_thread_stack, sizeof(ls->_internal.uq_thread_stack), THREAD_PRIORITY_MAIN - 2,
                                         THREAD_CREATE_STACKTEST, uq_handler, ls,
                                         "uplink queue thread");
    
    if (pid_uq <= KERNEL_PID_UNDEF) {
        puts("ls-gate: creation of uplink queue handler thread failed");
        return false;
    }

    ls->_internal.uq_thread_pid = pid_uq;

    return true;
}

static bool open_channel(ls_gate_channel_t *ch)
{
    assert(ch != NULL);
    printf("ls_gate_init: opening channel %d Hz with datarate DR%d\n", (unsigned int) ch->frequency, (unsigned int) ch->dr);

    /* Initialize uplink queue */
    ls_frame_fifo_init(&ch->_internal.ul_fifo);

    sx1276_t *sx1276 = ch->_internal.sx1276;

    /* Setup callbacks */
    sx1276->sx1276_event_cb = sx1276_handler;
    sx1276->callback_arg = ch;
   
    /* Initialize the transceiver */
    sx1276_init(ch->_internal.sx1276);

    /* Initialize random number generator */
    random_init(sx1276_random(ch->_internal.sx1276));
    
    /* Initialize and configure the transceiver for this channel */
    prepare_sx1276(ch);

    /* Set channel to receive */
    sx1276_set_rx(sx1276, sx1276->settings.lora.rx_timeout);
    
    return true;
}

static bool initialize_channels(ls_gate_t *ls)
{
    for (int i = 0; i < ls->num_channels; i++) {
        ls_gate_channel_t *ch = &ls->channels[i];
        assert(ch->_internal.sx1276 != NULL);

        ch->_internal.gate = ls;
        mutex_init(&ch->_internal.channel_mutex);

        if (!open_channel(ch)) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Initializes the internal gate structures, channels, transceivers, start listening threads.
 */
int ls_gate_init(ls_gate_t *ls)
{
    assert(ls != NULL);
    assert(ls->channels != NULL);
    assert(ls->num_channels > 0);

    msg_ping.type = LS_GATE_PING;
    msg_rx1_expired.type = LS_GATE_RX1_EXPIRED;
    
    if (!create_tim_handler_thread(ls)) {
        return -LS_INIT_E_TIM_THREAD;
    }
    
    if (!create_uq_handler_thread(ls)) {
    	return -LS_INIT_E_UQ_THREAD;
    }

    /* Start ping timer */
    xtimer_set_msg(&ls->_internal.ping_timer, LS_PING_TIMEOUT, &msg_ping, ls->_internal.tim_thread_pid);
    
    ls_devlist_init(&ls->devices);
    initialize_channels(ls);

    return LS_GATE_OK;
}

/**
 * @brief Sends an answer to the node in channel assigned to the node.
 *
 */
int ls_gate_send_to(ls_gate_t *ls, ls_addr_t addr, uint8_t *buf, size_t bufsize)
{
    DEBUG("ls-gate: sending %u bytes to 0x%08X\n", (unsigned) bufsize, (unsigned) addr);

    ls_gate_node_t *node = ls_devlist_get(&ls->devices, addr);
    if (node == NULL) {
        DEBUG("ls-gate: device is not in the list, aborting\n");
        return -LS_GATE_E_NODEV;
    }  

    /* Send next data frame as ack to the previous if number of pending frames is > 0 (class A) */
    bool send_ack_with_data = node->num_pending > 0;
    enqueue_frame((ls_gate_channel_t *) node->node_ch, addr, (send_ack_with_data) ? LS_DL_ACK_W_DATA : LS_DL, buf, bufsize);
    
    DEBUG("ls-gate: data enqueued\n");

    return LS_GATE_OK;
}

int ls_gate_invite(ls_gate_t *ls, uint64_t nodeid) {
    DEBUG("ls-gate: inviting node 0x%08x%08x\n", (unsigned int) (nodeid >> 32), (unsigned int) (nodeid & 0xFFFFFFFF));
    
    ls_gate_devices_t *devs = &ls->devices;
    ls_gate_node_t *node = ls_devlist_get_by_nodeid(devs, nodeid);
    if (node != NULL) {
        DEBUG("ls-gate: remove node from the list\n");
        ls_devlist_remove_device(devs, node->addr);
	}

	/* Iterate through all channels and send invitation */
    DEBUG("ls-gate: iterate through all channels and send invitation\n");
    for (int i = 0; i < ls->num_channels; i++) {
        ls_gate_channel_t *ch = &ls->channels[i];
        assert(ch->_internal.sx1276 != NULL);

        ls_invite_t invite = { .dev_id = nodeid };
        enqueue_frame(ch, LS_ADDR_UNDEFINED, LS_DL_INVITE, (uint8_t *) &invite, sizeof(ls_invite_t));
    }
    DEBUG("ls-gate: node invited\n");
    return LS_GATE_OK;
}

/**
 * @brief Broadcasts a packet to all channels.
 */
int ls_gate_broadcast(ls_gate_t *ls, uint8_t *buf, size_t bufsize)
{
	/* Iterate through all channels and send broadcast message */
    DEBUG("ls-gate: Iterate through all channels and send broadcast message\n");
    for (int i = 0; i < ls->num_channels; i++) {
        ls_gate_channel_t *ch = &ls->channels[i];
        assert(ch->_internal.sx1276 != NULL);

        enqueue_frame(ch, LS_ADDR_UNDEFINED, LS_DL_BROADCAST, buf, bufsize);
    }

    return LS_GATE_OK;
}

/**
 * @brief Puts gate into sleep mode.
 */
void ls_gate_sleep(ls_gate_t *ls)
{
    /* Set all channel transceivers into sleep mode */
    DEBUG("ls-gate: put transceivers into sleep mode");
    for (int i = 0; i < ls->num_channels; i++) {
        sx1276_t *sx1276 = ls->channels[i]._internal.sx1276;
        sx1276_set_sleep(sx1276);
    }
}

#ifdef __cplusplus
}
#endif
