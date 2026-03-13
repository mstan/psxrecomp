/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_INSTRID_H
#define RABBITIZER_INSTRID_H
#pragma once

#include <stdbool.h>

#include "common/Utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "generated/InstrId_enum.h"

extern const char *RabbitizerInstrId_Names[];


CONST NODISCARD
bool RabbitizerInstrId_isValid(RabbitizerInstrId uniqueId);

CONST NODISCARD RETURNS_NON_NULL
const char *RabbitizerInstrId_getOpcodeName(RabbitizerInstrId uniqueId);


#ifdef __cplusplus
}
#endif

#endif
