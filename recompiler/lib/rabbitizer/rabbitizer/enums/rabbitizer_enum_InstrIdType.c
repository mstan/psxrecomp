/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "enums_utils.h"
#include "instructions/RabbitizerInstrIdType.h"


#define RABBITIZER_DEF_INSTR_ID_TYPE(prefix, name) \
    { "InstrIdType", #prefix "_" #name, RAB_INSTR_ID_TYPE_##prefix##_##name, false, NULL },

RabbitizerEnumMetadata rabbitizer_enum_InstrIdType_enumvalues[] = {
    #include "tables/InstrIdType.inc"
    { 0 },
};

#undef RABBITIZER_DEF_INSTR_ID_TYPE

static PyMethodDef rabbitizer_enum_InstrIdType_methods[] = {
    { 0 },
};

DEF_ENUM(InstrIdType, "")
