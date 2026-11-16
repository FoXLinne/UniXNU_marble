// What plain Metal does with a LATE store action of `Store` on a memoryless attachment.
//
// A memoryless attachment holds no memory, so the descriptor's `MTLStoreActionStore` on one is
// already known to run on plain Metal while Metal's End Encoding Validation alone objects
// ("endEncoding called with store action unsupported for Memoryless attachment."). The encoder's
// own setters -- `setColorStoreAction:atIndex:`, `setDepthStoreAction:`,
// `setStencilStoreAction:` -- write the same encoder state after the fact, over a descriptor that
// said `MTLStoreActionUnknown`, and THAT call's answer is what this probe measures: the runtime
// refused it on the ground that it was unmeasured.
//
// ONE CASE PER PROCESS, chosen on the command line, and every case is GRADED AT THE COMMIT, not
// at the call: a render-encoder call that returns can still fail the command buffer. A sibling
// measurement of a memoryless resolve target graded "accepted" at record time and then failed its
// commit with `kIOGPUCommandBufferCallbackErrorInvalidResource` (code 9), zero vertices and zero
// fragments, so a call that returns proves nothing on its own.
//
// Every case therefore reports, in this order: the device read back off the device; whether the
// encoder was made and whether the setter returned; whether `endEncoding` returned; the command
// buffer's status and error after a bounded wait; the vertex and fragment counts the stages
// themselves wrote into atomics (positive evidence that the pass RAN, which no status gives); the
// first texel of every attachment that has memory to read; a 0xEE guard buffer placed at a
// 256-aligned offset after the readable destination in the same placement heap; and three canary
// command buffers, which say whether anything had to recover afterwards.
//
// EVERY MEMORYLESS CASE HAS A TWIN THAT DIFFERS ONLY IN THE ATTACHMENT'S STORAGE MODE, or only in
// the action set late, because "the pass completed" and "the setter did nothing" read identically
// against a memoryless attachment alone: nothing can be read back out of one.
//
// Nothing here asserts what a memoryless attachment's texels ARE. Metal specifies no contents for
// tile memory, and a related measurement of a memoryless `Load` reading `0x00000000` was
// deliberately not pinned for that reason.
//
// Build (no cargo target needed):
//   clang -fobjc-arc -O1 -fmodules -o memoryless_late_store_action \
//     memoryless_late_store_action.m -framework Foundation -framework Metal
//
// Measuring runs keep `MTL_DEBUG_LAYER` OUT of the environment: the layer is not passive (it has
// been measured to change what a shader reads). Layer messages are taken in separate runs, with
// `MTL_DEBUG_LAYER=1` and `MTL_DEBUG_LAYER_ERROR_MODE=nslog`, and reported as layer messages --
// End Encoding Validation is a layer message, not plain Metal's answer.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static void P(const char *fmt, ...) { va_list ap; va_start(ap,fmt);
  fputs("RESULT ",stdout); vfprintf(stdout,fmt,ap); fputc('\n',stdout); fflush(stdout); va_end(ap); }

static id<MTLDevice> D; static id<MTLCommandQueue> Q; static id<MTLLibrary> L;

static const char *SRC =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 c; };\n"
"static float2 corner(uint vid){ float2 p[3]={float2(-1.0,-1.0),float2(3.0,-1.0),float2(-1.0,3.0)}; return p[vid]; }\n"
"vertex VOut vplain(uint vid [[vertex_id]], device atomic_uint* rep [[buffer(1)]]){\n"
"  VOut o; o.pos=float4(corner(vid),0.5,1.0);\n"
"  atomic_fetch_add_explicit(&rep[1],1u,memory_order_relaxed);\n"
"  o.c=float4(1.0,0.5,0.25,1.0); return o; }\n"
"fragment float4 f1(VOut in [[stage_in]], device atomic_uint* rep [[buffer(1)]]){\n"
"  atomic_fetch_add_explicit(&rep[2],1u,memory_order_relaxed); return in.c; }\n"
"struct F2 { float4 a [[color(0)]]; float4 b [[color(1)]]; };\n"
"fragment F2 f2(VOut in [[stage_in]], device atomic_uint* rep [[buffer(1)]]){\n"
"  atomic_fetch_add_explicit(&rep[2],1u,memory_order_relaxed);\n"
"  F2 o; o.a=in.c; o.b=float4(0.0,1.0,0.5,1.0); return o; }\n"
"kernel void kbump(device atomic_uint* rep [[buffer(0)]], uint t [[thread_position_in_grid]]){\n"
"  atomic_fetch_add_explicit(&rep[3],1u,memory_order_relaxed); }\n";

