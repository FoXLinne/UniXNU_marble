// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
extern "C" {
bool marble_adsp_prepare(void);
void marble_adsp_start(void);
void marble_battery_probe(void);
unsigned marble_adsp_report(char *, unsigned);
extern volatile int32_t native_adsp_stats[16], native_battery_stats[16], native_charger_stats[16], native_qrtr_stats[16];
extern char native_adsp_text[96], native_qrtr_text[96];
void native_marble_charging_start(void) {
    if (!marble_adsp_prepare()) return;
    marble_battery_probe();
    marble_adsp_start();
}
unsigned native_marble_charging_report(char *out,unsigned cap) {
    unsigned at=0;
    const volatile int32_t *sets[]={native_adsp_stats,native_battery_stats,native_charger_stats,native_qrtr_stats};
    const char *names[]={"ADSP","Battery","Charger","QRTR"};
    for(unsigned n=0;n<4 && at+200<cap;n++) {
        at+=snprintf(out+at,cap-at,"Marble%s",names[n]);
        for(unsigned i=0;i<16;i++)at+=snprintf(out+at,cap-at," %d",sets[n][i]);
        at+=snprintf(out+at,cap-at,"\n");
    }
    at+=snprintf(out+at,cap-at,"ADSP %s QRTR %s\n",native_adsp_text,native_qrtr_text);
    at+=marble_adsp_report(out+at,cap-at);
    return at;
}
}
