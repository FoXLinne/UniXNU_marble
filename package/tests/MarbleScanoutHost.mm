// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "MarbleScanout.h"
#include "GpuScanoutClient.h"

// Exercise the production fallback blit with Apple's Metal device. The plugin
// owns these optional AIR and trace entry points on the phone.
extern "C" void NBWaitInFlight(uint64_t) {}
extern "C" void NBTraceSwap(uint64_t,uint64_t,uint64_t,uint32_t,uint32_t) {}
extern "C" const void *nmb_scanout_convert_air(size_t *length) {*length=0;return nullptr;}
extern "C" id native_bridge_metal_library_with_air(const void *,size_t) {return nil;}

struct Fixture {
    uint8_t *targets=nullptr,*source=nullptr;
    size_t span=(size_t)getpagesize();
    unsigned sequence=0,completed=0,shown=3;
    uint32_t version=NGP_VERSION_BUFFERED,targetOverride=UINT32_MAX;
    uint64_t previous[3]={};
    Fixture() {
        assert(posix_memalign((void **)&targets,span,span*3)==0);
        assert(posix_memalign((void **)&source,span,span)==0);
        memset(targets,0,span*3);memset(source,0,span);
    }
    ~Fixture() {free(targets);free(source);}
    static int call(void *cookie,uint32_t method,const void *,size_t,void *out,size_t bytes) {
        auto &f=*static_cast<Fixture *>(cookie);
        if(method==NGP_METHOD_MODE) {
            assert(bytes==sizeof(NGPMode));
            *static_cast<NGPMode *>(out)={f.version,NGP_BGRX8888,32,32,128,
                f.version==NGP_VERSION_BUFFERED?f.span*3:4096};
        } else if(method==NGP_METHOD_WAIT_SWAP) {
            assert(bytes==(f.version==NGP_VERSION_BUFFERED?sizeof(NGPBufferedSwap):sizeof(NGPSwap)));
            ++f.sequence;
            auto *frame=static_cast<NGPSwap *>(out);
            *frame={f.sequence,(uint64_t)(uintptr_t)f.source,f.span,128,32,32,NGP_BGRX8888,0};
            if(f.version==NGP_VERSION_BUFFERED)
                static_cast<NGPBufferedSwap *>(out)->target=f.targetOverride!=UINT32_MAX?f.targetOverride:(f.sequence-1)%3;
        } else if(method==NGP_METHOD_COMPLETE_SWAP) {
            const unsigned target=f.version==NGP_VERSION_BUFFERED?(f.sequence-1)%3:0;
            assert(memcmp(f.targets+target*f.span,f.source,4096)==0);
            // A GPU write to the next target preserves the frame on screen.
            if(f.shown<3 && f.shown!=target)
                assert(*reinterpret_cast<uint64_t *>(f.targets+f.shown*f.span)==f.previous[f.shown]);
            f.shown=target;
            f.previous[target]=*reinterpret_cast<uint64_t *>(f.source);
            ++f.completed;
        } else assert(false);
        return 0;
    }
    static int map(void *cookie,uint64_t *address,uint64_t *length) {
        auto &f=*static_cast<Fixture *>(cookie);
        *address=(uintptr_t)f.targets;*length=f.span*3;return 0;
    }
    gpuscanout::Transport transport() {return {this,call,map,nullptr};}
};
int main() {@autoreleasepool {
    MarbleScanoutLayout l{};
    assert(marbleScanoutLayout(MarbleSplashBase,1080,2400,4352,&l));
    assert(l.address==MarbleSplashBase+l.slotBytes && l.bytes==l.slotBytes*3);
    assert(l.address+l.bytes<=MarbleSplashBase+MarbleSplashBytes);
    assert(!marbleScanoutLayout(MarbleSplashBase+4096,1080,2400,4352,&l));
    assert(!marbleScanoutLayout(MarbleSplashBase,1080,2400,8192,&l));
    assert(!marbleScanoutLayout(MarbleSplashBase,1080,2400,UINT64_MAX,&l));
    assert(!marbleScanoutLayout(MarbleSplashBase,1080,UINT64_MAX,4352,&l));
    uint32_t pipe[8]={uint32_t(2400<<16)|540,540,0,4352,0,0,0,0};
    assert(marbleScanoutPipeValid(1,pipe,1080,2400,4352));
    pipe[1]=541;assert(!marbleScanoutPipeValid(1,pipe,1080,2400,4352));pipe[1]=540;
    pipe[5]=1;assert(!marbleScanoutPipeValid(1,pipe,1080,2400,4352));pipe[5]=0;
    pipe[7]=1;assert(!marbleScanoutPipeValid(1,pipe,1080,2400,4352));pipe[7]=0;
    assert(marbleScanoutPipeValid(4,pipe,1080,2400,4352));
    assert(!marbleScanoutPipeValid(8,pipe,1080,2400,4352));
    assert(marbleScanoutFlushBit(0)==1 && marbleScanoutFlushBit(3)==(1U<<18));
    assert(marbleScanoutFlushBit(4)==(1U<<11) && marbleScanoutFlushBit(7)==(1U<<25));
    MarbleScanoutOwnership own;
    assert(own.begin(0));assert(!own.begin(1));assert(!own.latched(1));
    // A timeout leaves the pending slot and the displayed slot retained.
    assert(own.pending==0 && own.shown==3);assert(own.latched(0));
    assert(!own.begin(0));assert(own.begin(1));assert(!own.begin(2));assert(own.latched(1));
    own.restored();assert(own.shown==3 && own.pending==3);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();assert(device);
    // The production client encodes real Metal blits into each selected target.
    // The test transport checks completion pixels and the previous target.
    const uint32_t versions[] = {NGP_VERSION,NGP_VERSION_BUFFERED};
    for(uint32_t version : versions) {
        Fixture fixture;fixture.version=version;
        gpuscanout::Stats stats{};gpuscanout::Client client;
        assert(client.open(device,fixture.transport(),&stats));
        for(unsigned i=0;i<9;++i) {
            memset(fixture.source,int(16+i),4096);
            assert(client.serveOne(1));
        }
        assert(fixture.completed==9 && stats.frames==9 && stats.failures==0);
    }
    Fixture bad;bad.targetOverride=NGP_MAX_SCANOUT_BUFFERS;
    gpuscanout::Stats stats{};gpuscanout::Client client;
    assert(client.open(device,bad.transport(),&stats));
    assert(!client.serveOne(1));assert(stats.failures==1 && bad.completed==0);
    puts("PASS: reserved-memory bounds, split-pipe geometry, latch ownership, legacy and buffered GPU pixels, invalid target");
}}
