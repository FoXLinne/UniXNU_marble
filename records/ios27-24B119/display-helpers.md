# iOS 27.2 (24B119) display helper adapter

Standalone `package/ios27-display/` module providing the ten
`native_legacy_*` helpers that `ios27/sources/GopFramebuffer/NativeLegacyDisplay.cpp`
and `package/MarbleDisplay.cpp` reference but the iOS driver link was
missing (see `outputs/UniXNU_marble/ios27_24B119/logs/kernel-development.log`).

Target: iPhone12,1 iOS 27.2 `24B119` kernelcache
`kernelcache.decompressed`, sha256
`8cc8eccc429aa95397aef0ffe64b226e71b014d84190c1e3f84cf5c270a6e6bc`.

All bindings were derived from the actual 24B119 fileset. The sheng
`legacy-display-24A446.json` profile was used only as methodology; the
24A446 VAs were not transplanted (several IOSurface getter instruction
patterns happen to be byte-identical, which is recorded as cross-cache
confirmation, not as a copied offset).

## Build / integrate

```sh
python3 package/ios27-display/build.py
```

Produces `build/ios27-display/obj/libios27-display.a`
(sha256 `005796b48cbd63afc608811a5cac6a5333cfef2d6206bd95b51becc67d7ca475`)
compiled through the same `IOPlatformExpert.cpo.json` target compile record
as every other driver TU (`arm64e-apple-ios27.2`, XNU 12377 flags). The
build refuses to proceed if any original target loses its `bti c`/`pacibsp`
prologue, if any compiled stub's movz/movk/br sequence encodes a different
VA, or if an exported helper symbol is missing. The parent adds the archive
to the driver link — no integration edits were made here.

## Bindings

All originals are reached through `bti c; movz/movk x16, VA; br x16`
landing pads, identical in shape to `emit_bindings()` code stubs.

| helper | original VA | member | evidence |
|---|---|---|---|
| `surface_descriptor` | `0xfffffff009be5bf4` | IOSurface | `ldr x0,[x0,#0x30]`; swap teardown unlocks (vt+0xe0, `0x9285`) and releases (vt+0x28, `0x3a87`) the fetched object — the locked memory descriptor |
| `surface_width` | `0xfffffff009be5cf8` | IOSurface | `ldr x0,[x0,#0x58]`; validator enforces `src_x+src_width <= field` at `0x9a762f0` |
| `surface_height` | `0xfffffff009be5d04` | IOSurface | `ldr x0,[x0,#0x60]`; paired `src_y+src_height` bound at `0x9a76390` |
| `surface_stride` | `0xfffffff009be5d1c` | IOSurface | `ldr x0,[x0,#0x68]`; printed as `"src_stride"` by serializer `0x9a7bad4` |
| `surface_format` | `0xfffffff009be5ce0` | IOSurface | `ldr w0,[x0,#0x80]`; same +0x80 field the `0x9bedfd0` setter writes |
| `surface_planes` | `0xfffffff009be5d4c` | IOSurface | `ldr w0,[x0,#0x98]`; nonzero gate selects per-plane stride query |
| `surface_allocation` | — | — | `descriptor->getLength()`; no allocation getter exists on the original IOSurface (same rule as the 24A446 adapter) |
| `set_refresh_properties` | `0xfffffff009a7e3d4` | IOMobileGraphicsFamily | sole referrer of `"IOMFBDisplayRefresh"` (`0x7877127`); calls `getRefreshAttributes` slot +0x9f8 disc `0xdcfc`, `setProperty` slot +0xc8 disc `0xff53` |
| `vsync_notify` | `0xfffffff009a7a200` | IOMobileGraphicsFamily | invoked at `0x9a72cd4` only when vsync-client queue `display+0x1ab8` is non-empty, with `display+0x100/0x108/0x110` = timestamp/continuousTime/interval; iterates clients, calls vt+0x7e8 (`0x9562`) |

## Default-surface factory

`native_legacy_default_surface_create(root, width, height)` replicates the
body of `IOMobileFramebufferLegacy::create_default_fb_surface`
(`0xfffffff009a780c4`, instance vtable slot 316, diversity `0xe2fa`)
step-for-step, substituting the caller's `root` (the IOSurfaceRoot the
original reads from `display+0x1b40`) and the BGRA format `0x42475241`
the original reads from its params block at `display+0x1c40`:

1. `opts = 0xfffffff00a878000(1)` — options dict (com.apple.kernel).
2. `region = 0xfffffff00a8a9920("PurpleGfxMem")` — region lookup
   (com.apple.kernel); if found, `opts->setObject(key, region)` through
   authenticated slot +0x100, call-site diversity `0xc323`, key loaded via
   `**(0xfffffff008030f70)` exactly as the original call site at
   `0x9a78150–0x9a78188`; `region->release()` (vt+0x28, `0x3a87`).
3. `surface = 0xfffffff009bf75b8(root, 0, opts)` — the IOSurface create
   thunk (`mov w3,#0; b 0xfffffff009bf7544`), then `opts->release()`.
4. `setregion(surface, 0xfffffff009bf7bb4(root,"PurpleGfxMem"))` via
   `0xfffffff009bedd54` — the same region the original caches at
   `display+0x1d10`.
5. `0xfffffff009bedfd0(surface, 0x42475241)` (`str w1,[x0,#0x80]; ret`) —
   the same constant the original gates its format-extra path on, so that
   path is correctly skipped.
6. `0xfffffff009bedd60(surface,0)` then `0xfffffff009bedfec(surface,1)` —
   the original's two post-format calls, same arguments.
7. `0xfffffff009a74928(surface, width, height, params)` — the real
   `layout_buffers`: an 80-byte params struct `{format@0, planes@4=1,
   elems@8=4, zeros}` with flag byte `+0x48=0` selecting the original's
   own stride/allocation derivation (`0x9a77f7c`).

Failure at any step releases the partial objects and returns `nullptr`; the
helper never returns residue from the `void`-typed
`create_default_fb_surface` method.

## Vtable proof (IOMobileFramebufferLegacy, base `0xfffffff00802e740`)

| slot | entry | target | diversity | method |
|---|---|---|---|---|
| 278 | `0x802eff0` | `0x9a7a4d4` | `0x786c` | `hotPlug_notify(uint64_t)` |
| 316 | `0x802f120` | `0x9a780c4` | `0xe2fa` | `create_default_fb_surface` |
| 319 | `0x802f138` | `0x9a7e284` | `0xdcfc` | `getRefreshAttributes` |

Call-site diversities observed in original code match the stored slot
diversities (`0xdcfc` for `getRefreshAttributes`, `0x3a87` for release,
`0xc323` for dict `setObject`).

## Out of scope / unchanged

- `swap_info_notify_dispatch_gated` is macOS-only and remains pending —
  no no-op stub was added; `NATIVE_LEGACY_MACOS` stays 0 for the iOS path
  and nothing in this module enables it.
- Constructor derivation is owned by another session.
- `scripts/build_ios27_drivers.py`, prelink files, `ios27/sources`, other
  packages and the existing `package/ios27-ioav` work were not modified.
- The iOS default-surface path (`NATIVE_LEGACY_DEFAULT_SURFACE=1`) is
  fully preserved — that is precisely what `default_surface_create`
  implements.
