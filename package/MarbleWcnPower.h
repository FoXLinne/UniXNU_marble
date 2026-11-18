// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { MARBLE_WCN_WLAN=0, MARBLE_WCN_BLUETOOTH=1 };
int marble_wcn_power_get(unsigned);
int marble_wcn_power_put(unsigned);
unsigned marble_wcn_power_owners(void);
#ifdef __cplusplus
}
#endif
