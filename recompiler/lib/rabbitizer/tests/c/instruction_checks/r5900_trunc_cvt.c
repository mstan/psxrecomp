/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "rabbitizer.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>


typedef struct TestEntry {
    uint32_t word;
    bool gnuMode;
    const char *expectedStr;
} TestEntry;


const TestEntry entries[] = {
    { 0x4600600D, true, ".word       0x4600600D                   # trunc.w.s   $f0, $f12 # 00000000 <InstrIdType: CPU_COP1_FPUS>" },
    { 0x46006024, true, ".word       0x46006024                   # cvt.w.s     $f0, $f12 # 00000000 <InstrIdType: CPU_COP1_FPUS>" },
    { 0x4600600D, false, "trunc.w.s   $f0, $f12" },
    { 0x46006024, false, "cvt.w.s     $f0, $f12" },
};

int main() {
    int errorCount = 0;
    size_t i;

    for (i = 0; i < ARRAY_COUNT(entries); i++) {
        const TestEntry *entry = &entries[i];
        RabbitizerConfig_Cfg.toolchainTweaks.gnuMode = entry->gnuMode;
        RabbitizerInstruction instr;
        char *buffer;
        size_t bufferSize;

        RabbitizerInstructionR5900_init(&instr, entry->word, 0x80000000);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        bufferSize = RabbitizerInstruction_getSizeForBuffer(&instr, 0, 0);
        buffer = malloc(bufferSize + 1);
        assert(buffer != NULL);

        RabbitizerInstruction_disassemble(&instr, buffer, NULL, 0, 0);

        if (entry->expectedStr == NULL) {
            printf("Word '0x%08X' gnuMode '%s' doesn't have a expected str, got '%s'\n", entry->word, entry->gnuMode ? "true" : "false", buffer);
            errorCount++;
        } else if (strcmp(buffer, entry->expectedStr) != 0) {
            fprintf(stderr, "Error on word '0x%08X' gnuMode '%s'. Expected '%s', got '%s'\n", entry->word, entry->gnuMode ? "true" : "false", entry->expectedStr, buffer);
            errorCount++;
        }

        free(buffer);
        RabbitizerInstructionR5900_destroy(&instr);
    }

    return errorCount;
}
