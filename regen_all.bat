@echo off
REM Rebuild recompiler with the portable-constructor emitter fix, regenerate the
REM BIOS + game C, and rebuild the framework runtime. Run after editing the emitter.

set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;D:\DEV_TOOLS\Git\bin;%PATH%"

cd /d D:\tools\psx-recomp || exit /b 1
echo ===== [1/4] Rebuild recompiler (emitter fix) =====
cmake --build recompiler/build || exit /b 1

echo ===== [2/4] Regenerate BIOS C =====
bash tools/regen_bios.sh || exit /b 1

echo ===== [3/4] Regenerate SmackDown game C =====
cd /d D:\tools\SmackDown2Recomp || exit /b 1
"D:\tools\psx-recomp\recompiler\build\psxrecomp-game.exe" --config game.toml || exit /b 1

echo ===== [4/4] Rebuild framework runtime (BIOS) =====
cd /d D:\tools\psx-recomp || exit /b 1
cmake --build runtime/build --target psx-runtime || exit /b 1

echo === REGEN + RUNTIME OK ===
