/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_INSTR_ID_TYPE_H
#define RABBITIZER_INSTR_ID_TYPE_H
#pragma once

#include "common/Utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "generated/InstrIdType_enum.h"

extern const char *RabInstrIdType_Names[];


CONST NODISCARD RETURNS_NON_NULL
const char *RabInstrIdType_getName(RabInstrIdType idType);


#ifdef __cplusplus
}
#endif

#endif
