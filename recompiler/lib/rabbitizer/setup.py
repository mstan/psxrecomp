# SPDX-FileCopyrightText: © 2022-2023 Decompollaborate
# SPDX-License-Identifier: MIT

from setuptools import setup, Extension
from pathlib import Path
import platform

bindingsPath = Path("rabbitizer")
srcPath = Path("src")

sourcesList = [str(x) for x in bindingsPath.glob("**/*.c")] + [str(x) for x in srcPath.glob("**/*.c")]
headersList = [str(x) for x in bindingsPath.glob("**/*.h")] + [str(x) for x in srcPath.glob("**/*.h")]

extraCompileArgs = ["-std=c11", "-Wall", "-g",]
if platform.system() == "Linux":
    extraCompileArgs += ["-Os", "-Wextra",]
    extraCompileArgs += ["-Werror=vla", "-Werror=switch", "-Werror=implicit-fallthrough", "-Werror=unused-function", "-Werror=unused-parameter", "-Werror=shadow", "-Werror=switch"]
    extraCompileArgs += ["-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types"]
    extraCompileArgs += ["-Werror"]

setup(
    ext_modules=[
        Extension(
            name="rabbitizer",
            sources=sourcesList,
            include_dirs=["include", "rabbitizer", "tables"],
            extra_compile_args = extraCompileArgs,
            depends=headersList,
        ),
    ],
)
