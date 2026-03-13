/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerRegisterDescriptor.h"

#include "instructions/RabbitizerRegister.h"

#include "generated/RegisterDescriptor_Descriptors_arrays.h"

bool RabbitizerRegisterDescriptor_isClobberedByFuncCall(const RabbitizerRegisterDescriptor *self) {
    return self->isClobberedByFuncCall;
}
bool RabbitizerRegisterDescriptor_isReserved(const RabbitizerRegisterDescriptor *self) {
    return self->isReserved;
}
bool RabbitizerRegisterDescriptor_isKernel(const RabbitizerRegisterDescriptor *self) {
    return self->isKernel;
}
bool RabbitizerRegisterDescriptor_isZero(const RabbitizerRegisterDescriptor *self) {
    return self->isZero;
}
bool RabbitizerRegisterDescriptor_isAt(const RabbitizerRegisterDescriptor *self) {
    return self->isAt;
}
bool RabbitizerRegisterDescriptor_isReturnReg(const RabbitizerRegisterDescriptor *self) {
    return self->isReturnReg;
}
bool RabbitizerRegisterDescriptor_isRa(const RabbitizerRegisterDescriptor *self) {
    return self->isRa;
}
bool RabbitizerRegisterDescriptor_isStackPointer(const RabbitizerRegisterDescriptor *self) {
    return self->isStackPointer;
}
bool RabbitizerRegisterDescriptor_isGp(const RabbitizerRegisterDescriptor *self) {
    return self->isGp;
}
bool RabbitizerRegisterDescriptor_isTemp(const RabbitizerRegisterDescriptor *self) {
    return self->isTemp;
}
bool RabbitizerRegisterDescriptor_isArg(const RabbitizerRegisterDescriptor *self) {
    return self->isArg;
}
bool RabbitizerRegisterDescriptor_isSaved(const RabbitizerRegisterDescriptor *self) {
    return self->isSaved;
}