static void boot(void){
  D=MTLCreateSystemDefaultDevice(); if(!D){P("fatal=no-device");exit(3);}
  P("device.class=%s name=%s unified=%d",class_getName([(NSObject*)D class]),
    [[D name] UTF8String],(int)[D hasUnifiedMemory]);
  Q=[D newCommandQueue];
  NSError *e=nil; L=[D newLibraryWithSource:[NSString stringWithUTF8String:SRC] options:nil error:&e];
  if(!L){P("fatal=library %s",[[e description] UTF8String]);exit(3);}
  P("library=made");
}
static id<MTLFunction> fn(const char*n){ id<MTLFunction> f=[L newFunctionWithName:[NSString stringWithUTF8String:n]];
  if(!f){P("fatal=no-function %s",n);exit(3);} return f; }

static const char *STATUS[]={"NotEnqueued","Enqueued","Committed","Scheduled","Completed","Error"};
// Commits and waits. Returns 0 on completion (any status), 1 on timeout.
static int finish(id<MTLCommandBuffer> cb, const char *tag, double secs){
  dispatch_semaphore_t s=dispatch_semaphore_create(0);
  [cb addCompletedHandler:^(id<MTLCommandBuffer> c){ (void)c; dispatch_semaphore_signal(s); }];
  [cb commit];
  P("%s: commit returned", tag);
  long r=dispatch_semaphore_wait(s,dispatch_time(DISPATCH_TIME_NOW,(int64_t)(secs*NSEC_PER_SEC)));
  if(r!=0){ P("%s: TIMEOUT after %.1fs status=%s",tag,secs,
      ((unsigned)cb.status<6)?STATUS[cb.status]:"?"); return 1; }
  NSError *err=cb.error;
  P("%s: status=%s error=%s",tag,((unsigned)cb.status<6)?STATUS[cb.status]:"?",
    err?[[NSString stringWithFormat:@"domain=%@ code=%ld desc=%@",err.domain,(long)err.code,
          err.localizedDescription] UTF8String]:"none");
  return 0;
}

static id<MTLBuffer> shared(NSUInteger n){
  id<MTLBuffer> b=[D newBufferWithLength:n options:MTLResourceStorageModeShared];
  memset(b.contents,0,n); return b; }
static void repline(const char *tag, id<MTLBuffer> rep){ unsigned *r=(unsigned*)rep.contents;
  P("%s: rep vertices=%u fragments=%u canary_threads=%u",tag,r[1],r[2],r[3]); }

static MTLTextureDescriptor *desc2d(MTLPixelFormat pf, MTLStorageMode sm, NSUInteger samples,
                                    NSUInteger wh){
  MTLTextureDescriptor *x=[[MTLTextureDescriptor alloc] init];
  x.pixelFormat=pf; x.width=wh; x.height=wh; x.depth=1;
  x.textureType = samples>1 ? MTLTextureType2DMultisample : MTLTextureType2D;
  x.sampleCount=samples; x.mipmapLevelCount=1; x.arrayLength=1;
  x.usage=MTLTextureUsageRenderTarget;
  if(sm==MTLStorageModeMemoryless && samples==1) x.usage|=MTLTextureUsageShaderRead;
  x.storageMode=sm; return x; }
static id<MTLTexture> tex(MTLPixelFormat pf, MTLStorageMode sm, NSUInteger samples, NSUInteger wh){
  id<MTLTexture> t=[D newTextureWithDescriptor:desc2d(pf,sm,samples,wh)];
  if(!t){P("fatal=no-texture format=%lu storage=%lu samples=%lu",(unsigned long)pf,
           (unsigned long)sm,(unsigned long)samples);exit(4);}
  return t; }

