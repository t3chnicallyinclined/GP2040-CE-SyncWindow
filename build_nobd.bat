@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d C:\Users\trist\projects\GP2040-CE

REM Use SDK from build dir if it exists, otherwise fetch from git
if exist "build\_deps\pico_sdk-src" (
    set PICO_SDK_PATH=C:\Users\trist\projects\GP2040-CE\build\_deps\pico_sdk-src
    set FETCH_FLAG=-DFETCHCONTENT_FULLY_DISCONNECTED=on
) else (
    set PICO_SDK_PATH=
    set PICO_SDK_FETCH_FROM_GIT=on
    set PICO_SDK_FETCH_FROM_GIT_TAG=2.1.1
    set FETCH_FLAG=
)

echo [1/3] Building RP2040AdvancedBreakoutBoard...
set GP2040_BOARDCONFIG=RP2040AdvancedBreakoutBoard
set PICO_BOARD=pico
del build\CMakeCache.txt >nul 2>&1
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGP2040_BOARDCONFIG=RP2040AdvancedBreakoutBoard -DPICO_BOARD=pico -DSKIP_WEBBUILD=on %FETCH_FLAG% 2>&1
cmake --build build --config Release --parallel 2>&1
for %%f in (build\GP2040-CE-NOBD_*_RP2040AdvancedBreakoutBoard.uf2) do copy "%%f" release\ >nul 2>&1
echo   Done: RP2040AdvancedBreakoutBoard

echo [2/3] Building Pico...
set GP2040_BOARDCONFIG=Pico
set PICO_BOARD=pico
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGP2040_BOARDCONFIG=Pico -DPICO_BOARD=pico -DSKIP_WEBBUILD=on -DFETCHCONTENT_FULLY_DISCONNECTED=on 2>&1
cmake --build build --config Release --parallel 2>&1
for %%f in (build\GP2040-CE-NOBD_*_Pico.uf2) do copy "%%f" release\ >nul 2>&1
echo   Done: Pico

echo [3/3] Building PicoW...
set GP2040_BOARDCONFIG=PicoW
set PICO_BOARD=pico_w
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGP2040_BOARDCONFIG=PicoW -DPICO_BOARD=pico_w -DSKIP_WEBBUILD=on -DFETCHCONTENT_FULLY_DISCONNECTED=on 2>&1
cmake --build build --config Release --parallel 2>&1
for %%f in (build\GP2040-CE-NOBD_*_PicoW.uf2) do copy "%%f" release\ >nul 2>&1
echo   Done: PicoW

REM Pico2 (RP2350) removed — untested on RP2350 hardware, only releasing RP2040 builds

echo.
echo === Release files ===
dir /b release\GP2040-CE-NOBD_*.uf2
