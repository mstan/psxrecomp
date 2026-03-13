#!/usr/bin/env python3

# SPDX-FileCopyrightText: © 2022-2023 Decompollaborate
# SPDX-License-Identifier: MIT

# Automatically generated. DO NOT MODIFY

from __future__ import annotations
from .Enum import Enum
class Abi:
    NUMERIC: Enum
    O32: Enum
    N32: Enum
    N64: Enum
    @staticmethod
    def fromStr(name: str | None) -> Enum: ...
