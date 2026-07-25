#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

#define MAX_KEY_LEN          64
#define MAX_VALUE_LEN        256
#define MAX_PATH_LEN         256
#define MAX_ID_LEN           64
#define MAX_VERSION_STR      32
#define SHA256_HEX_LEN       64
#define SHA256_HEX_BUF       (SHA256_HEX_LEN + 1)

#define FRAME_MAGIC0         0x55
#define FRAME_MAGIC1         0x50
#define FRAME_MAGIC2         0x44
#define FRAME_MAGIC3         0x54
#define FRAME_HEADER_SIZE    12
#define PROTO_VERSION        1

#define DEFAULT_CHUNK_SIZE   65536
#define MIN_CHUNK_SIZE       4096
#define MAX_CHUNK_SIZE       (1 << 20)
#define DEFAULT_PACKET_DELAY_US 40000
#define MAX_CONTROL_PAYLOAD  1024

#define LOG_LINE_MAX         512
#define GUI_LOG_RING         64
#define LOG_QUEUE_CAP        4096
#define CONFIG_MAX_ENTRIES   256
#define CLIENT_INFO_LEN      128

#endif
