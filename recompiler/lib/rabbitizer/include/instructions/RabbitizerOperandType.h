/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_OPERAND_TYPE_H
#define RABBITIZER_OPERAND_TYPE_H
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "generated/OperandType_enum.h"

struct RabbitizerInstruction;

typedef size_t (*OperandCallback)(const struct RabbitizerInstruction *self, char *dst, const char *immOverride, size_t immOverrideLength);

extern const OperandCallback instrOpercandCallbacks[];

#include "generated/OperandType_function_declarations.h"

#ifdef __cplusplus
}
#endif

#endif
