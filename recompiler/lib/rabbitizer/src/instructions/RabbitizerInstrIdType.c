/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstrIdType.h"

#include <assert.h>

#include "generated/InstrIdType_Names_array.h"

const char *RabInstrIdType_getName(RabInstrIdType idType) {
    const char *name;

    assert(idType >= RAB_INSTR_ID_TYPE_ALL_INVALID);
    assert(idType < RAB_INSTR_ID_TYPE_ALL_MAX);

    name = RabInstrIdType_Names[idType];
    assert(name != NULL);

    return name;
}
