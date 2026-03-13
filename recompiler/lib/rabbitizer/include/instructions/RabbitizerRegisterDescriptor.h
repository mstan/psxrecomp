/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_REGISTERDESCRIPTOR_H
#define RABBITIZER_REGISTERDESCRIPTOR_H
#pragma once

#include <stdbool.h>

#include "common/Utils.h"

#ifdef __cplusplus
extern "C" {
#endif


// Please note the members of this struct may be renamed or removed without further notice.
// For consistent usage please use the functions instead
typedef struct RabbitizerRegisterDescriptor {
    bool isClobberedByFuncCall; // A function call invalidates the value hold by the register
    bool isReserved; // This register is not implemented and is reserved for future use
    bool isKernel; // Kernel-only registers ($k0, $k1)
    bool isZero; // $zero
    bool isAt; // $at
    bool isReturnReg; // $v0, $v1
    bool isRa; // $ra
    bool isStackPointer; // $sp
    bool isGp; // $gp
    bool isTemp; // $tX
    bool isArg; // $aX
    bool isSaved; // $sX
} RabbitizerRegisterDescriptor;


extern const RabbitizerRegisterDescriptor RabbitizerRegister_GprO32_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_GprN32_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_Cop0_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_Cop1O32_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_Cop1N32_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_Cop1N64_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_Cop1Control_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_Cop2_Descriptors[];

/* RSP */

extern const RabbitizerRegisterDescriptor RabbitizerRegister_RspGpr_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_RspCop0_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_RspCop2_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_RspCop2Control_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_RspVector_Descriptors[];

/* RSP */

/* R5900 */

extern const RabbitizerRegisterDescriptor RabbitizerRegister_R5900VF_Descriptors[];
extern const RabbitizerRegisterDescriptor RabbitizerRegister_R5900VI_Descriptors[];

/* R5900 */


NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isClobberedByFuncCall(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isReserved(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isKernel(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isZero(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isAt(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isReturnReg(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isRa(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isStackPointer(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isGp(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isTemp(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isArg(const RabbitizerRegisterDescriptor *self);
NODISCARD NON_NULL(1) PURE
bool RabbitizerRegisterDescriptor_isSaved(const RabbitizerRegisterDescriptor *self);


#ifdef __cplusplus
}
#endif

#endif
