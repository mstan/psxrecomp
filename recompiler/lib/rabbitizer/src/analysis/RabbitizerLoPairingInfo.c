/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "analysis/RabbitizerLoPairingInfo.h"

void RabbitizerLoPairingInfo_Init(RabbitizerLoPairingInfo *self) {
    *self = (RabbitizerLoPairingInfo){
        .instrOffset = 0,
        .value = 0,
        .shouldProcess = false,
        .isGpRel = false,
        .isGpGot = false,
    };
}
