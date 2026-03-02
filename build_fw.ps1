$ErrorActionPreference = "Continue"

# Import VS environment
$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
$tempFile = [IO.Path]::GetTempFileName()
cmd /c "`"$vsPath`" x64 && set > `"$tempFile`""
Get-Content $tempFile | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}
Remove-Item $tempFile

Set-Location "C:\Users\trist\projects\GP2040-CE"

# Override environment variable that CMakeLists.txt checks FIRST (line 39)
# This was set to RP2040_Advanced_Breakout system-wide, which overrides -D flags
$env:GP2040_BOARDCONFIG = "RP2040AdvancedBreakoutBoard"
Write-Host "Set GP2040_BOARDCONFIG=$env:GP2040_BOARDCONFIG"

# Verify config dir exists
$boardDir = "configs\RP2040AdvancedBreakoutBoard"
if (-not (Test-Path "$boardDir\BoardConfig.h")) {
    Write-Host "ERROR: $boardDir\BoardConfig.h not found!"
    exit 1
}
Write-Host "Board config found: $boardDir"

# Full clean
Write-Host "=== CLEAN ==="
if (Test-Path build) {
    Remove-Item -Recurse -Force build
    Start-Sleep -Seconds 2
}
if (Test-Path build) {
    Write-Host "ERROR: Could not remove build directory!"
    exit 1
}

Write-Host "=== CONFIGURE ==="
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DGP2040_BOARDCONFIG=RP2040AdvancedBreakoutBoard" -DPICO_SDK_FETCH_FROM_GIT=on
if ($LASTEXITCODE -ne 0) {
    Write-Host "CONFIGURE FAILED"
    exit 1
}

# Verify the config was set correctly
$cacheContent = Get-Content build\CMakeCache.txt | Select-String "PICO_BOARD_HEADER_DIRS"
Write-Host "Cache check: $cacheContent"

Write-Host "=== BUILD ==="
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    # Try fixing setuptools if venv exists
    $python = ".\build\venv\Scripts\python.exe"
    if (Test-Path $python) {
        Write-Host "=== FIXING SETUPTOOLS ==="
        & $python -m pip install "setuptools<81" --quiet
        Write-Host "=== BUILD (retry) ==="
        cmake --build build --config Release --parallel
        if ($LASTEXITCODE -ne 0) {
            Write-Host "BUILD FAILED"
            exit 1
        }
    } else {
        Write-Host "BUILD FAILED (no venv to fix)"
        exit 1
    }
}

Write-Host "=== BUILD COMPLETE ==="
Get-ChildItem build\GP2040-CE_*.uf2
