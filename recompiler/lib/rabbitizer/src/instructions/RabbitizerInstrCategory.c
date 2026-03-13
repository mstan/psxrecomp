/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstrCategory.h"

#include <string.h>

#include "generated/InstrCategory_Names_array.h"

RabbitizerInstrCategory RabbitizerInstrCategory_fromStr(const char *name) {
    if (name == NULL) {
        return (RabbitizerInstrCategory)-2;
    }

    for (RabbitizerInstrCategory i = 0; i < RABBITIZER_INSTRCAT_MAX; i++) {
        if (strcmp(RabbitizerInstrCategory_Names[i], name) == 0) {
            return i;
        }
    }

    return (RabbitizerInstrCategory)-1;
}
