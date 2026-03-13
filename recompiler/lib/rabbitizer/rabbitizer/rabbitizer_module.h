/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef PYRABBITIZER_MODULE_H
#define PYRABBITIZER_MODULE_H
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#include "rabbitizer_macro_utilities.h"
#include "enums/enums_utils.h"

#include "instructions/RabbitizerInstruction.h"
#include "analysis/RabbitizerLoPairingInfo.h"
#include "analysis/RabbitizerTrackedRegisterState.h"
#include "analysis/RabbitizerRegistersTracker.h"


// TODO: clean up this...


extern PyModuleDef rabbitizer_module;

PyObject *rabbitizer_submodule_Utils_Init(void);

extern PyTypeObject rabbitizer_global_config_TypeObject;

extern PyTypeObject rabbitizer_type_Enum_TypeObject;

DECL_RAB_TYPE(Instruction, instr)
DECL_RAB_TYPE(LoPairingInfo, pairingInfo)
DECL_RAB_TYPE(TrackedRegisterState, registerState)
DECL_RAB_TYPE(RegistersTracker, tracker)


DECL_ENUM(Abi)
DECL_ENUM(InstrCategory)
DECL_ENUM(InstrId)
DECL_ENUM(InstrIdType)
DECL_ENUM(OperandType)
DECL_ENUM(AccessType)

DECL_ENUM(RegGprO32)
DECL_ENUM(RegGprN32)

DECL_ENUM(RegCop1O32)
DECL_ENUM(RegCop1N32)
DECL_ENUM(RegCop1N64)

#endif
