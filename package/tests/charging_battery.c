// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../MarbleBattmgr.h"
static uint32_t get(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static int32_t battery[22]={1,1,1,3,7300,100,4000000,4010000,4460000,-448000,2000000,16,2000000,3840,2,3022,419,5000000,4140000,0,600,65535};
static int32_t usb[8]={1,5090000,5000000,503000,500000,100000,1,1};
static unsigned writes;
static void cycle(MarbleBattmgr *b) {
    uint8_t q[24],r[24];
    for(unsigned n=0;n<200;n++) {
        unsigned size=marble_battmgr_next(b,q);
        if(!size)return;
        assert(size==24 && get(q)==32778 && get(q+4)==1 && get(q+12)==0);
        unsigned opcode=get(q+8),property=get(q+16);
        memcpy(r,q,12);put(r+12,property);put(r+20,0);
        if(opcode==0x30) {assert(property<22);put(r+16,(uint32_t)battery[property]);}
        else if(opcode==0x32) {assert(property<8);put(r+16,(uint32_t)usb[property]);}
        else {assert(opcode==0x33 && property==5);assert(usb[0]);usb[5]=(int32_t)get(q+20);writes++;put(r+16,0);}
        assert(marble_battmgr_receive(b,r,24)!=BmIgnored);
    }
    assert(!"refresh did not terminate");
}
int main(void) {
    MarbleBattmgr b;MarbleBatterySnapshot s;uint8_t q[24],r[24]={0};
    marble_battmgr_init(&b);marble_battmgr_refresh(&b,1);cycle(&b);marble_battmgr_snapshot(&b,&s);
    assert(s.present && s.external && s.charging && !s.fully_charged);
    assert(s.percent==73 && s.voltage_mv==4010 && s.current_ma==448 && s.temperature==3840);
    assert(s.raw_mah==3022 && s.full_mah==4140 && s.design_mah==5000 && s.cycles==419);
    assert(writes==1 && usb[5]==500000 && b.limit_sets==1);
    battery[4]=10000;marble_battmgr_refresh(&b,0);cycle(&b);marble_battmgr_snapshot(&b,&s);assert(!s.fully_charged);
    battery[0]=4;marble_battmgr_refresh(&b,0);cycle(&b);marble_battmgr_snapshot(&b,&s);assert(s.fully_charged && !s.charging);
    usb[0]=0;usb[5]=100000;battery[0]=2;battery[9]=620000;
    marble_battmgr_refresh(&b,0);cycle(&b);marble_battmgr_snapshot(&b,&s);
    assert(!s.external && !s.charging && s.current_ma==-620 && writes==1);
    usb[0]=1;battery[0]=1;marble_battmgr_refresh(&b,0);cycle(&b);assert(writes==2);
    usb[7]=2;usb[6]=2;usb[5]=100000;marble_battmgr_refresh(&b,0);cycle(&b);assert(writes==2);
    marble_battmgr_refresh(&b,0);assert(marble_battmgr_next(&b,q)==24);
    put(r,32778);put(r+4,1);put(r+8,0x30);put(r+12,99);assert(marble_battmgr_receive(&b,r,24)==BmIgnored);
    assert(b.in_flight);marble_battmgr_timeout(&b);assert(!(b.valid&(1u<<BmStatus)));cycle(&b);assert(b.timeouts==1);
    put(r+8,7);put(r+4,1);assert(marble_battmgr_receive(&b,r,16)==BmIgnored);
    put(r+4,2);assert(marble_battmgr_receive(&b,r,12)==BmIgnored);assert(marble_battmgr_receive(&b,r,16)==BmNotified);
    cycle(&b);assert(b.notifications==1);
    puts("battery protocol, units, current limits, disconnect, timeout: passed");
    return 0;
}
