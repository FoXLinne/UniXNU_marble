// SPDX-License-Identifier: MIT
#include "NativeAdrenoABI.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
    auto open=(int(*)())dlsym(RTLD_DEFAULT,"host_gpu_open");
    auto close=(int(*)())dlsym(RTLD_DEFAULT,"host_gpu_close");
    auto call=(int(*)(uint32_t,const void *,size_t,void *,size_t))dlsym(RTLD_DEFAULT,"host_gpu_call");
    auto live=(unsigned(*)())dlsym(RTLD_DEFAULT,"host_gpu_live_objects");
    CHECK(open && close && call && live);CHECK(!open());
    NativeAdrenoAbiParams params{};CHECK(!call(NativeAdrenoMethodQuery,nullptr,0,&params,sizeof(params)));
    CHECK(params.flags&NativeAdrenoParamsTaskCpuMap);
    NativeAdrenoAbiBoAlloc alloc{16384,12,0};NativeAdrenoAbiBoAllocReply bo{},other{};
    CHECK(!call(NativeAdrenoMethodBoAlloc,&alloc,sizeof(alloc),&bo,sizeof(bo)));
    CHECK(!call(NativeAdrenoMethodBoAlloc,&alloc,sizeof(alloc),&other,sizeof(other)));
    NativeAdrenoAbiBoCpuMapTask request{bo.handle,0};NativeAdrenoAbiBoCpuMapTaskReply mapping{},second{};
    CHECK(call(NativeAdrenoMethodBoCpuMapTask,&request,sizeof(request)-1,&mapping,sizeof(mapping)));
    CHECK(!call(NativeAdrenoMethodBoCpuMapTask,&request,sizeof(request),&mapping,sizeof(mapping)));
    CHECK(mapping.address && mapping.bytes>=alloc.bytes && mapping.token && !mapping.reserved);
    std::memset((void *)(uintptr_t)mapping.address,0x73,(size_t)alloc.bytes);
    NativeAdrenoAbiBoFree free{bo.handle};
    CHECK(call(NativeAdrenoMethodBoFree,&free,sizeof(free),nullptr,0));
    NativeAdrenoAbiBoCpuUnmapTask undo{other.handle,mapping.address,mapping.token,0};
    CHECK(call(NativeAdrenoMethodBoCpuUnmapTask,&undo,sizeof(undo),nullptr,0));
    undo.handle=bo.handle;undo.address+=4096;
    CHECK(call(NativeAdrenoMethodBoCpuUnmapTask,&undo,sizeof(undo),nullptr,0));
    undo.address=mapping.address;undo.reserved=1;
    CHECK(call(NativeAdrenoMethodBoCpuUnmapTask,&undo,sizeof(undo),nullptr,0));
    undo.reserved=0;
    CHECK(!call(NativeAdrenoMethodBoCpuMapTask,&request,sizeof(request),&second,sizeof(second)));
    CHECK(second.address==mapping.address && second.token==mapping.token);
    CHECK(!call(NativeAdrenoMethodBoCpuUnmapTask,&undo,sizeof(undo),nullptr,0));
    CHECK(call(NativeAdrenoMethodBoFree,&free,sizeof(free),nullptr,0));
    CHECK(((unsigned char *)(uintptr_t)second.address)[alloc.bytes-1]==0x73);
    CHECK(!call(NativeAdrenoMethodBoCpuUnmapTask,&undo,sizeof(undo),nullptr,0));
    CHECK(call(NativeAdrenoMethodBoCpuUnmapTask,&undo,sizeof(undo),nullptr,0));
    CHECK(!call(NativeAdrenoMethodBoFree,&free,sizeof(free),nullptr,0));
    free.handle=other.handle;CHECK(!call(NativeAdrenoMethodBoFree,&free,sizeof(free),nullptr,0));
    CHECK(!live());CHECK(!close());
    std::puts("PASS: task CPU map protocol: bounds, identity, repeated leases, invalid release and BO lifetime");
}
