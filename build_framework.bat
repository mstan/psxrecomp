@echo off
REM Build the psxrecomp FRAMEWORK (recompiler tool + BIOS-only runtime).
REM Uses MSVC from VS Build Tools 2026 and its bundled CMake + Ninja (no PATH setup required).

set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cd /d D:\tools\psx-recomp || exit /b 1

echo ===== [1/4] Configure recompiler =====
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
echo ===== [2/4] Build recompiler =====
cmake --build recompiler/build || exit /b 1
echo ===== [3/4] Configure runtime =====
REM MSVC portability flags:
REM   /DNOMINMAX  - stop windows.h min/max macros clobbering std::min/std::max
REM   /EHsc       - C++ exception unwind semantics (restored after CXX_FLAGS override)
REM   /std:c11    - the runtime's C files use <stdatomic.h> (C11 atomics)
REM   /experimental:c11atomics - MSVC gates <stdatomic.h> behind this opt-in
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="/DNOMINMAX /EHsc" -DCMAKE_C_FLAGS="/DNOMINMAX /std:c11 /experimental:c11atomics" || exit /b 1
echo ===== [4/4] Build runtime (psx-runtime) =====
cmake --build runtime/build --target psx-runtime || exit /b 1

echo === BUILD OK ===
