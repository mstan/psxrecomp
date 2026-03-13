/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "rabbitizer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define STR_STARTS_WITH(str, prefix) (strncmp((str), (prefix), strlen((prefix))) == 0)
#define INSTR_OPCODE_PREFIX "jalr        "

int main() {
    uint32_t word = 0x00000009; // jalr
    RabbitizerInstruction instr;
    size_t rs;
    int errorCount = 0;

    for (rs = 0; rs < RAB_REGISTERS_COUNT; rs++) {
        uint32_t shifted_rs = RAB_INSTR_PACK_rs(0, rs);
        size_t rd;

        for (rd = 0; rd < RAB_REGISTERS_COUNT; rd++) {
            uint32_t shifted_rd = RAB_INSTR_PACK_rd(0, rd);
            char buffer[0x100];
            char *bufferPtr = buffer;
            uint32_t new_word = word | shifted_rd | shifted_rs;

            memset(buffer, 0, sizeof(buffer));

            RabbitizerInstruction_init(&instr, new_word, 0x80000000);
            RabbitizerInstruction_processUniqueId(&instr);

            RabbitizerInstruction_disassemble(&instr, buffer, NULL, 0, 0);

            if (!STR_STARTS_WITH(bufferPtr, INSTR_OPCODE_PREFIX)) {
                fprintf(stderr, "Error in word 0x%08X. Missing '%s'\n", new_word, INSTR_OPCODE_PREFIX);
                errorCount++;
            } else {
                const char *rsName = RabbitizerRegister_getNameGpr(rs);
                bufferPtr += strlen(INSTR_OPCODE_PREFIX);

                if (rd == RABBITIZER_REG_GPR_O32_ra) {
                    if (!STR_STARTS_WITH(bufferPtr, rsName)) {
                        fprintf(stderr, "Error in word 0x%08X. Expected '%s', got '%s'\n", new_word, rsName, bufferPtr);
                        errorCount++;
                    } else {
                        bufferPtr += strlen(rsName);

                        if (*bufferPtr != '\0') {
                            fprintf(stderr, "Error in word 0x%08X. Extraneous character found '%c'. String: '%s'\n", new_word, *bufferPtr, bufferPtr);
                            errorCount++;
                        }
                    }
                } else {
                    const char *rdName = RabbitizerRegister_getNameGpr(rd);

                    if (!STR_STARTS_WITH(bufferPtr, rdName)) {
                        fprintf(stderr, "Error in word 0x%08X. Expected '%s', got '%s'\n", new_word, rdName, bufferPtr);
                        errorCount++;
                    } else {
                        bufferPtr += strlen(rdName);

                        if (!STR_STARTS_WITH(bufferPtr, ", ")) {
                            fprintf(stderr, "Error in word 0x%08X. Expected '%s', got '%s'\n", new_word, ", ", bufferPtr);
                            errorCount++;
                        } else {
                            bufferPtr += strlen(", ");

                            if (!STR_STARTS_WITH(bufferPtr, rsName)) {
                                fprintf(stderr, "Error in word 0x%08X. Expected '%s', got '%s'\n", new_word, rsName, bufferPtr);
                                errorCount++;
                            } else {
                                bufferPtr += strlen(rsName);

                                if (*bufferPtr != '\0') {
                                    fprintf(stderr, "Error in word 0x%08X. Extraneous character found '%c'. String: '%s'\n", new_word, *bufferPtr, bufferPtr);
                                    errorCount++;
                                }
                            }
                        }
                    }
                }
            }

            RabbitizerInstruction_destroy(&instr);
        }
    }

    return errorCount;
}
