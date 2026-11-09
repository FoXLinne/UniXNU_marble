// SPDX-License-Identifier: BSD-2-Clause-Patent
// RootStage command compatibility over native EP0 vendor requests.
static char rootReply[512]="OKAYRootStage2";
static unsigned rootReplyBytes=14;
static IOMemoryMap *rootExport;
static const uint8_t *rootExportPointer;
static unsigned rootExportBytes;
static unsigned rootAction;
static bool rootReplyTransfer,rootContactDown;
static unsigned rootContactX,rootContactY;

static void rootRespond(const char *code,const char *text) {
    rootReplyBytes=unsigned(snprintf(rootReply,sizeof(rootReply),"%s%s",code,text));
}
static bool rootHex(const char *text,unsigned count,uint64_t *value) {
    uint64_t result=0;
    for(unsigned i=0;i<count;++i) {
        const char c=text[i];
        const unsigned digit=c>='0'&&c<='9'?unsigned(c-'0'):
            c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):16;
        if(digit>15)return false;
        result=result*16+digit;
    }
    *value=result;return true;
}
static void rootValue(uint64_t value) {
    char text[17];snprintf(text,sizeof(text),"%016llx",(unsigned long long)value);rootRespond("OKAY",text);
}
static bool rootRange(uint64_t address,uint64_t count,uint64_t base,uint64_t bytes) {
    return address>=base && count<=bytes && address-base<=bytes-count;
}
static bool rootSelect(uint64_t address,unsigned count) {
    const ROOT_STAGE_TABLE *table=native_resident_table();
    if(!table || !count || count>0x400000)return false;
    const ROOT_STAGE_DISK &seed=table->Disks[2];
    const bool kernel=rootRange(address,count,table->KernelBase,table->KernelBytes+0x200000);
    const bool output=seed.Count==1 && seed.Bytes>=0x100000 &&
        rootRange(address,count,seed.Extents[0].Address+seed.Bytes-0x100000,0x100000);
    if(!kernel&&!output)return false;
    if(kernel) {
        const uintptr_t first=ml_static_ptovirt(address),last=ml_static_ptovirt(address+count-1);
        if(!first || !last || last-first!=count-1)return false;
        OSSafeReleaseNULL(rootExport);rootExportPointer=reinterpret_cast<const uint8_t *>(first);
        rootExportBytes=count;return true;
    }
    auto *memory=IOMemoryDescriptor::withAddressRange(address,count,kIODirectionIn|kIOMemoryMapperNone,nullptr);
    auto *mapping=memory?memory->map(kIOMapAnywhere|kIOMapCopybackCache):nullptr;
    OSSafeReleaseNULL(memory);
    if(!mapping)return false;
    OSSafeReleaseNULL(rootExport);rootExport=mapping;rootExportBytes=count;
    rootExportPointer=reinterpret_cast<const uint8_t *>(mapping->getVirtualAddress());return true;
}
static void rootSessionReset() {
    OSSafeReleaseNULL(rootExport);rootExportPointer=nullptr;rootExportBytes=0;rootRespond("FAIL","USB session reset");
    rootAction=0;rootReplyTransfer=false;returnTicks=rebootTicks=0;
    if(rootContactDown)native_hid_request_contact(rootContactX,rootContactY,false);
    rootContactDown=false;
}
static unsigned rootUpload(unsigned offset,void *out,unsigned count) {
    if(!rootExportPointer || offset>=rootExportBytes)return 0;
    const unsigned size=MIN(count,rootExportBytes-offset);
    memcpy(out,rootExportPointer+offset,size);
    if(offset+size==rootExportBytes)rootRespond("OKAY","");
    return size;
}
static void rootCommand(const char *text) {
    rootAction=0;
    const ROOT_STAGE_TABLE *table=native_resident_table();
    char value[256];
    if(!strcmp(text,"getvar:version"))rootRespond("OKAY","RootStage2");
    else if(!strcmp(text,"getvar:phase"))rootRespond("OKAY","kernel");
    else if(!strcmp(text,"getvar:root-table-version"))rootValue(ROOT_STAGE_VERSION);
    else if(!strcmp(text,"getvar:kernel-ready"))rootRespond("OKAY",table?"ready":"pending");
    else if(!strcmp(text,"getvar:features"))rootRespond("OKAY","runtime-usb,memory-read,input-control,kernel-usb");
    else if(!strcmp(text,"getvar:control"))rootValue(0);
    else if(!strcmp(text,"getvar:returns"))rootValue(table?table->ReturnCount:0);
    else if(!strcmp(text,"getvar:usb-session"))rootValue(connectCount+disconnectCount+restartCount);
    else if(!strcmp(text,"getvar:return-usb-status"))rootValue(0);
    else if(!strcmp(text,"getvar:max-download-size"))rootValue(0x400000);
    else if(!strcmp(text,"getvar:buttons-firmware"))rootRespond("OKAY","firmware-paused");
    else if(!strcmp(text,"getvar:usb-affinity")) {
        uint64_t affinity;__asm__ volatile("mrs %0,mpidr_el1":"=r"(affinity));rootValue(affinity&0xff00ffffffULL);
    } else if(!strcmp(text,"getvar:usb-stats")) {
        uint64_t frequency;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
        const uint64_t fields[]={pollCount,busyTicks,idleWaits,timeoutCount,connectCount,disconnectCount,restartCount,failures,usbCounter(),frequency};
        for(unsigned i=0;i<10;++i)snprintf(value+17*i,sizeof(value)-17*i,"%016llx%s",(unsigned long long)fields[i],i<9?":":"");
        rootRespond("OKAY",value);
    } else if(!strcmp(text,"getvar:kernel-memory") && table) {
        snprintf(value,sizeof(value),"%016llx:%016llx",table->KernelBase,table->KernelBytes);rootRespond("OKAY",value);
    } else if(!strcmp(text,"getvar:handoff-translation")) {
        uint64_t translation,control;
        __asm__ volatile("mrs %0,ttbr1_el1\n\tmrs %1,tcr_el1":"=r"(translation),"=r"(control));
        snprintf(value,sizeof(value),"%016llx:%016llx",(unsigned long long)translation,(unsigned long long)control);rootRespond("OKAY",value);
    } else if(!strcmp(text,"getvar:watchdog")) {
        uint64_t state=0;const uint64_t result=watchdogCall(0x86000006,&state);
        snprintf(value,sizeof(value),"%016llx:%016llx:%016llx",(unsigned long long)result,
                 (unsigned long long)state,(unsigned long long)watchdogPetStatus);rootRespond("OKAY",value);
    }
    else if(!strcmp(text,"getvar:rtc")) {
        clock_sec_t seconds=0;clock_usec_t usec=0;clock_get_calendar_microtime(&seconds,&usec);
        snprintf(value,sizeof(value),"%016llx:0000000000000000",(unsigned long long)seconds);rootRespond("OKAY",value);
    } else if(!strcmp(text,"oem rtc-sample")) {native_marble_rtc_observe();rootRespond("OKAY","");}
    else if(!strncmp(text,"oem rtc-sync:",13) && strlen(text)==29) {
        uint64_t seconds=0;
        if(!rootHex(text+13,16,&seconds))rootRespond("FAIL","invalid clock request");
        else {native_marble_rtc_host_sync(seconds);rootRespond("OKAY","");}
    } else if(!strcmp(text,"getvar:display-timing"))rootRespond("OKAY","kernel");
    else if(!strcmp(text,"oem select:2") && table) {
        const ROOT_STAGE_DISK &seed=table->Disks[2];
        const bool ok=seed.Count==1 && seed.Bytes>=0x100000 && rootSelect(seed.Extents[0].Address+seed.Bytes-0x100000,0x100000);
        rootRespond(ok?"OKAY":"FAIL",ok?"":"kernel output unavailable");
    } else if(!strncmp(text,"oem memory-read:",16) && strlen(text)==41 && text[32]==':') {
        uint64_t address=0,count=0;
        const bool ok=rootHex(text+16,16,&address)&&rootHex(text+33,8,&count)&&count<=0x400000&&rootSelect(address,unsigned(count));
        rootRespond(ok?"OKAY":"FAIL",ok?"":"outside retained kernel memory");
    } else if(!strcmp(text,"upload") && rootExportPointer) {
        snprintf(value,sizeof(value),"%08x",rootExportBytes);rootRespond("DATA",value);
    } else if(!strncmp(text,"oem input:",10) && strlen(text)==22 && text[12]==':' && text[17]==':') {
        uint64_t operation=0,x=0,y=0;
        const bool valid=rootHex(text+10,2,&operation)&&rootHex(text+13,4,&x)&&rootHex(text+18,4,&y)&&
            operation>=1&&operation<=5&&x<1080&&y<2400;
        if(!valid){rootRespond("FAIL","invalid input request");return;}
        if(operation==1)native_hid_request_tap(unsigned(x),unsigned(y));
        if(operation==2||operation==3) {
            native_hid_request_contact(unsigned(x),unsigned(y),operation==2);
            rootContactDown=operation==2;rootContactX=unsigned(x);rootContactY=unsigned(y);
        }
        if(operation==4)native_marble_display_snapshot();
        if(operation==5) {
            if(y==1)native_marble_buttons_pulse(unsigned(x));
            else if(x)native_marble_panel_refresh_request(x==1?0:unsigned(x));
            else native_marble_display_timing_request();
        }
        rootRespond("OKAY","");
    } else if(!strcmp(text,"oem interrupt")) {rootAction=1;rootRespond("OKAY","interrupt requested");}
    else if(!strcmp(text,"reboot-bootloader")) {rootAction=2;rootRespond("OKAY","bootloader reset requested");}
    else rootRespond("FAIL","command requires firmware phase");
}
