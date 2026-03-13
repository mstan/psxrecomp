/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "enums_utils.h"
#include "instructions/RabbitizerOperandType.h"


#define RAB_DEF_OPERAND(prefix, operand) { "OperandType", #prefix "_" #operand, RAB_OPERAND_##prefix##_##operand, false, NULL },

RabbitizerEnumMetadata rabbitizer_enum_OperandType_enumvalues[] = {
    RAB_DEF_OPERAND(ALL, INVALID)

#include "tables/OperandTypes.inc"

    RAB_DEF_OPERAND(ALL, MAX)

    { 0 },
};

#undef RAB_DEF_OPERAND

static PyMethodDef rabbitizer_enum_OperandType_methods[] = {
    { 0 },
};

DEF_ENUM(OperandType, "")
