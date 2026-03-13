/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "rabbitizer.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>

size_t strlen_null(const char *string) {
    if (string == NULL) {
        return 0;
    }
    return strlen(string);
}

typedef struct TestEntry {
    uint32_t word;
    const char *immOverride;
    const char *expectedStr;
} TestEntry;

const TestEntry entries[] = {
    { 0x4A000038, NULL, "vcallms     0x0" },
    { 0x4A004038, NULL, "vcallms     0x800" },
    { 0x4A008038, NULL, "vcallms     0x1000" },
    { 0x4A008838, NULL, "vcallms     0x1100" },
    { 0x4A009038, NULL, "vcallms     0x1200" },
    { 0x4A009838, NULL, "vcallms     0x1300" },
    { 0x4A00a038, NULL, "vcallms     0x1400" },
    { 0x4A07FFF8, NULL, "vcallms     0xFFF8" },
    { 0x4A080038, NULL, "vcallms     0x10000" },
    { 0x4A1F8038, NULL, "vcallms     0x3F000" },
    { 0x4A1FFFB8, NULL, "vcallms     0x3FFF0" },
};

int main() {
    int errorCount = 0;
    size_t i;

    for (i = 0; i < ARRAY_COUNT(entries); i++) {
        const TestEntry *entry = &entries[i];
        RabbitizerInstruction instr;
        char *buffer;
        size_t bufferSize;
        size_t immOverrideLength = strlen_null(entry->immOverride);

        RabbitizerInstructionR5900_init(&instr, entry->word, 0);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        bufferSize = RabbitizerInstruction_getSizeForBuffer(&instr, immOverrideLength, 0);
        buffer = malloc(bufferSize + 1);
        assert(buffer != NULL);

        RabbitizerInstruction_disassemble(&instr, buffer, entry->immOverride, immOverrideLength, 0);

        if (entry->expectedStr == NULL) {
            printf("Word '0x%08X' doesn't have a expected str, got '%s'\n", entry->word, buffer);
            errorCount++;
        } else if (strcmp(buffer, entry->expectedStr) != 0) {
            fprintf(stderr, "Error on word '0x%08X'. Expected '%s', got '%s'\n", entry->word, entry->expectedStr, buffer);
            errorCount++;
        }

        free(buffer);
        RabbitizerInstructionR5900_destroy(&instr);
    }

    return errorCount;
}
