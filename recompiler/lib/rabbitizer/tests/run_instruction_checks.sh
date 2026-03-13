#!/bin/bash

# SPDX-FileCopyrightText: © 2022-2023 Decompollaborate
# SPDX-License-Identifier: MIT

set -e

./build/tests/c/instruction_checks/jalr.elf
./build/tests/c/instruction_checks/plain_disassembly.elf
./build/tests/c/instruction_checks/r3000gte_disasm.elf
./build/tests/c/instruction_checks/r5900_trunc_cvt.elf
./build/tests/c/instruction_checks/r5900_vcallms.elf
