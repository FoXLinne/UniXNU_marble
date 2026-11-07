/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MARBLE_RTC_H
#define MARBLE_RTC_H

/* Two cache lines give the firmware and kernel separate write ownership. */
#define MARBLE_RTC_MAGIC 0x314354524c42524dULL
#define MARBLE_RTC_OFFSET 2048
#define MARBLE_RTC_CAL_OFFSET 1024
#define MARBLE_RTC_CAL_MAGIC 0x314c41435452424dULL
typedef struct {
    unsigned long long magic;
    unsigned sequence, error;
    unsigned long long seconds, counter, frequency;
    unsigned nanoseconds, completed;
    unsigned long long reserved[2];
    unsigned requested, request_nanoseconds;
    unsigned long long request_seconds, request_counter;
    unsigned long long request_reserved[5];
} MARBLE_RTC;

#endif
