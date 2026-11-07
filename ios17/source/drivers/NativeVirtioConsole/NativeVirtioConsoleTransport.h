// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generic exclusive-owner kernel byte transport. No message/GPU semantics.
#pragma once
#include <IOKit/IOService.h>
extern "C" IOReturn native_console_send(IOService *,IOService *,const void *,size_t);
extern "C" IOReturn native_console_receive(IOService *,IOService *,void *,size_t *);