// The first texel of a Shared texture, and how many of its bytes are nonzero.
static void texel(const char *tag, id<MTLTexture> t){
  if(!t){ P("%s: absent",tag); return; }
  if(t.storageMode!=MTLStorageModeShared){
    P("%s: unreadable (storage=%lu) -- a memoryless or private attachment has nothing to read",
      tag,(unsigned long)t.storageMode); return; }
  NSUInteger w=t.width,h=t.height,bpr=w*4; unsigned char *buf=calloc(1,bpr*h);
  @try { [t getBytes:buf bytesPerRow:bpr fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0]; }
  @catch(NSException *ex){ P("%s: getBytes raised %s",tag,[[ex reason] UTF8String]); free(buf); return; }
  int nz=0; for(NSUInteger i=0;i<bpr*h;i++) if(buf[i]) nz++;
  P("%s: %lux%lu texel0=%02x%02x%02x%02x nonzero_bytes=%d/%lu",tag,(unsigned long)w,(unsigned long)h,
    buf[0],buf[1],buf[2],buf[3],nz,(unsigned long)(bpr*h));
  free(buf); }

static void canaries(id<MTLBuffer> rep){
  NSError *e=nil;
  id<MTLComputePipelineState> ps=[D newComputePipelineStateWithFunction:fn("kbump") error:&e];
  if(!ps){P("canary: pipeline failed %s",[[e description] UTF8String]);return;}
  for(int i=0;i<3;i++){ id<MTLCommandBuffer> cb=[Q commandBuffer];
    id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:rep offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(8,1,1) threadsPerThreadgroup:MTLSizeMake(8,1,1)];
    [ce endEncoding];
    char tag[32]; snprintf(tag,sizeof tag,"canary%d",i);
    if(finish(cb,tag,10.0)) return; }
  unsigned *r=(unsigned*)rep.contents; P("canary: threads_total=%u (expect 24)",r[3]); }

// A Shared placement heap holding `x`'s texture at offset 0 and a 0xEE guard buffer at the next
// 256-aligned offset after it. The guard is what says whether Metal wrote past the destination.
static id<MTLHeap> HEAP; static id<MTLBuffer> GUARD;
static id<MTLTexture> guarded(MTLTextureDescriptor *x){
  MTLSizeAndAlign ta=[D heapTextureSizeAndAlignWithDescriptor:x];
  NSUInteger goff=(ta.size+255)&~(NSUInteger)255;
  MTLSizeAndAlign ba=[D heapBufferSizeAndAlignWithLength:4096 options:MTLResourceStorageModeShared];
  if(ba.align>256) goff=(goff+ba.align-1)&~(ba.align-1);
  MTLHeapDescriptor *h=[[MTLHeapDescriptor alloc] init];
  h.size=goff+ba.size+4096; h.storageMode=MTLStorageModeShared; h.type=MTLHeapTypePlacement;
  HEAP=[D newHeapWithDescriptor:h];
  if(!HEAP){P("fatal=no-heap");exit(4);}
  id<MTLTexture> t=[HEAP newTextureWithDescriptor:x offset:0];
  GUARD=[HEAP newBufferWithLength:4096 options:MTLResourceStorageModeShared offset:goff];
  if(!t||!GUARD){P("fatal=no-placement t=%d guard=%d",t?1:0,GUARD?1:0);exit(4);}
  memset(GUARD.contents,0xEE,4096);
  P("heap: destination_size=%lu align=%lu guard_at=%lu (0xEE, 4096 bytes)",
    (unsigned long)ta.size,(unsigned long)ta.align,(unsigned long)goff);
  return t; }
static void guardline(void){
  if(!GUARD){ P("guard: none"); return; }
  unsigned char *c=(unsigned char*)GUARD.contents; long at=-1,n=0;
  for(NSUInteger i=0;i<GUARD.length;i++) if(c[i]!=0xEE){ if(at<0)at=(long)i; n++; }
  if(at<0) P("guard: unchanged (all 0xEE, %lu bytes)",(unsigned long)GUARD.length);
  else P("guard: CHANGED first_at=%ld changed_bytes=%ld byte_there=%02x",at,n,c[at]); }

