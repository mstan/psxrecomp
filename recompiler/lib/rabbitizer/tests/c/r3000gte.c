/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstructionR3000GTE.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


int main() {
    uint32_t word;
    RabbitizerInstruction instr;
    char *buffer;
    int extraLJust = 5;
#if 1
    uint32_t validbits;
#endif

    word = 0x4A180001; // RTPS

    RabbitizerInstructionR3000GTE_init(&instr, word, 0x00100000);

    RabbitizerInstructionR3000GTE_processUniqueId(&instr);

    buffer = malloc(RabbitizerInstruction_getSizeForBuffer(&instr, 0, extraLJust) + 1);
    assert(buffer != NULL);

    RabbitizerInstruction_disassemble(&instr, buffer, NULL, 0, extraLJust);

    printf("%08X: %s\n", word, buffer);

#if 1
    validbits = RabbitizerInstruction_getValidBits(&instr);

    printf("word:           %08X\n", instr.word);
    printf("mandatory bits: %08X\n", instr._mandatorybits);
    printf("valid bits:     %08X\n", validbits);
    printf("invalid bits:   %08X\n", (~validbits) & instr.word);
#endif

    free(buffer);
    RabbitizerInstructionR3000GTE_destroy(&instr);

    return 0;
}
