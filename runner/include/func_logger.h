/*
 * func_logger.h — Runtime function discovery logger
 *
 * Tracks unique JAL/JALR targets discovered during interpreter execution.
 * Used in INTERPRETER_ONLY mode to capture function entry points for
 * feeding back to the recompiler.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the logger. Call once at startup. */
void func_logger_init(void);

/* Log a discovered function entry point.
 * addr = JAL/JALR target address
 * caller_pc = PC of the calling instruction
 * Deduplicates automatically. */
void func_logger_log(uint32_t addr, uint32_t caller_pc);

/* Write all discovered addresses to a file.
 * Format: one hex address per line (e.g., "0x80012345")
 * Sorted by address for easy diffing. */
void func_logger_dump(const char *path);

/* Shutdown and free resources. */
void func_logger_shutdown(void);

/* Return count of unique discovered addresses. */
int func_logger_count(void);

/* Build a JSON array of all discovered addresses into buf.
 * Returns number of bytes written. */
int func_logger_json(char *buf, int buf_size);

#ifdef __cplusplus
}
#endif
