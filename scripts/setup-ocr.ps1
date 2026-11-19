param(
    [string]$OutputDirectory = '',
    [string]$Generator = 'Visual Studio 18 2026'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cache = Join-Path $repoRoot 'artifacts\ocr-deps'
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repoRoot 'build\ocr' }
$manifestPath = Join-Path $PSScriptRoot 'ocr-dependencies.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $cache,$output | Out-Null

function Get-PinnedFile($Entry, [string]$Directory) {
    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    $path = Join-Path $Directory $Entry.file
    if (-not (Test-Path -LiteralPath $path)) {
        $temporary = $path + '.download'
        $uri = if ($Entry.api) { $Entry.api } else { $Entry.url }
        try {
            Write-Host "Downloading $($Entry.file) from $uri"
            if ($Entry.api) {
                $response = Invoke-RestMethod -Uri $uri -TimeoutSec 1200
                [IO.File]::WriteAllBytes($temporary, [Convert]::FromBase64String($response.content))
            } else {
                Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $temporary -TimeoutSec 1200
            }
            if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $Entry.sha256) { throw 'SHA256 checksum mismatch.' }
            Move-Item -LiteralPath $temporary -Destination $path
        } catch {
            throw "Failed to download $($Entry.file) from ${uri}: $($_.Exception.Message)"
        } finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        }
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Cached checksum mismatch: $path" }
    return $path
}
$extractor = Get-PinnedFile $manifest.extractor $cache
$sourceArchive = Get-PinnedFile $manifest.rapidSource $cache
$ortArchive = Get-PinnedFile $manifest.onnxRuntime $cache
$source = Join-Path $cache 'source\Project_RapidOcrOnnx-1.2.2'
$ort = Join-Path $cache ('ort\' + [IO.Path]::GetFileNameWithoutExtension($ortArchive))
& $extractor x $sourceArchive ('-o' + (Join-Path $cache 'source')) -y -bso0
if ($LASTEXITCODE -ne 0) { throw 'RapidOCR source extraction failed.' }
New-Item -ItemType Directory -Force -Path $ort | Out-Null
& tar.exe -xf $ortArchive -C $ort 'build/native/include' 'runtimes/win-x64/native' 'LICENSE' 'ThirdPartyNotices.txt'
if ($LASTEXITCODE -ne 0) { throw 'ONNX Runtime package extraction failed.' }
foreach ($model in $manifest.models) { $file = Get-PinnedFile $model (Join-Path $cache 'models'); Copy-Item -LiteralPath $file -Destination (Join-Path $output $model.file) -Force }
$nativeBuild = Join-Path $repoRoot 'build\ocr-native'
& cmake -S (Join-Path $repoRoot 'tools\ocr-native') -B $nativeBuild -G $Generator -A x64 "-DRAPID_SOURCE=$source" "-DORT_ROOT=$ort"
if ($LASTEXITCODE -ne 0) { throw 'OCR CMake configuration failed. Install the Visual Studio C++ workload or select its generator.' }
& cmake --build $nativeBuild --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'OCR native build failed.' }
Copy-Item -LiteralPath (Join-Path $nativeBuild 'Release\YiyiOcr.dll') -Destination $output -Force
Get-ChildItem -LiteralPath (Join-Path $ort 'runtimes\win-x64\native') -Filter '*.dll' | Copy-Item -Destination $output -Force

# ONNX Runtime uses the Microsoft runtime. Deploy the redistributable app-local;
# the app remains usable on machines with only the extracted portable folder.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Cannot locate the Visual C++ redistributable files.' }
$redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -Directory | Where-Object { $_.Name -match '^\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
$crt = Get-ChildItem -LiteralPath (Join-Path $redist.FullName 'x64') -Directory -Filter '*.CRT' | Select-Object -First 1
if (-not $crt) { throw 'Cannot locate the x64 Microsoft CRT.' }
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $output -Force

$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $licenses 'RapidOcrOnnx-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $source 'opencv-static\windows-x64\LICENSE') -Destination (Join-Path $licenses 'OpenCV-LICENSE.txt') -Force
Get-ChildItem -LiteralPath (Join-Path $source 'opencv-static\windows-x64\etc\licenses') -File | Copy-Item -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $ort 'LICENSE') -Destination (Join-Path $licenses 'ONNXRuntime-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $ort 'ThirdPartyNotices.txt') -Destination (Join-Path $licenses 'ONNXRuntime-ThirdPartyNotices.txt') -Force
foreach ($license in $manifest.licenses) { $file = Get-PinnedFile $license $cache; Copy-Item -LiteralPath $file -Destination $licenses -Force }
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $output 'dependencies.json') -Force
$hashes = Get-ChildItem -LiteralPath $output -File | Where-Object { $_.Name -ne 'checksums.json' } | Get-FileHash -Algorithm SHA256 | ForEach-Object { @{file=[IO.Path]::GetFileName($_.Path); sha256=$_.Hash.ToLowerInvariant()} }
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'checksums.json') -Encoding UTF8
Write-Output "Native OCR runtime ready: $output"
