/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MARBLE_BLUETOOTH_FIRMWARE_H
#define MARBLE_BLUETOOTH_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
/* stage-bluetooth-firmware.py consumes this exact addon-relative prefix. */
#define MARBLE_BT_FIRMWARE_PREFIX "firmware/qca"
#define MARBLE_BT_FIRMWARE_ROOT "/private/var/ngpu/" MARBLE_BT_FIRMWARE_PREFIX
#ifdef __cplusplus
extern "C" {
#endif
int marble_bt_firmware_read(const char *name, uint8_t **data, size_t *size);
void marble_bt_firmware_release(uint8_t *data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
