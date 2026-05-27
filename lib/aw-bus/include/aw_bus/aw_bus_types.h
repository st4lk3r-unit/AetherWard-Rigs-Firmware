#pragma once
#include <stdint.h>

#define AWBUS_MAGIC_0  0x41u   /* 'A' */
#define AWBUS_MAGIC_1  0x57u   /* 'W' */

/* Total SPI transaction size in bytes (header + payload).
   Both brain and companion must be compiled with the same value.
   Override with -DAWBUS_FRAME_SIZE=N in build flags. */
#ifndef AWBUS_FRAME_SIZE
#define AWBUS_FRAME_SIZE  260u
#endif

#define AWBUS_HEADER_SIZE  8u
#define AWBUS_MAX_PAYLOAD  (AWBUS_FRAME_SIZE - AWBUS_HEADER_SIZE)

/* Wire layout (AWBUS_FRAME_SIZE bytes total):
 *  [0-1]  magic       : AWBUS_MAGIC_0, AWBUS_MAGIC_1
 *  [2]    node_id     : target/source companion (0x00 = broadcast / brain)
 *  [3]    cmd         : awbus_cmd_t
 *  [4-5]  payload_len : uint16_t little-endian, <= AWBUS_MAX_PAYLOAD
 *  [6-7]  crc16       : CRC16/CCITT-FALSE over bytes [0..5] + payload bytes
 *  [8..]  payload     : `payload_len` data bytes, rest zero-padded to AWBUS_FRAME_SIZE
 */

typedef enum {
    AWBUS_CMD_NULL       = 0x00,  /* empty / keep-alive frame   */
    AWBUS_CMD_PING       = 0x01,  /* brain → companion          */
    AWBUS_CMD_PONG       = 0x02,  /* companion → brain          */
    AWBUS_CMD_RESET      = 0x03,  /* brain → companion: reboot  */
    AWBUS_CMD_STATUS_REQ = 0x10,  /* brain → companion          */
    AWBUS_CMD_STATUS_RSP = 0x11,  /* companion → brain          */
    AWBUS_CMD_INFO_REQ   = 0x12,  /* brain → companion: rig info request */
    AWBUS_CMD_INFO_RSP   = 0x13,  /* companion → brain: rig info reply   */
    AWBUS_CMD_DATA_PUSH  = 0x20,  /* companion → brain: event   */
    AWBUS_CMD_ACTION     = 0x30,  /* brain → companion: command */
} awbus_cmd_t;

typedef enum {
    AWBUS_OK            =  0,
    AWBUS_ERR_TIMEOUT   = -1,
    AWBUS_ERR_CRC       = -2,
    AWBUS_ERR_MAGIC     = -3,
    AWBUS_ERR_OVERFLOW  = -4,
    AWBUS_ERR_IO        = -5,
    AWBUS_ERR_NODATA    = -6,
} awbus_err_t;

/* In-memory frame (not packed — use awbus_serialize/awbus_deserialize for wire) */
typedef struct {
    uint8_t  node_id;
    uint8_t  cmd;
    uint16_t payload_len;
    uint8_t  payload[AWBUS_MAX_PAYLOAD];
} awbus_frame_t;
