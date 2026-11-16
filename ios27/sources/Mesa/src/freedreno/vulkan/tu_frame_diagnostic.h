/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
extern "C" bool tu_darwin_frame_diagnostic(uint32_t,const void *,uint32_t);
/* The GMEM swap trace (SHENG_GPU_TRACE, in the render server only): render passes (36), their attachments (44) and the transfers
 * between tile memory and memory (37) reach the kernel's GPU trace. Every other Turnip event stays off. */
static inline bool tu_swap_trace_on() {
   static const bool on=getenv("SHENG_GPU_TRACE")!=nullptr && !strcmp(getprogname(),"backboardd");
   return on;
}
static inline void tu_frame_event(uint32_t event,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0,uint64_t f=0,uint64_t g=0,uint64_t h=0,uint64_t i=0,uint64_t j=0,uint64_t k=0,uint64_t l=0) {
   if(!tu_swap_trace_on() || (event!=36 && event!=37 && event!=44))return;
   /* Other pass and submission records stay in this process's rings (frames.trace). */
   struct {uint64_t seq,us;uint32_t event,pid;uint64_t v[12];} rec={0,0,event,0,{a,b,c,d,e,f,g,h,i,j,k,l}};
   (void)tu_darwin_frame_diagnostic(0x7102,&rec,sizeof(rec));
}
/* Boot-arg sheng-tum=g renders every pass that can in GMEM, sheng-tum=s every pass in system memory (transient attachments too):
 * the colour runs of the GMEM swap check, set by the boot so their command lines stay short. 0 when absent. */
#include <sys/sysctl.h>
static inline char tu_sheng_render_mode() {
   static char mode = 1;
   if (mode == 1) {
      char args[1100] = {};
      size_t size = sizeof args - 1;
      const char *at = sysctlbyname("kern.bootargs", args, &size, nullptr, 0) == 0 ? strstr(args, "sheng-tum=") : nullptr;
      mode = at && (at[10] == 'g' || at[10] == 's') ? at[10] : 0;
   }
   return mode;
}
#else
static inline char tu_sheng_render_mode() {return 0;}
static inline bool tu_swap_trace_on() {return false;}
static inline void tu_frame_event(uint32_t,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0,uint64_t=0) {}
#endif
