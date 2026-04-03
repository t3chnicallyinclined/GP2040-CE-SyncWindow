@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d C:\Users\trist\projects\GP2040-CE

if exist "build\_deps\pico_sdk-src" (
    set PICO_SDK_PATH=C:\Users\trist\projects\GP2040-CE\build\_deps\pico_sdk-src
    set FETCH_FLAG=-DFETCHCONTENT_FULLY_DISCONNECTED=on
) else (
    set PICO_SDK_PATH=
    set PICO_SDK_FETCH_FROM_GIT=on
    set PICO_SDK_FETCH_FROM_GIT_TAG=2.1.1
    set FETCH_FLAG=
)

set GP2040_BOARDCONFIG=W6100EVBPico2
set PICO_BOARD=pico2
del build\CMakeCache.txt >nul 2>&1
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGP2040_BOARDCONFIG=W6100EVBPico2 -DPICO_BOARD=pico2 -DSKIP_WEBBUILD=on %FETCH_FLAG% 2>&1
cmake --build build --config Release --parallel 2>&1

echo.
echo === Output ===
for %%f in (build\GP2040-CE-NOBD_*_W6100EVBPico2.uf2) do echo %%f
