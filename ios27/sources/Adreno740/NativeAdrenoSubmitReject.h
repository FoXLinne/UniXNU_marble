/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* First invalid submission survives client teardown. Word 0 publishes the
 * record (0 empty, 1 writing, 2 complete); word 15 counts rejection reports.
 * Words 1..14: pid, source (1 client, 2 submit), source line, status, count,
 * queue, buffer index, BO handle, requested IOVA/bytes, mapping handle,
 * mapped IOVA/bytes and permissions. */
extern "C" volatile uint64_t native_adreno_submit_reject[16];
static inline void nativeAdrenoSubmitReject(uint32_t pid,uint32_t source,
    uint32_t line,uint32_t status,uint64_t count,uint64_t queue,
    uint64_t index=UINT64_MAX,uint64_t handle=0,uint64_t iova=0,
    uint64_t bytes=0,uint64_t mapping=0,uint64_t mapped_iova=0,
    uint64_t mapped_bytes=0,uint64_t permissions=0)
{
    if(status!=0xe00002c2u)return;
    __atomic_fetch_add(&native_adreno_submit_reject[15],1,__ATOMIC_RELAXED);
    uint64_t expected=0;
    if(!__atomic_compare_exchange_n(&native_adreno_submit_reject[0],&expected,1,
        false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    const uint64_t values[]={pid,source,line,status,count,queue,index,handle,
        iova,bytes,mapping,mapped_iova,mapped_bytes,permissions};
    for(unsigned i=0;i<14;++i)native_adreno_submit_reject[1+i]=values[i];
    __atomic_store_n(&native_adreno_submit_reject[0],2,__ATOMIC_RELEASE);
}
