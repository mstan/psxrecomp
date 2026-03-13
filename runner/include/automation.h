#ifndef AUTOMATION_H
#define AUTOMATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int      g_turbo;              /* 0=normal, 1=uncapped. Toggled by F5 or script */
extern uint32_t g_ps1_frame;          /* promoted from static in psx_present_frame */
extern int      g_snap_inject_requested; /* set by script inject-snapshot; cleared by main loop */

int  script_load(const char* path);
void script_tick(uint32_t ps1_frame, uint8_t* ram, uint8_t* scratch);
int  script_get_pad(void);           /* -1 = no override, else pad bitmask */
int  script_wants_screenshot(char* buf, int buflen);
int  script_check_exit(void);        /* -1 = keep running, else exit code */

#ifdef __cplusplus
}
#endif

#endif /* AUTOMATION_H */
