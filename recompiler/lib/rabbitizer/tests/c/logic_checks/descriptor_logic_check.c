/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

/**
 * Checks every instruction descriptor to not violate any logic assumption
 */

#include "rabbitizer.h"

#include <stdio.h>


#define LOGIC_ERROR(uniqueId, errorCount, successCondition) \
    if (!(successCondition)) { \
        fprintf(stderr, "Logic error on %s (%i): \n", RabbitizerInstrId_getOpcodeName(uniqueId), uniqueId); \
        fprintf(stderr, "\t%s\n", #successCondition); \
        errorCount++; \
    }

#define LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, successConditionA, successConditionB) \
    LOGIC_ERROR(uniqueId, errorCount, ((successConditionA) && (successConditionB)) || !(successConditionA))


int main() {
    int errorCount = 0;
    RabbitizerInstrId uniqueId;

    for (uniqueId = 0; uniqueId < RABBITIZER_INSTR_ID_ALL_MAX; uniqueId++) {
        const RabbitizerInstrDescriptor *descriptor = &RabbitizerInstrDescriptor_Descriptors[uniqueId];

        if (!RabbitizerInstrId_isValid(uniqueId)) {
            continue;
        }

        // An isBranchLikely must be marked as isBranch
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->isBranchLikely, descriptor->isBranch);

        // An isJumpWithAddress must be marked as isJump
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->isJumpWithAddress, descriptor->isJump);

        // An instruction should have at either isBranch, isJump, isTramp or none of them
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->isBranch && descriptor->isJump));
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->isBranch && descriptor->isTrap));
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->isJump && descriptor->isTrap));

        // An isDouble must be marked as isFloat
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->isDouble, descriptor->isFloat);

        // modifiesR* and readsR*
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->modifiesRs && descriptor->readsRs));
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->modifiesRt && descriptor->readsRt));
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->modifiesRd && descriptor->readsRd));
        LOGIC_ERROR(uniqueId, errorCount, !((descriptor->modifiesRs || descriptor->readsRs) && !RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_rs)));
        LOGIC_ERROR(uniqueId, errorCount, !((descriptor->modifiesRt || descriptor->readsRt) && !RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_rt)));
        LOGIC_ERROR(uniqueId, errorCount, !((descriptor->modifiesRd || descriptor->readsRd) && !RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_rd)));

        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_rs), (descriptor->readsRs || descriptor->modifiesRs));
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_rt), (descriptor->readsRt || descriptor->modifiesRt));
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_rd), (descriptor->readsRd || descriptor->modifiesRd));

        // float modifiesR* and readsR*
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->modifiesFs && descriptor->readsFs));
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->modifiesFt && descriptor->readsFt));
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->modifiesFd && descriptor->readsFd));
        LOGIC_ERROR(uniqueId, errorCount, !((descriptor->modifiesFs || descriptor->readsFs) && !RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_fs)));
        LOGIC_ERROR(uniqueId, errorCount, !((descriptor->modifiesFt || descriptor->readsFt) && !RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_ft)));
        LOGIC_ERROR(uniqueId, errorCount, !((descriptor->modifiesFd || descriptor->readsFd) && !RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_fd)));

        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_fs), (descriptor->readsFs || descriptor->modifiesFs));
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_ft), (descriptor->readsFt || descriptor->modifiesFt));
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, RabbitizerInstrDescriptor_hasOperandAlias(descriptor, RAB_OPERAND_cpu_fd), (descriptor->readsFd || descriptor->modifiesFd));


        // An instruction should have at most canBeHi or canBeLo, not both
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->canBeHi && descriptor->canBeLo));

        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->doesLink, (descriptor->isBranch || descriptor->isJump));

        // A doesDereference must have either doesLoad or doesStore
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->doesDereference, (descriptor->doesLoad || descriptor->doesStore));

        // A doesLoad must have a doesDereference
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->doesLoad, descriptor->doesDereference);
        // A doesStore must have a doesDereference
        LOGIC_ERROR_A_IMPLIES_B(uniqueId, errorCount, descriptor->doesStore, descriptor->doesDereference);

        // A instruction should not have both doesLoad and doesStore
        LOGIC_ERROR(uniqueId, errorCount, !(descriptor->doesLoad && descriptor->doesStore));
    }

    if (errorCount != 0) {
        printf("Descriptor had %i logic errors\n", errorCount);
    } else {
        printf("No logic errors on descriptor!\n");
    }

    return errorCount;
}
