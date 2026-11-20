// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t known,firmware_owned,budget_ua,mode;
} MarbleChargeSourcePolicy;

/* UCSI connector flags: operation mode bits 18:16, connected bit19,
 * power-source direction bit20. Type-C modes4/5 advertise1.5A/3A.
 * PD contracts, disconnected ports and source-role ports stay firmware-owned. */
static inline MarbleChargeSourcePolicy marble_charge_source_policy(
    uint32_t valid,uint32_t status,uint32_t configured_usb_ua)
{
    MarbleChargeSourcePolicy p={valid?1u:0u,0,0,(status>>16)&7u};
    if(!valid)return p;
    if(!(status&(1u<<19)) || (status&(1u<<20)) || p.mode==3 || p.mode==0 || p.mode>5) {
        p.firmware_owned=1;
    } else if(p.mode==4)p.budget_ua=1500000;
    else if(p.mode==5)p.budget_ua=3000000;
    else if(p.mode==1)p.budget_ua=configured_usb_ua;
    /* BC1.2 mode2 uses the battery firmware's SDP/CDP adapter classification. */
    return p;
}
