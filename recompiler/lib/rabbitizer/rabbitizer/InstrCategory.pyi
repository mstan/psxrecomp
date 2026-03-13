#!/usr/bin/env python3

# SPDX-FileCopyrightText: © 2022-2023 Decompollaborate
# SPDX-License-Identifier: MIT

# Automatically generated. DO NOT MODIFY

from __future__ import annotations
from .Enum import Enum
class InstrCategory:
    CPU: Enum
    RSP: Enum
    R3000GTE: Enum
    R5900: Enum
    MAX: Enum
    @staticmethod
    def fromStr(name: str | None) -> Enum|None: ...
