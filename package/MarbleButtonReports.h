// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
struct MarbleButtonReport {
    MarbleButtonReport *next = nullptr;
    uint64_t timestamp = 0;
    uint32_t mask = 0;
};
// The producer and consumer hold their enclosing queue lock.
struct MarbleButtonReports {
    MarbleButtonReport *head = nullptr, *tail = nullptr;
    unsigned count = 0;
    uint32_t delivered = 0;
    bool deliveredOnce = false;
    bool append(MarbleButtonReport *report) {
        report->mask &= 0xe;
        if ((tail && tail->mask == report->mask) ||
            (!tail && deliveredOnce && delivered == report->mask)) return false;
        report->next = nullptr;
        if (tail) tail->next = report;
        else head = report;
        tail = report; ++count;
        return true;
    }
    MarbleButtonReport *completed() {
        auto *report = head;
        if (!report) return nullptr;
        head = report->next;
        if (!head) tail = nullptr;
        --count; delivered = report->mask; deliveredOnce = true;
        return report;
    }
};
