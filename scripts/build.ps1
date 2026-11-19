param(
    [string]$QtRoot = 'C:\Qt\6.12.0\mingw_64',
    [string]$CompilerRoot = 'C:\Qt\Tools\mingw1310_64',
    [string]$NinjaPath = 'C:\Qt\Tools\Ninja\ninja.exe',
    [string]$PythonPath = '',
    [switch]$SkipTests,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath = Join-Path $repoRoot 'build'
$env:PATH = "$QtRoot\bin;$CompilerRoot\bin;" + $env:PATH
& (Join-Path $PSScriptRoot 'setup-ocr.ps1') -OutputDirectory (Join-Path $buildPath 'ocr')
& (Join-Path $PSScriptRoot 'setup-pdf.ps1') -OutputDirectory (Join-Path $buildPath 'pdf') -PythonPath $PythonPath

& cmake -S $repoRoot -B $buildPath -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCMAKE_CXX_COMPILER=$CompilerRoot\bin\g++.exe" "-DCMAKE_C_COMPILER=$CompilerRoot\bin\gcc.exe" "-DCMAKE_MAKE_PROGRAM=$NinjaPath" '-DBUILD_TESTING=ON'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $buildPath --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (-not $SkipTests) {
    & ctest --test-dir $buildPath --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
if ($Package) {
    & (Join-Path $PSScriptRoot 'package.ps1') -QtRoot $QtRoot -CompilerRoot $CompilerRoot
}
Write-Output 'Build finished.'
