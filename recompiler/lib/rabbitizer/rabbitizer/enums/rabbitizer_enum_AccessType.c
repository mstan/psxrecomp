/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "enums_utils.h"
#include "instructions/RabbitizerAccessType.h"


#define RAB_DEF_ACCESSTYPE(name) { "AccessType", #name , RAB_ACCESSTYPE_##name, false, NULL },

RabbitizerEnumMetadata rabbitizer_enum_AccessType_enumvalues[] = {
#include "tables/AccessType.inc"

    RAB_DEF_ACCESSTYPE(MAX)

    { 0 },
};

#undef RAB_DEF_ACCESSTYPE


static PyMethodDef rabbitizer_enum_AccessType_methods[] = {
    { 0 },
};

DEF_ENUM(AccessType, "")
