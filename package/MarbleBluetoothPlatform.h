// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7475 UART20: stock clock ID 0x82 is QUP wrapper2 serial5.
#pragma once
#include <stdint.h>
#include <sys/errno.h>
constexpr uint64_t MARBLE_UART20_BASE=0x894000, MARBLE_QUP2_BASE=0x8c0000;
constexpr uint32_t MARBLE_UART20_INTID=619, MARBLE_QUP2_SID=0x483;
struct marble_geni_clock_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay)(void *,unsigned);
};
static inline bool marble_gcc_update32(volatile uint32_t *reg,uint32_t clear,uint32_t set) {
    *reg=(*reg&~clear)|set;__asm__ volatile("dsb sy":::"memory");
    return (*reg&set)==set && !(*reg&clear&~set);
}
static inline bool marble_geni_uart20_vote(volatile uint32_t *reg,bool enable) {
    // Wrapper core, core2x and both AHB clocks remain shared. Only serial5 is gated.
    return marble_gcc_update32(reg,enable?0u:1u<<9,enable?0x20fu:0u);
}
static inline int marble_geni_uart20_source(const marble_geni_clock_io *io,uint32_t baud,uint32_t *hz) {
    if(!io || !hz)return -EINVAL;
    uint32_t rate,m,n;
    switch(baud) {
        case 115200:rate=7372800;m=384;n=15625;break;
        case 3000000:rate=48000000;m=4;n=25;break;
        case 4000000:rate=64000000;m=16;n=75;break;
        default:return -EINVAL;
    }
    constexpr uint32_t command=0x2e618;
    io->write(io->context,command+8,m);
    io->write(io->context,command+12,(~(n-m))&0xffff);
    io->write(io->context,command+16,(~n)&0xffff);
    const uint32_t cfg=(io->read(io->context,command+4)&~0x373fu)|1u|(6u<<8)|(2u<<12);
    io->write(io->context,command+4,cfg);
    io->write(io->context,command,io->read(io->context,command)|1u);
    for(unsigned i=0;i<1000;i++) {
        if(!(io->read(io->context,command)&1u)){*hz=rate;return 0;}
        io->delay(io->context,1);
    }
    return -ETIMEDOUT;
}
