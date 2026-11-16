/* SPDX-License-Identifier: MIT */
#ifndef TU_BUILTIN_CACHE_H
#define TU_BUILTIN_CACHE_H
#include "tu_common.h"
VkResult tu_builtin_pack_load(struct tu_device *dev);
extern "C" VkResult tu_export_builtin_shader_pack(VkDevice device, size_t *size, void *data);
extern "C" VkResult tu_validate_builtin_shader_pack(VkDevice device, size_t size, const void *data);
#endif
