#include "freeze_heartbeat.h"

/* Browser builds have no host filesystem watchdog process. Fatal errors are
 * surfaced through the JavaScript console by Emscripten instead. */
void freeze_heartbeat_start(const char *backend_label) { (void)backend_label; }
void freeze_heartbeat_fatal_dump(const char *reason) { (void)reason; }
