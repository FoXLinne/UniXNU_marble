/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Freestanding Android arm64 helper for F2FS_IOC_RESIZE_FS. */
typedef unsigned long u64;
static long call(long n, long a, long b, long c, long d) {
  register long x0 __asm__("x0")=a, x1 __asm__("x1")=b;
  register long x2 __asm__("x2")=c, x3 __asm__("x3")=d, x8 __asm__("x8")=n;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x8) : "memory", "cc");
  return x0;
}
static void number(long n) {
  char text[32]; unsigned i=sizeof(text); text[--i]='\n';
  u64 v=n<0 ? -n : n;
  do { text[--i]='0'+v%10; v/=10; } while(v);
  if(n<0) text[--i]='-';
  call(64,1,(long)(text+i),sizeof(text)-i,0);
}
__attribute__((noreturn)) void run(long *stack) {
  long argc=stack[0]; char **argv=(char **)(stack+1); u64 blocks=0;
  if(argc!=2) { number(-22); call(93,2,0,0,0); __builtin_unreachable(); }
  for(char *s=argv[1];*s;++s) {
    if(*s<'0'||*s>'9') { number(-22); call(93,2,0,0,0); __builtin_unreachable(); }
    blocks=blocks*10+*s-'0';
  }
  long fd=call(56,-100,(long)"/data",0x10000,0);
  long status=fd<0 ? fd : call(29,fd,0x4008f510,(long)&blocks,0);
  number(status); call(93,status<0 ? 1 : 0,0,0,0); __builtin_unreachable();
}
__asm__(".global _start\n_start:\nmov x0,sp\nbl run\n");
