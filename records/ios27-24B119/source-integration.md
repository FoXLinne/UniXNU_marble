# iOS 27.2 (24B119) source integration

generated: 2026-11-16T09:17:27.585675+08:00
status: **failed**

| component | base | new | files | conflicts | manual | local-only | added |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AdrenoMetal | `73d582fd` | `9f6a887a` | 115 | 0 | 0 | 6 | 0 |
| metal2vulkan | `400efe79` | `244fa022` | 7559 | 6 | 0 | 4 | 73 |

## semantic gaps

- `metal2vulkan` crates/metal2vulkan-runtime/src/buffer_texture.rs: text (1 hunks)
- `metal2vulkan` crates/metal2vulkan-runtime/src/gpu_time.rs: text (1 hunks)
- `metal2vulkan` crates/metal2vulkan-runtime/src/shader_cache/store.rs: text (6 hunks)
- `metal2vulkan` crates/metal2vulkan-runtime/src/texture/heap_two_planes.rs: text (1 hunks)
- `metal2vulkan` crates/metal2vulkan-runtime/src/texture/ycbcr_image.rs: text (1 hunks)
- `metal2vulkan` crates/metal2vulkan/src/cache/mod.rs: text (1 hunks)

## pin changes

- AdrenoMetal: `c1351337`/`73d582fd` -> `73d582fd`/`9f6a887a`
- metal2vulkan: `7ef17497`/`400efe79` -> `400efe79`/`244fa022`
