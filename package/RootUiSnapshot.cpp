#include <IOKit/IOLib.h>
extern "C" {
void boot_process_capture(void);
void boot_process_snapshot(void);
extern char boot_process_rows[10][120];
unsigned boot_ui_trace_role_rows(unsigned,char [][120],unsigned,unsigned);
unsigned boot_ui_trace_hot_rows(char [][120],unsigned);
void native_root_block_trace_write(const char *);
unsigned native_legacy_state_words(char *,unsigned);
unsigned native_legacy_prepare_report(char *,unsigned);
unsigned native_legacy_clients_dump(char *,unsigned);
unsigned native_legacy_swap_timeline(char *,unsigned);
void root_ui_capture(unsigned seconds);
}
static char rows[64][120];
static char report[8192];
void root_ui_capture(unsigned seconds)
{
  IOSleep((seconds > 90 ? seconds - 75 : seconds / 2) * 1000);
  for (unsigned pass = 0; pass < 2; pass++) {
    snprintf(report,sizeof(report),"[snapshot %u begin]",pass + 1);
    native_root_block_trace_write(report);
    boot_process_capture();
    boot_process_snapshot();
    for (unsigned i = 0; i < 10; i++) native_root_block_trace_write(boot_process_rows[i]);
    for (unsigned role = 0; role < 4; role++) {
      unsigned count = boot_ui_trace_role_rows(role,rows,64,2);
      for (unsigned i = 0; i < count; i++) native_root_block_trace_write(rows[i]);
    }
    unsigned count = boot_ui_trace_hot_rows(rows,64);
    for (unsigned i = 0; i < count; i++) native_root_block_trace_write(rows[i]);
    native_root_block_trace_write("[display state]");
    native_legacy_state_words(report,sizeof(report));native_root_block_trace_write(report);
    native_legacy_prepare_report(report,sizeof(report));native_root_block_trace_write(report);
    native_legacy_clients_dump(report,sizeof(report));native_root_block_trace_write(report);
    native_legacy_swap_timeline(report,sizeof(report));native_root_block_trace_write(report);
    native_root_block_trace_write("[snapshot complete]");
    if (!pass) IOSleep(25000);
  }
}
