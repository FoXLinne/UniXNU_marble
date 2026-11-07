/* SPDX-License-Identifier: MIT */
/* Driver-owned, build-time embedded builtin variants. This is not a general
 * untrusted shader import API. Pack generation must use this exact compiler
 * source/profile and GPU, and the final driver must carry the normal owner
 * signature. No disk I/O, cache thread, fake completion or shader execution. */
#include "tu_builtin_cache.h"
#include "tu_device.h"
#include "tu_shader.h"
#include "tu_cs.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"
#include "util/blob.h"
#include "util/mesa-blake3.h"

#ifdef TU_BUILTIN_PACK_HEADER
#include TU_BUILTIN_PACK_HEADER
#endif

static constexpr unsigned variant_count = GLOBAL_SH_COUNT + 2;
static constexpr size_t max_pack_size = 4 * 1024 * 1024;
static constexpr size_t max_variant_size = 256 * 1024;

static void
identity(struct tu_device *dev, struct blob *out)
{
   blob_write_bytes(out, "TUBUILT1", 8);
   blob_write_uint32(out, 1);
   blob_write_uint32(out, variant_count);
   blob_write_bytes(out, TU_BUILTIN_SOURCE_ID, strlen(TU_BUILTIN_SOURCE_ID));
   blob_write_uint32(out, dev->physical_device->dev_id.gpu_id);
   blob_write_uint64(out, dev->physical_device->dev_id.chip_id);
   blob_write_uint64(out, ir3_shader_debug_hash_key());
   blob_write_uint32(out, sizeof(struct ir3_shader_variant));
   blob_write_uint32(out, VARIANT_CACHE_SIZE);
   blob_write_uint32(out, sizeof(struct ir3_const_state));
   blob_write_uint32(out, sizeof(struct ir3_shader_key));
   const struct ir3_compiler_options *o = &dev->compiler->options;
   uint32_t flags = o->push_ubo_with_preamble | (o->disable_cache << 1) |
      (o->storage_16bit << 2) | (o->storage_8bit << 3) |
      (o->lower_base_vertex << 4) | (o->shared_push_consts << 5) |
      (o->dual_color_blend_by_location << 6);
   blob_write_uint32(out, flags);
   blob_write_uint32(out, o->bindless_fb_read_descriptor);
   blob_write_uint32(out, o->bindless_fb_read_slot);
   blob_write_uint64(out, o->uche_trap_base);
}

static bool
available(const struct blob_reader *in, size_t bytes)
{
   return !in->overrun && bytes <= (size_t)(in->end - in->current);
}

/* Read only after bounding the record and every allocation. Unlike the
 * general IR3 disk-cache reader, failure is checked before dereferencing an
 * allocation, and no allocation size is taken from an unchecked short blob. */
static struct ir3_shader_variant *
read_variant(struct blob_reader *in, struct ir3_compiler *compiler, void *owner)
{
   if (!available(in, sizeof(struct ir3_shader_key) + 8))
      return NULL;
   struct ir3_shader_key key;
   blob_copy_bytes(in, &key, sizeof(key));
   const struct ir3_shader_key zero_key = {};
   if (memcmp(&key, &zero_key, sizeof(key)))
      return NULL;
   const uint32_t stage = blob_read_uint32(in);
   const uint32_t mergedregs = blob_read_uint32(in);
   if ((stage != MESA_SHADER_VERTEX && stage != MESA_SHADER_FRAGMENT) || mergedregs > 1)
      return NULL;
   struct ir3_shader_variant *v = rzalloc(owner, struct ir3_shader_variant);
   if (!v)
      return NULL;
   v->compiler = compiler;
   v->key = key;
   v->type = (mesa_shader_stage) stage;
   v->mergedregs = mergedregs;
   v->const_state = rzalloc(v, struct ir3_const_state);
   if (!v->const_state)
      return NULL;
   unsigned count = stage == MESA_SHADER_VERTEX && ir3_has_binning_vs(&key) ? 2 : 1;
   for (unsigned i = 0; i < count; ++i) {
      struct ir3_shader_variant *part = v;
      if (i) {
         part = rzalloc(v, struct ir3_shader_variant);
         if (!part)
            return NULL;
         v->binning = part;
         part->compiler = compiler;
         part->key = key;
         part->type = MESA_SHADER_VERTEX;
         part->mergedregs = mergedregs;
         part->binning_pass = true;
         part->nonbinning = v;
         part->const_state = v->const_state;
      }
      if (!available(in, VARIANT_CACHE_SIZE))
         return NULL;
      blob_copy_bytes(in, VARIANT_CACHE_PTR(part), VARIANT_CACHE_SIZE);
      const size_t size = part->info.size;
      if (!size || size > max_variant_size || size % 4 ||
          part->info.sizedwords * sizeof(uint32_t) > size ||
          part->info.constant_data_offset > size || !available(in, size))
         return NULL;
      part->bin = (uint32_t *) ralloc_size(part, size);
      if (!part->bin)
         return NULL;
      blob_copy_bytes(in, part->bin, size);
      if (!i) {
         if (!available(in, sizeof(*part->const_state)))
            return NULL;
         blob_copy_bytes(in, part->const_state, sizeof(*part->const_state));
      }
      if (!compiler->info->props.load_shader_consts_via_preamble) {
         if (!available(in, 4))
            return NULL;
         unsigned n = blob_read_uint32(in);
         if (n > max_variant_size / 4 || !available(in, (size_t)n * 4))
            return NULL;
         part->imm_state.size = part->imm_state.count = n;
         if (n) {
            part->imm_state.values = (uint32_t *) ralloc_size(part, (size_t)n * 4);
            if (!part->imm_state.values)
               return NULL;
            blob_copy_bytes(in, part->imm_state.values, (size_t)n * 4);
         }
      }
   }
   return v;
}

