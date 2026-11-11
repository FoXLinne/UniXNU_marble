// SPDX-License-Identifier: BSD-2-Clause-Patent
// GPU addon installation uses the UniXNU NGPUADD2 format and preserves Data identity.
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/sysproto.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
int native_marble_metal_prepare(proc_t, user_addr_t);
boolean_t native_marble_haptics_vnode(vfs_context_t, vnode_t, struct componentname *);
boolean_t native_marble_haptics_vnode(vfs_context_t context, vnode_t vnode, struct componentname *component)
{
    uint32_t enabled = 0;
    PE_parse_boot_argn("marble-haptics", &enabled, sizeof(enabled));
    proc_t process = vfs_context_proc(context);
    if (enabled < 2 || !process || strcmp(process->p_name, "audiomxd")) return FALSE;
    char path[MAXPATHLEN];
    int length = sizeof(path);
    if (vn_getpath(vnode, path, &length)) return FALSE;
    if (component)
        return vnode_vtype(vnode) == VDIR && !strcmp(path, "/dev") &&
            ((component->cn_namelen == sizeof("marble-haptics") - 1 &&
              !memcmp(component->cn_nameptr, "marble-haptics", sizeof("marble-haptics") - 1)) ||
             (component->cn_namelen == sizeof("marble-speaker") - 1 &&
              !memcmp(component->cn_nameptr, "marble-speaker", sizeof("marble-speaker") - 1)));
    return vnode_vtype(vnode) == VCHR &&
        (!strcmp(path, "/dev/marble-haptics") || !strcmp(path, "/dev/marble-speaker"));
}
volatile int native_bindfs_boot_mount;
volatile int native_gpu_addon_stage = 0;
volatile int native_gpu_addon_status = -1;
volatile int native_gpu_addon2_stage = 0;
volatile int native_gpu_addon2_status = -1;
volatile int native_gpu_addon3_stage = 0;
volatile int native_gpu_addon3_status = -1;
volatile int marble_haptics_overlay_stage[2];
volatile int marble_haptics_overlay_status[2];
static void
native_gpu_addon_mount(proc_t p, user_addr_t scratch, user_addr_t offset, const char *source, size_t source_size,
    const char *point, size_t point_size, const char *visible, const char *own, volatile int *stage, volatile int *status)
{
	vnode_t probe = NULLVP;
	int error;
	*stage = 1;
	error = vnode_lookup(source, 0, &probe, vfs_context_current());
	if (error) {
		*status = error;
		return;
	}
	vnode_put(probe);
	probe = NULLVP;
	*stage = 2;
	struct mount_args request = {.type = scratch + offset, .path = scratch + offset + 0x20,
	    .flags = MNT_RDONLY | MNT_UNION, .data = scratch + offset + 0x100};
	int32_t result = 0;
	if ((error = copyout("bindfs", request.type, 7)) || (error = copyout(point, request.path, point_size)) ||
	    (error = copyout(source, request.data, source_size))) {
		*status = error;
		return;
	}
	native_bindfs_boot_mount = 1;
	error = mount(p, &request, &result);
	native_bindfs_boot_mount = 0;
	if (error) {
		*status = error;
		return;
	}
	*stage = 3;
	/* The union must still show a sealed lower entry next to the owner's. */
	error = vnode_lookup(visible, 0, &probe, vfs_context_current());
	if (!error) {
		vnode_put(probe);
		probe = NULLVP;
		*stage = 4;
		/* ... and the owner's own entry must be visible through it (stage 5). */
		error = vnode_lookup(own, 0, &probe, vfs_context_current());
		if (!error) {
			vnode_put(probe);
			probe = NULLVP;
			*stage = 5;
		}
	}
	*status = error;
}
/* The GPU addon of the boot payload (scripts/make-gpu-addon.py, handed over by the loader as /chosen/memory-map/GpuAddon): every file
 * whose content or mode differs from the mounted Data volume is written there before the addon mounts, so the plugin no longer has to
 * be part of the Data seed and the seed's persistent store survives a plugin rebuild. Missing directories below /private/var/ngpu are
 * created. A malformed addon is refused as a whole; a file that cannot be written keeps its old content.
 * Counters for the boot log: files in the addon (-1 none), files written, files already equal, failures and the last error. */