static const char *ACTNAME(MTLStoreAction a){
  switch(a){ case MTLStoreActionDontCare: return "DontCare";
             case MTLStoreActionStore: return "Store";
             case MTLStoreActionMultisampleResolve: return "MultisampleResolve";
             case MTLStoreActionStoreAndMultisampleResolve: return "StoreAndMultisampleResolve";
             case MTLStoreActionUnknown: return "Unknown";
             case MTLStoreActionCustomSampleDepthStore: return "CustomSampleDepthStore";
             default: return "?"; } }

int main(int argc,char**argv){ if(argc<2){fprintf(stderr,"usage: mls <case>\n");return 2;}
 const char *c=argv[1];
 @autoreleasepool {
 P("case=%s MTL_DEBUG_LAYER=%s",c,getenv("MTL_DEBUG_LAYER")?:"unset");
 boot();
 id<MTLBuffer> rep=shared(64);
 #define HAS(s) (strstr(c,s)!=NULL)

 // Which action the encoder sets late, from the case name. Every case's descriptor says
 // `Unknown`, which is the only descriptor state that makes the late setter the deciding call.
 MTLStoreAction late = MTLStoreActionStore;
 if(HAS("-dontcare")) late=MTLStoreActionDontCare;
 else if(HAS("-samr")) late=MTLStoreActionStoreAndMultisampleResolve;
 else if(HAS("-mresolve")) late=MTLStoreActionMultisampleResolve;
 int nosetter = HAS("-nosetter");
 int ok = HAS("-ok");                   // the twin: the attachment has memory

 MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
 id<MTLTexture> t0=nil,t1=nil,resolve=nil,ds=nil;
 NSUInteger nat=1, rsc=1;
 const char *arm="?";
 int depthleg=0, stencilleg=0;

 if(strncmp(c,"color",5)==0 && !HAS("samr") && !HAS("mresolve")){
   // The colour leg: attachment 0 memoryless (or Shared in the twin) at `Unknown`, attachment 1
   // always Shared so the pass's output is readable whatever attachment 0 does.
   MTLStorageMode sm = ok ? MTLStorageModeShared : MTLStorageModeMemoryless;
   t0=tex(MTLPixelFormatRGBA8Unorm,sm,1,64);
   t1=guarded(desc2d(MTLPixelFormatRGBA8Unorm,MTLStorageModeShared,1,64));
   rp.colorAttachments[0].texture=t0; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
   rp.colorAttachments[0].storeAction=MTLStoreActionUnknown;
   rp.colorAttachments[1].texture=t1; rp.colorAttachments[1].loadAction=MTLLoadActionClear;
   rp.colorAttachments[1].storeAction=MTLStoreActionStore;
   nat=2; arm="RenderPassProblem::MemorylessStore(Color(0)), late setColorStoreAction:";
 } else if(strncmp(c,"color",5)==0){
   // The resolving colour leg: a 4-sample attachment 0 with a 1-sample resolve target, set late
   // to `StoreAndMultisampleResolve` (refused on a memoryless attachment) or to
   // `MultisampleResolve` (already taken), so the resolve target says whether the resolve ran.
   MTLStorageMode sm = ok ? MTLStorageModePrivate : MTLStorageModeMemoryless;
   t0=tex(MTLPixelFormatRGBA8Unorm,sm,4,64);
   resolve=guarded(desc2d(MTLPixelFormatRGBA8Unorm,MTLStorageModeShared,1,64));
   rp.colorAttachments[0].texture=t0; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
   rp.colorAttachments[0].storeAction=MTLStoreActionUnknown;
   rp.colorAttachments[0].resolveTexture=resolve;
   nat=1; rsc=4; arm="RenderPassProblem::MemorylessStore(Color(0)) resolving, late setColorStoreAction:";
 } else if(strncmp(c,"depth",5)==0 || strncmp(c,"stencil",7)==0){
   depthleg = (strncmp(c,"depth",5)==0); stencilleg=!depthleg;
   MTLStorageMode sm = ok ? MTLStorageModePrivate : MTLStorageModeMemoryless;
   MTLPixelFormat pf = depthleg ? MTLPixelFormatDepth32Float : MTLPixelFormatDepth32Float_Stencil8;
   ds=tex(pf,sm,1,64);
   t1=guarded(desc2d(MTLPixelFormatRGBA8Unorm,MTLStorageModeShared,1,64));
   rp.colorAttachments[0].texture=t1; rp.colorAttachments[0].loadAction=MTLLoadActionClear;
   rp.colorAttachments[0].storeAction=MTLStoreActionStore;
   rp.depthAttachment.texture=ds; rp.depthAttachment.loadAction=MTLLoadActionClear;
   rp.depthAttachment.clearDepth=1.0;
   rp.depthAttachment.storeAction= depthleg ? MTLStoreActionUnknown : MTLStoreActionDontCare;
   if(!depthleg){
     rp.stencilAttachment.texture=ds; rp.stencilAttachment.loadAction=MTLLoadActionClear;
     rp.stencilAttachment.clearStencil=0;
     rp.stencilAttachment.storeAction=MTLStoreActionUnknown; }
   nat=1; arm = depthleg ? "RenderPassProblem::MemorylessStore(Depth), late setDepthStoreAction:"
                         : "RenderPassProblem::MemorylessStore(Stencil), late setStencilStoreAction:";
 } else { P("fatal=unknown-case"); return 2; }

 P("arm=%s late=%s%s storage_under_test=%lu",arm,ACTNAME(late),nosetter?" (SETTER OMITTED)":"",
   (unsigned long)(ds?ds.storageMode:t0.storageMode));

 MTLRenderPipelineDescriptor *pd=[[MTLRenderPipelineDescriptor alloc] init];
 pd.vertexFunction=fn("vplain"); pd.fragmentFunction=fn(nat==2?"f2":"f1");
 pd.rasterSampleCount=rsc;
 for(NSUInteger i=0;i<nat;i++) pd.colorAttachments[i].pixelFormat=MTLPixelFormatRGBA8Unorm;
 if(ds){ pd.depthAttachmentPixelFormat=ds.pixelFormat;
         if(stencilleg) pd.stencilAttachmentPixelFormat=ds.pixelFormat; }
 NSError *e=nil; id<MTLRenderPipelineState> ps=[D newRenderPipelineStateWithDescriptor:pd error:&e];
 P("pipeline=%s%s",ps?"made":"nil",ps?"":[[NSString stringWithFormat:@" err=%@",e.localizedDescription] UTF8String]);
 if(!ps){ P("blocked=no-pipeline"); return 0; }

 id<MTLDepthStencilState> dss=nil;
 if(ds){ MTLDepthStencilDescriptor *dd=[[MTLDepthStencilDescriptor alloc] init];
   dd.depthCompareFunction=MTLCompareFunctionAlways; dd.depthWriteEnabled=YES;
   if(stencilleg){ MTLStencilDescriptor *sd=[[MTLStencilDescriptor alloc] init];
     sd.stencilCompareFunction=MTLCompareFunctionAlways;
     sd.depthStencilPassOperation=MTLStencilOperationReplace;
     dd.frontFaceStencil=sd; dd.backFaceStencil=sd; }
   dss=[D newDepthStencilStateWithDescriptor:dd]; }

 id<MTLCommandBuffer> cb=[Q commandBuffer];
 id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];
 P("encoder=%s class=%s",enc?"made":"nil",enc?class_getName([(NSObject*)enc class]):(const char*)"-");
 if(!enc){ P("blocked=no-encoder"); return 0; }
 [enc setRenderPipelineState:ps];
 if(dss){ [enc setDepthStencilState:dss]; if(stencilleg) [enc setStencilReferenceValue:0x5A]; }
 [enc setVertexBuffer:rep offset:0 atIndex:1];
 [enc setFragmentBuffer:rep offset:0 atIndex:1];
 [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
 P("draw=issued");
 // The call under test, after the draw, exactly as a late store action is meant to be set.
 if(!nosetter){
   if(depthleg) [enc setDepthStoreAction:late];
   else if(stencilleg) [enc setStencilStoreAction:late];
   else [enc setColorStoreAction:late atIndex:0];
   P("setter=returned action=%s",ACTNAME(late));
 } else P("setter=omitted (negative control: an Unknown nobody resolved)");
 [enc endEncoding]; P("endEncoding=returned");
 if(finish(cb,"pass",10.0)) return 0;
 repline("pass",rep);
 texel("t0",t0); texel("t1",t1); texel("resolve",resolve); texel("ds",ds);
 guardline();
 canaries(rep);
 P("done=ok"); return 0; }
}
