// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <CoreFoundation/CoreFoundation.h>
#include <sys/mount.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

extern CFTypeRef MGCopyAnswer(CFStringRef, CFDictionaryRef);
extern const CFStringRef kMGQDiskUsageTotalDiskCapacity;

/* 24B119 AboutSettings._loadValues reads DiskUsage.TotalDiskCapacity from
 * MobileGestalt, which queries disk0's IOMedia size. On marble disk0 is the
 * sealed OS image; the installed writable storage is mounted at /private/var.
 * Preserve its device geometry and supply the logical Data volume's actual
 * size to this space API. The retained answer and all other fields survive. */
static CFTypeRef marble_storage_answer(CFTypeRef answer, const struct statfs *volume)
{
    if (!answer || CFGetTypeID(answer)!=CFDictionaryGetTypeID() || !volume ||
        strcmp(volume->f_fstypename,"apfs") || !volume->f_bsize ||
        volume->f_blocks>(uint64_t)INT64_MAX/volume->f_bsize ||
        volume->f_bfree>volume->f_blocks || volume->f_bavail>volume->f_bfree) return answer;
    CFTypeRef old=CFDictionaryGetValue((CFDictionaryRef)answer,kMGQDiskUsageTotalDiskCapacity);
    if (!old || CFGetTypeID(old)!=CFNumberGetTypeID()) return answer;
    const int64_t bytes=(int64_t)(volume->f_blocks*volume->f_bsize);
    CFNumberRef number=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt64Type,&bytes);
    CFMutableDictionaryRef fixed=number ? CFDictionaryCreateMutableCopy(kCFAllocatorDefault,0,(CFDictionaryRef)answer) : NULL;
    if (!fixed) {if(number)CFRelease(number);return answer;}
    CFDictionarySetValue(fixed,kMGQDiskUsageTotalDiskCapacity,number);
    CFRelease(number);CFRelease(answer);
    return fixed;
}

static CFTypeRef marble_copy_storage_answer(CFStringRef question, CFDictionaryRef options)
{
    CFTypeRef answer=MGCopyAnswer(question,options);
    if (!answer || CFGetTypeID(answer)!=CFDictionaryGetTypeID() ||
        !CFDictionaryContainsKey((CFDictionaryRef)answer,kMGQDiskUsageTotalDiskCapacity)) return answer;
    struct statfs volume;
    if (statfs("/private/var",&volume)) return answer;
    return marble_storage_answer(answer,&volume);
}
#ifndef MARBLE_STORAGE_HOST_TEST
__attribute__((used,section("__DATA,__interpose")))
static const struct {const void *replacement,*replacee;} marble_storage_interpose={
    (const void *)marble_copy_storage_answer,(const void *)MGCopyAnswer
};
#endif