extern int native_boot_gpu_addon(const uint8_t **, uint64_t *);
#include <libkern/zlib.h>
/* zlib's state, from kalloc with its size in front (the default allocator of the kernel's zlib is not meant for this). */
static voidpf
native_gpu_addon_zalloc(voidpf opaque __unused, uInt items, uInt size)
{
	size_t bytes = (size_t)items * size + 16;
	uint8_t *block = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
	if (!block) {
		return Z_NULL;
	}
	memcpy(block, &bytes, sizeof(bytes));
	return block + 16;
}
static void
native_gpu_addon_zfree(voidpf opaque __unused, voidpf address)
{
	uint8_t *block = (uint8_t *)address - 16;
	size_t bytes = 0;
	memcpy(&bytes, block, sizeof(bytes));
	kfree_data(block, bytes);
}
static int
native_gpu_addon_inflate(const uint8_t *packed, uint64_t packed_size, uint8_t *data, uint64_t size)
{
	z_stream stream;
	memset(&stream, 0, sizeof(stream));
	stream.zalloc = native_gpu_addon_zalloc;
	stream.zfree = native_gpu_addon_zfree;
	stream.next_in = (Bytef *)(uintptr_t)packed;
	stream.avail_in = (uInt)packed_size;
	stream.next_out = data;
	stream.avail_out = (uInt)size;
	if (inflateInit(&stream) != Z_OK) {
		return ENOMEM;
	}
	int result = inflate(&stream, Z_FINISH);
	inflateEnd(&stream);
	return result == Z_STREAM_END && stream.total_out == size && !stream.avail_in ? 0 : EINVAL;
}
volatile int native_gpu_addon_files = -1;
volatile int native_gpu_addon_written = 0;
volatile int native_gpu_addon_same = 0;
volatile int native_gpu_addon_failed = 0;
volatile int native_gpu_addon_error = 0;
static int
native_gpu_addon_equal(vnode_t vp, const uint8_t *data, uint64_t size, uint32_t mode, vfs_context_t ctx)
{
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_mode);
	if (vnode_getattr(vp, &va, ctx) || va.va_data_size != size || (va.va_mode & 07777) != mode) {
		return 0;
	}
	const uint32_t chunk = 1024 * 1024;
	uint8_t *buffer = kalloc_data(chunk, Z_WAITOK);
	if (!buffer) {
		return 0;
	}
	int equal = 1;
	for (uint64_t at = 0; equal && at < size; at += chunk) {
		int length = (int)(size - at < chunk ? size - at : chunk), resid = 0;
		if (vn_rdwr(UIO_READ, vp, (caddr_t)buffer, length, (off_t)at, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx)) || resid || memcmp(buffer, data + at, length)) {
			equal = 0;
		}
	}
	kfree_data(buffer, chunk);
	return equal;
}
static int
native_gpu_addon_write(const char *path, const uint8_t *data, uint64_t size, uint32_t mode, vfs_context_t ctx)
{
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
	if (!error) {
		int equal = native_gpu_addon_equal(vp, data, size, mode, ctx);
		vnode_close(vp, FREAD, ctx);
		vp = NULLVP;
		if (equal) {
			return -1;
		}
	}
	error = vnode_open(path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW, mode, 0, &vp, ctx);
	if (error) {
		return error;
	}
	int resid = 0;
	error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)data, (int)size, 0, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx),
	    &resid, vfs_context_proc(ctx));
	if (!error && resid) {
		error = EIO;
	}
	if (!error) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_SET(&va, va_mode, mode);
		VATTR_SET(&va, va_data_size, size);
		error = vnode_setattr(vp, &va, ctx);
	}
	if (!error) {
		error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
	}
	int close_error = vnode_close(vp, FWRITE, ctx);
	return error ? error : close_error;
}
/* Creates the missing parent directories of an addon path with the mkdir syscall, /private/var/ngpu itself included (a Data seed
 * that never had add-gpu-plugin-data.py applied has none). The scratch is one 4 KiB page; the mount arguments end below 0xa00. */
