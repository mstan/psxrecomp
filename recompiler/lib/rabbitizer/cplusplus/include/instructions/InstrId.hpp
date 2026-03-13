/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_INSTRID_HPP
#define RABBITIZER_INSTRID_HPP
#pragma once

#include <string>


namespace rabbitizer {
    namespace InstrId {
#include "generated/UniqueId_enum_class.hpp"

        std::string getOpcodeName(UniqueId uniqueId);
    };
};


#endif
