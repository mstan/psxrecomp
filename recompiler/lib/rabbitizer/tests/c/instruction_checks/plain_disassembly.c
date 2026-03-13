/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
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
    { 0x3C088001, NULL, "lui         $t0, 0x8001" },
    { 0x25080E60, NULL, "addiu       $t0, $t0, 0xE60" },
    { 0x3C090002, NULL, "lui         $t1, 0x2" },
    { 0x25298DE0, NULL, "addiu       $t1, $t1, -0x7220" },
    { 0xAD000000, NULL, "sw          $zero, 0x0($t0)" },
    { 0xAD000004, NULL, "sw          $zero, 0x4($t0)" },
    { 0x21080008, NULL, "addi        $t0, $t0, 0x8" },
    { 0x2129FFF8, NULL, "addi        $t1, $t1, -0x8" },
    { 0x1520FFFB, NULL, "bnez        $t1, . + 4 + (-0x5 << 2)" },
    { 0x00000000, NULL, "nop" },
    { 0x3C0A8000, NULL, "lui         $t2, 0x8000" },
    { 0x254A0494, NULL, "addiu       $t2, $t2, 0x494" },
    { 0x3C1D8002, NULL, "lui         $sp, 0x8002" },
    { 0x01400008, NULL, "jr          $t2" },
    { 0x27BDF8C0, NULL, "addiu       $sp, $sp, -0x740" },

    { 0x3C018001, NULL, "lui         $at, 0x8001" },
    { 0x03E00008, NULL, "jr          $ra" },
    { 0xAC24E190, NULL, "sw          $a0, -0x1E70($at)" },

    { 0x3C018001, "%hi(D_8000E190)", "lui         $at, %hi(D_8000E190)" },
    { 0x03E00008, NULL,              "jr          $ra" },
    { 0xAC24E190, "%lo(D_8000E190)", "sw          $a0, %lo(D_8000E190)($at)" },

    { 0x0C001F24, NULL,        "jal         func_80007C90" },
    { 0x0C001F24, "some_func", "jal         some_func" },

    { 0x8F99805C, NULL,              "lw          $t9, -0x7FA4($gp)"},
    { 0x8F99805C, "%call16(strcmp)", "lw          $t9, %call16(strcmp)($gp)"},

    { 0x8F858028, NULL,                 "lw          $a1, -0x7FD8($gp)"},
    { 0x8F858028, "%got(STR_10007C90)", "lw          $a1, %got(STR_10007C90)($gp)"},

    // Invalid instructions
    { 0x44444444, NULL, ".word       0x44444444                   # cfc1        $a0, $8 # 00000444 <InstrIdType: CPU_COP1>" },
    { 0x77777777, NULL, ".word       0x77777777                   # INVALID     $k1, $s7, 0x7777 # 00000000 <InstrIdType: CPU_NORMAL>" },
    { 0xEEEEEEEE, NULL, ".word       0xEEEEEEEE                   # INVALID     $s7, $t6, -0x1112 # 00000000 <InstrIdType: CPU_NORMAL>" },
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

        RabbitizerInstruction_init(&instr, entry->word, 0);
        RabbitizerInstruction_processUniqueId(&instr);

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
        RabbitizerInstruction_destroy(&instr);
    }

    return errorCount;
}