static int
native_gpu_addon_parents(proc_t p, user_addr_t scratch, const char *path)
{
	char parent[192];
	strlcpy(parent, path, sizeof(parent));
	for (size_t i = sizeof("/private/var/ngpu") - 1; parent[i]; ++i) {
		if (parent[i] != '/') {
			continue;
		}
		parent[i] = 0;
		struct mkdir_args request = {.path = scratch + 0xc00, .mode = 0755};
		int32_t result = 0;
		int error = copyout(parent, request.path, i + 1);
		if (!error) {
			error = mkdir(p, &request, &result);
		}
		parent[i] = '/';
		if (error && error != EEXIST) {
			return error;
		}
	}
	return 0;
}
static void
native_gpu_addon_install(proc_t p, user_addr_t scratch)
{
	const uint8_t *addon = NULL;
	uint64_t bytes = 0;
	if (!native_boot_gpu_addon(&addon, &bytes)) {
		return;
	}
	uint32_t count = 0;
	uint64_t total = 0;
	if (bytes < 24 || memcmp(addon, "NGPUADD2", 8)) {
		native_gpu_addon_error = EINVAL;
		return;
	}
	memcpy(&count, addon + 8, 4);
	memcpy(&total, addon + 16, 8);
	if (total != bytes || !count || count > 4096) {
		native_gpu_addon_error = EINVAL;
		return;
	}
	/* Validate the whole addon before writing anything. */
	uint64_t at = 24;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t name_bytes = 0, mode = 0;
		uint64_t size = 0, packed = 0;
		if (bytes - at < 24) {
			native_gpu_addon_error = EINVAL;
			return;
		}
		memcpy(&name_bytes, addon + at, 4);
		memcpy(&mode, addon + at + 4, 4);
		memcpy(&size, addon + at + 8, 8);
		memcpy(&packed, addon + at + 16, 8);
		at += 24;
		uint64_t name_padded = (name_bytes + 7) & ~7ULL, size_padded = (packed + 7) & ~7ULL;
		const char *name = (const char *)(addon + at);
		if (name_bytes < 2 || name_bytes > 160 || bytes - at < name_padded || name[name_bytes - 1] || strnlen(name, name_bytes) != name_bytes - 1 ||
		    name[0] == '/' || (mode & ~0777) || size > INT32_MAX || !packed || packed > INT32_MAX || bytes - at - name_padded < size_padded) {
			native_gpu_addon_error = EINVAL;
			return;
		}
		for (uint32_t k = 0; k + 1 < name_bytes - 1; ++k) {
			if (name[k] == '.' && name[k + 1] == '.') {
				native_gpu_addon_error = EINVAL;
				return;
			}
		}
		at += name_padded + size_padded;
	}
	if (at != bytes) {
		native_gpu_addon_error = EINVAL;
		return;
	}
	native_gpu_addon_files = (int)count;
	vfs_context_t ctx = vfs_context_current();
	char path[192];
	at = 24;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t name_bytes = 0, mode = 0;
		uint64_t size = 0, packed = 0;
		memcpy(&name_bytes, addon + at, 4);
		memcpy(&mode, addon + at + 4, 4);
		memcpy(&size, addon + at + 8, 8);
		memcpy(&packed, addon + at + 16, 8);
		at += 24;
		const char *name = (const char *)(addon + at);
		at += (name_bytes + 7) & ~7ULL;
		snprintf(path, sizeof(path), "/private/var/ngpu/%s", name);
		uint8_t *data = kalloc_data(size ? size : 1, Z_WAITOK);
		int error = data ? native_gpu_addon_inflate(addon + at, packed, data, size) : ENOMEM;
		if (!error) {
			error = native_gpu_addon_write(path, data, size, mode, ctx);
			if (error == ENOENT && !(error = native_gpu_addon_parents(p, scratch, path))) {
				error = native_gpu_addon_write(path, data, size, mode, ctx);
			}
		}
		if (data) {
			kfree_data(data, size ? size : 1);
		}
		if (error < 0) {
			native_gpu_addon_same++;
		} else if (error) {
			native_gpu_addon_failed++;
			native_gpu_addon_error = error;
		} else {
			native_gpu_addon_written++;
		}
		at += (packed + 7) & ~7ULL;
	}
}

