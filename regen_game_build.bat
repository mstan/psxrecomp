@echo off
REM Rebuild recompiler (game emitter fix), regenerate SmackDown game C, rebuild game.

set "VSBT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cd /d D:\tools\psx-recomp || exit /b 1
echo ===== [1/3] Rebuild recompiler (game emitter fix) =====
cmake --build recompiler/build || exit /b 1

echo ===== [2/3] Regenerate SmackDown game C =====
cd /d D:\tools\SmackDown2Recomp || exit /b 1
"D:\tools\psx-recomp\recompiler\build\psxrecomp-game.exe" --config game.toml || exit /b 1

echo ===== [3/3] Build SmackDown2Recomp =====
cmake --build build --target psx-runtime || exit /b 1

echo === GAME BUILD OK ===