static bool
decode(struct tu_device *dev, const void *data, size_t size, void *owner,
       struct ir3_shader_variant **variants)
{
   if (strlen(TU_BUILTIN_SOURCE_ID) != 64 || !data || size > max_pack_size || size < 32)
      return false;
   uint8_t digest[BLAKE3_OUT_LEN];
   _mesa_blake3_compute(data, size - sizeof(digest), digest);
   if (memcmp(digest, (const uint8_t *)data + size - sizeof(digest), sizeof(digest)))
      return false;
   struct blob header;
   blob_init(&header);
   identity(dev, &header);
   const bool matches = !header.out_of_memory && header.size <= size - sizeof(digest) &&
                        !memcmp(header.data, data, header.size);
   const size_t prefix = header.size;
   blob_finish(&header);
   if (!matches)
      return false;
   struct blob_reader in;
   blob_reader_init(&in, (const uint8_t *)data + prefix, size - prefix - sizeof(digest));
   for (unsigned i = 0; i < variant_count; ++i) {
      if (!available(&in, 8) || blob_read_uint32(&in) != i)
         return false;
      const size_t length = blob_read_uint32(&in);
      if (!length || length > max_variant_size || !available(&in, length))
         return false;
      struct blob_reader entry;
      blob_reader_init(&entry, in.current, length);
      variants[i] = read_variant(&entry, dev->compiler, owner);
      if (!variants[i] || entry.overrun || entry.current != entry.end ||
          variants[i]->type != (i < 3 ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT))
         return false;
      in.current += length;
   }
   uint64_t words = 0;
   for (unsigned i = 0; i < GLOBAL_SH_COUNT; ++i)
      words += align(variants[i]->info.sizedwords, 32);
   return words <= ARRAY_SIZE(((struct tu6_global *)0)->shaders) &&
          !in.overrun && in.current == in.end;
}

VkResult
tu_builtin_pack_load(struct tu_device *dev)
{
#ifdef TU_BUILTIN_PACK_HEADER
   void *owner = ralloc_context(NULL);
   if (!owner)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!decode(dev, tu_builtin_pack_bytes, sizeof(tu_builtin_pack_bytes), owner, dev->builtin_variants)) {
      ralloc_free(owner);
      memset(dev->builtin_variants, 0, sizeof(dev->builtin_variants));
      /* A requested pack is mandatory: no silent runtime compiler fallback. */
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   dev->builtin_cache_owner = owner;
#endif
   return VK_SUCCESS;
}

/* Explicit offline tool interfaces. They never load a pack into a running
 * device, replace a shader, submit commands, or change Vulkan feature reports. */
extern "C" __attribute__((visibility("default"))) VkResult
tu_export_builtin_shader_pack(VkDevice handle, size_t *size, void *data)
{
   VK_FROM_HANDLE(tu_device, dev, handle);
   if (!dev || !size || strlen(TU_BUILTIN_SOURCE_ID) != 64)
      return VK_ERROR_INITIALIZATION_FAILED;
   struct blob out;
   blob_init(&out);
   identity(dev, &out);
   for (unsigned i = 0; i < variant_count; ++i) {
      const struct ir3_shader_variant *v = i < GLOBAL_SH_COUNT ? dev->global_shader_variants[i] :
         (i == GLOBAL_SH_COUNT ? dev->empty_fs->variant : dev->empty_fs_fdm->variant);
      if (!v) {
         blob_finish(&out);
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      struct blob entry;
      blob_init(&entry);
      ir3_store_variant(&entry, v);
      blob_write_uint32(&out, i);
      blob_write_uint32(&out, entry.size);
      blob_write_bytes(&out, entry.data, entry.size);
      bool failed = entry.out_of_memory;
      blob_finish(&entry);
      if (failed) {
         blob_finish(&out);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }
   if (out.out_of_memory || out.size > max_pack_size - BLAKE3_OUT_LEN) {
      blob_finish(&out);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   uint8_t digest[BLAKE3_OUT_LEN];
   _mesa_blake3_compute(out.data, out.size, digest);
   blob_write_bytes(&out, digest, sizeof(digest));
   if (out.out_of_memory || out.size > max_pack_size) {
      blob_finish(&out);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   VkResult result = VK_SUCCESS;
   if (data) {
      if (*size < out.size)
         result = VK_INCOMPLETE;
      else
         memcpy(data, out.data, out.size);
   }
   *size = out.size;
   blob_finish(&out);
   return result;
}

extern "C" __attribute__((visibility("default"))) VkResult
tu_validate_builtin_shader_pack(VkDevice handle, size_t size, const void *data)
{
   VK_FROM_HANDLE(tu_device, dev, handle);
   if (!dev)
      return VK_ERROR_INITIALIZATION_FAILED;
   void *owner = ralloc_context(NULL);
   if (!owner)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct ir3_shader_variant *variants[variant_count] = {};
   bool valid = decode(dev, data, size, owner, variants);
   ralloc_free(owner);
   return valid ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}
