param(
    [string]$Generator = "Visual Studio 17 2022",
    [ValidateSet("x64")][string]$Arch = "x64",
    [string]$GameRoot
)
$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot
try {
    if ($GameRoot) {
        python .\tools\generate_motion_curves.py --game-dir $GameRoot --output .\src\generated_motion_curves.h
        if ($LASTEXITCODE -ne 0) { throw "Curve generation failed: $LASTEXITCODE" }
    }
    if (-not (Test-Path -LiteralPath .\src\generated_motion_curves.h)) {
        throw 'Generate curves first: install tools/requirements.txt, then run ./build.ps1 -GameRoot "your game directory".'
    }
    cmake -B build -S . -G $Generator -A $Arch
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed: $LASTEXITCODE" }
    cmake --build build --config Release
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    Write-Host "Output: $PSScriptRoot\build\Release\version.dll"
}
finally { Pop-Location }
