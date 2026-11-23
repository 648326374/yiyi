param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PythonPath = '',
    [switch]$SmokeTest,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-winui.ps1') @PSBoundParameters