int native_marble_metal_prepare(proc_t process, user_addr_t scratch)
{
    unsigned enabled = 0;
    PE_parse_boot_argn("sheng-gpu-addon", &enabled, sizeof(enabled));
    if (!enabled) return 0;
    native_gpu_addon_install(process, scratch);
    if (native_gpu_addon_error || native_gpu_addon_files < 1)
        return native_gpu_addon_error ? native_gpu_addon_error : ENOENT;
    static const char source[] = "/private/var/ngpu/addons";
    static const char target[] = "/System/Library/Extensions";
    native_gpu_addon_mount(process, scratch, 0x400, source, sizeof(source), target, sizeof(target),
        "/System/Library/Extensions/NativeAdrenoMetal.bundle/Info.plist",
        "/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal",
        &native_gpu_addon_stage, &native_gpu_addon_status);
    printf("MarbleMetal: addon files=%d written=%d reused=%d failed=%d mount=%d/%d\n",
        native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_same,
        native_gpu_addon_failed, native_gpu_addon_stage, native_gpu_addon_status);
    if (native_gpu_addon_status) return native_gpu_addon_status;
    static const char wallpaper_source[] = "/private/var/ngpu/wallpaper";
    static const char wallpaper_target[] = "/Library/Wallpaper";
    native_gpu_addon_mount(process, scratch, 0x400, wallpaper_source, sizeof(wallpaper_source),
        wallpaper_target, sizeof(wallpaper_target),
        "/Library/Wallpaper/Collections/Stripe.wallpaperCollection/WallpaperCollection.plist",
        "/Library/Wallpaper/Collections/iOS_17~iphone.wallpaperCollection/Wallpapers/7565.iOS_17-360w-800h@3x~iphone.wallpaper/Wallpaper.plist",
        &native_gpu_addon2_stage, &native_gpu_addon2_status);
    printf("MarbleMetal: wallpaper mount=%d/%d\n", native_gpu_addon2_stage, native_gpu_addon2_status);
    if (native_gpu_addon2_status) return native_gpu_addon2_status;
    static const char timing_source[] = "/private/var/ngpu/event-timing";
    static const char timing_target[] = "/System/Library/EventTimingProfiles";
    native_gpu_addon_mount(process, scratch, 0x400, timing_source, sizeof(timing_source),
        timing_target, sizeof(timing_target),
        "/System/Library/EventTimingProfiles/N841.Default.plist",
        "/System/Library/EventTimingProfiles/D84.Default.plist",
        &native_gpu_addon3_stage, &native_gpu_addon3_status);
    printf("MarbleMetal: event timing mount=%d/%d\n", native_gpu_addon3_stage, native_gpu_addon3_status);
    uint32_t appsync = 0;
    PE_parse_boot_argn("appsync", &appsync, sizeof(appsync));
    if (appsync == 1) {
        static const char library[] = "/private/var/ngpu/appsync/lib";
        static const char libexec[] = "/private/var/ngpu/appsync/libexec";
        static const char helper[] = "/private/var/ngpu/appsync/helper";
        static const char helper_point[] = "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/com.apple.MobileInstallationHelperService.xpc";
        static const char coreservices[] = "/private/var/ngpu/appsync/coreservices";
        static volatile int stages[4], status[4];
        native_gpu_addon_mount(process, scratch, 0x400, library, sizeof(library),
            "/usr/lib", sizeof("/usr/lib"), "/usr/lib/dyld", "/usr/lib/UniXNUAppSync.dylib",
            &stages[0], &status[0]);
        if (status[0]) return status[0];
        native_gpu_addon_mount(process, scratch, 0x400, libexec, sizeof(libexec),
            "/usr/libexec", sizeof("/usr/libexec"), "/usr/libexec/launchd_cache_loader", "/usr/libexec/unixnu-appinst",
            &stages[1], &status[1]);
        native_gpu_addon_mount(process, scratch, 0x400, helper, sizeof(helper),
            helper_point, sizeof(helper_point),
            "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/com.apple.MobileInstallationHelperService.xpc/Info.plist",
            "/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/com.apple.MobileInstallationHelperService.xpc/UniXNUAppSync.runtime",
            &stages[2], &status[2]);
        native_gpu_addon_mount(process, scratch, 0x400, coreservices, sizeof(coreservices),
            "/System/Library/CoreServices", sizeof("/System/Library/CoreServices"),
            "/System/Library/CoreServices/SystemVersion.plist",
            "/System/Library/CoreServices/SpringBoard.app/UniXNUAppSync.runtime", &stages[3], &status[3]);
        printf("MarbleAppSync: library=%d/%d installd=%d/%d helper=%d/%d SpringBoard=%d/%d\n",
            stages[0], status[0], stages[1], status[1], stages[2], status[2], stages[3], status[3]);
        for (unsigned i = 1; i < 4; ++i) if (status[i]) return status[i];
    }
    uint32_t wifi_service = 0;
    PE_parse_boot_argn("marble-wifi-service", &wifi_service, sizeof(wifi_service));
    if (wifi_service) {
        static const char wifi_xpc[] = "/private/var/ngpu/xpc";
        static const char wifi_bin[] = "/private/var/ngpu/bin";
        volatile int wifi_stage = 0, wifi_status = -1;
        native_gpu_addon_mount(process, scratch, 0x400, wifi_bin, sizeof(wifi_bin),
            "/usr/libexec", sizeof("/usr/libexec"),
            "/usr/libexec/launchd_cache_loader", "/usr/libexec/unixnu-wpa_supplicant",
            &wifi_stage, &wifi_status);
        printf("MarbleWifi: service binary mount=%d/%d\n", wifi_stage, wifi_status);
        if (wifi_status) return wifi_status;
        native_gpu_addon_mount(process, scratch, 0x400, wifi_xpc, sizeof(wifi_xpc),
            "/System/Library/xpc", sizeof("/System/Library/xpc"),
            "/System/Library/xpc/launchd.plist", "/System/Library/xpc/launchd.plist.sig",
            &wifi_stage, &wifi_status);
        printf("MarbleWifi: launchd cache mount=%d/%d\n", wifi_stage, wifi_status);
        if (wifi_status) return wifi_status;
    }
    uint32_t haptics = 0;
    PE_parse_boot_argn("marble-haptics", &haptics, sizeof(haptics));
    if (haptics >= 2) {
        static const char hal[] = "/private/var/ngpu/haptics-hal";
        static const char tunings[] = "/private/var/ngpu/haptics-tunings";
        native_gpu_addon_mount(process, scratch, 0x400, hal, sizeof(hal),
            "/System/Library/Audio/Plug-Ins/HAL", sizeof("/System/Library/Audio/Plug-Ins/HAL"),
            "/System/Library/Audio/Plug-Ins/HAL/AirPlayHalogen.driver/Info.plist",
            "/System/Library/Audio/Plug-Ins/HAL/MarbleHaptics.driver/Info.plist",
            &marble_haptics_overlay_stage[0], &marble_haptics_overlay_status[0]);
        if (marble_haptics_overlay_status[0]) return marble_haptics_overlay_status[0];
        native_gpu_addon_mount(process, scratch, 0x400, tunings, sizeof(tunings),
            "/Library/Audio/Tunings", sizeof("/Library/Audio/Tunings"),
            "/Library/Audio/Tunings/N841/Haptics/hapticengineconfig.plist",
            "/Library/Audio/Tunings/D84/Haptics/hapticengineconfig.plist",
            &marble_haptics_overlay_stage[1], &marble_haptics_overlay_status[1]);
        printf("MarbleHaptics: HAL overlay=%d/%d tunings=%d/%d\n",
            marble_haptics_overlay_stage[0], marble_haptics_overlay_status[0],
            marble_haptics_overlay_stage[1], marble_haptics_overlay_status[1]);
        if (marble_haptics_overlay_status[1]) return marble_haptics_overlay_status[1];
    }
    return native_gpu_addon3_status;
}
