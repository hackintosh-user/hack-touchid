/*
 * synaptics_proto.h
 *
 * Wire constants for the Synaptics Prometheus 06cb:00f0 ("Prometheus 0F")
 * match-in-sensor reader. Source: PROTOCOL-06cb-00f0.md, written by
 * labrouss from a Linux usbmon capture (HP EliteBook 840 G8, firmware
 * Prometheus 10.01.3478575). Reference implementation: libfprint
 * drivers/synaptics/ (LGPL-2.1).
 *
 * No TLS, no pairing, no calibration, no firmware upload: matching and
 * template storage happen inside the sensor.
 */

#ifndef __SYNAPTICS_PROTO_H
#define __SYNAPTICS_PROTO_H

#define SYN_VID                 0x06cb
#define SYN_PID                 0x00f0

#define SYN_EP_OUT              0x01   /* bulk OUT, 64: requests */
#define SYN_EP_IN               0x81   /* bulk IN, 64: replies */
#define SYN_EP_INT              0x83   /* interrupt IN, 8: 7-byte async reports */

/* Sensor firmware commands (first byte of every bulk OUT transfer). */
#define SYN_CMD_GET_VERSION     0x01
#define SYN_CMD_ACE_COMMAND     0xA7   /* carries one BMKT message */
#define SYN_CMD_ASYNCMSG_READ   0xA8

/* Interrupt report: byte 0 bit 2 means "async message pending". */
#define SYN_INT_ASYNC_PENDING   0x04

/* BMKT message framing: FE seq msg_id payload_len payload... */
#define SYN_BMKT_MAGIC          0xFE

/* BMKT message ids seen in the capture. */
#define SYN_MSG_FPS_INIT            0x11
#define SYN_MSG_FPS_INIT_FAIL       0x12   /* payload 00 67 = already initialised */
#define SYN_MSG_FPS_INIT_OK         0x13
#define SYN_MSG_CANCEL_OP           0x41   /* from libfprint, not captured yet */
#define SYN_MSG_ENROLL_USER         0x51
#define SYN_MSG_ENROLL_READY        0x54
#define SYN_MSG_ENROLL_REPORT       0x55
#define SYN_MSG_ENROLL_OK           0x59
#define SYN_MSG_CAPTURE_COMPLETE    0x60
#define SYN_MSG_ID_USER             0x61   /* from libfprint, not captured yet */
#define SYN_MSG_VERIFY_USER         0x65
#define SYN_MSG_VERIFY_READY        0x66
#define SYN_MSG_VERIFY_FAIL         0x67   /* payload 00 d5 = bad capture, retry */
#define SYN_MSG_VERIFY_OK           0x68
#define SYN_MSG_GET_TEMPLATE_RECORDS 0x71  /* from libfprint, not captured yet */
#define SYN_MSG_GET_NEXT_QUERY_RESP 0x72   /* from libfprint, not captured yet */
#define SYN_MSG_DEL_USER_FP         0x81
#define SYN_MSG_DEL_USER_FP_OK      0x83
#define SYN_MSG_DEL_FULL_DB         0x84   /* NEVER SENT, see syn_send_bmkt() */
#define SYN_MSG_DEL_FULL_DB_OK      0x86
#define SYN_MSG_DELETE_PROGRESS     0x87
#define SYN_MSG_EVT_FINGER_REPORT   0x91   /* 01 finger on, 00 finger off */
#define SYN_MSG_POWER_DOWN_NOTIFY   0xA1

#endif /* __SYNAPTICS_PROTO_H */
