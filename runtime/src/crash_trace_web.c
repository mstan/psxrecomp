#include "crash_trace.h"

#include <stdlib.h>

const char *g_psx_fatal_reason = NULL;

void psx_crash_trace_install_handlers(void) {}
void psx_crash_trace_dump(const char *reason, void *seh_info) {
    (void)reason;
    (void)seh_info;
}
void psx_crash_trace_set_exit_origin(const char *origin) { (void)origin; }
void psx_fatal_halt(const char *reason) {
    g_psx_fatal_reason = reason ? reason : "web_fatal";
    abort();
}
