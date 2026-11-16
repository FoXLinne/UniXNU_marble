// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_ACTIVITY_STREAM_24B119_H
#define MARBLE_ACTIVITY_STREAM_24B119_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>
/* 24B119 LoggingSupport, UUID-matched SystemOS cache:
 * _os_activity_stream_entry_decode 0x1c355b3c8 writes uid at0x10,
 * unaligned imageUUID at0x14, imagePath at0x1c, activity at0x24,
 * parent at0x2c and the message union at0x34, total0xec bytes.
 * os_log_copy_formatted_message 0x1c3559cb4 reads format at message+0x58.
 * The 2017 debugserver entry layout lacks uid and uses an older message.
 */
struct os_activity_stream_entry_s {
    uint32_t type;
    int32_t pid;
    uint64_t proc_id;
    uint32_t uid;
    const uint8_t *proc_imageuuid;
    const char *proc_imagepath;
    uint64_t activity_id, parent_id;
    uint8_t log_message[0xb8];
} __attribute__((packed));
_Static_assert(offsetof(struct os_activity_stream_entry_s, proc_imagepath) == 0x1c, "24B119 process path");
_Static_assert(offsetof(struct os_activity_stream_entry_s, log_message) == 0x34, "24B119 message union");
_Static_assert(sizeof(struct os_activity_stream_entry_s) == 0xec, "24B119 decoded entry");
typedef struct os_activity_stream_entry_s *os_activity_stream_entry_t;
typedef struct os_activity_stream_s *os_activity_stream_t;
typedef uint32_t os_activity_stream_event_t;
enum { OS_ACTIVITY_STREAM_HISTORICAL=8, OS_ACTIVITY_STREAM_NO_SENSITIVE=0x80,
       OS_ACTIVITY_STREAM_INFO=0x100, OS_ACTIVITY_STREAM_DEBUG=0x200, OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE=0x400,
       OS_ACTIVITY_STREAM_EVENT_STOPPED=2, OS_ACTIVITY_STREAM_EVENT_FAILED=3 };
typedef bool (^os_activity_stream_block_t)(os_activity_stream_entry_t, int);
typedef void (^os_activity_stream_event_block_t)(os_activity_stream_t, os_activity_stream_event_t);
typedef os_activity_stream_t (*os_activity_stream_for_pid_t)(pid_t, uint32_t, os_activity_stream_block_t);
typedef void (*os_activity_stream_resume_t)(os_activity_stream_t);
typedef void (*os_activity_stream_cancel_t)(os_activity_stream_t);
typedef void (*os_activity_stream_set_event_handler_t)(os_activity_stream_t, os_activity_stream_event_block_t);
typedef char *(*os_log_copy_formatted_message_t)(void *);
#endif
