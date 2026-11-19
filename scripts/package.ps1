param(
    [string]$QtRoot = 'C:\Qt\6.12.0\mingw_64',
    [string]$CompilerRoot = 'C:\Qt\Tools\mingw1310_64',
    [string]$BinaryPath = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distRoot = Join-Path $repoRoot 'dist'
$releaseId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$stagingRoot = Join-Path $distRoot ('.staging-' + $releaseId)
$packagePath = Join-Path $stagingRoot 'Yiyi'
$finalPackagePath = Join-Path $distRoot 'Yiyi'
$backupRoot = Join-Path $repoRoot ('artifacts\package-backup-' + $releaseId)
$binaryPath = if ($BinaryPath) { [System.IO.Path]::GetFullPath($BinaryPath) } else { Join-Path $repoRoot 'build\Yiyi.exe' }
if (-not (Test-Path -LiteralPath $binaryPath)) { throw 'Build the application first.' }
$binaryDirectory = Split-Path $binaryPath -Parent
$ocrDirectory = Join-Path $binaryDirectory 'ocr'
$ocrWorker = Join-Path $binaryDirectory 'yiyi_ocr_worker.exe'
foreach ($requiredFile in @($ocrWorker, (Join-Path $ocrDirectory 'YiyiOcr.dll'), (Join-Path $ocrDirectory 'onnxruntime.dll'), (Join-Path $ocrDirectory 'det.onnx'), (Join-Path $ocrDirectory 'rec.onnx'), (Join-Path $ocrDirectory 'cls.onnx'), (Join-Path $ocrDirectory 'keys.txt'), (Join-Path $ocrDirectory 'checksums.json'))) {
    if (-not (Test-Path -LiteralPath $requiredFile)) { throw "OCR component missing: $requiredFile. Run scripts/setup-ocr.ps1 and build yiyi_ocr_worker first." }
}
foreach ($entry in (Get-Content -LiteralPath (Join-Path $ocrDirectory 'checksums.json') -Raw | ConvertFrom-Json)) {
    if ((Get-FileHash -LiteralPath (Join-Path $ocrDirectory $entry.file) -Algorithm SHA256).Hash -ne $entry.sha256) { throw "OCR runtime checksum mismatch: $($entry.file)" }
}
$pdfDirectory = Join-Path $binaryDirectory 'pdf'
$pdfWorker = Join-Path $binaryDirectory 'yiyi_pdf_worker.exe'
foreach ($requiredFile in @($pdfWorker, (Join-Path $pdfDirectory 'pdfium.dll'), (Join-Path $pdfDirectory 'qpdf\qpdf.dll'), (Join-Path $pdfDirectory 'YiyiPdfSans-Regular.otf'), (Join-Path $pdfDirectory 'YiyiPdfSerif-Regular.otf'), (Join-Path $pdfDirectory 'checksums.json'), (Join-Path $pdfDirectory 'pdf-dependencies.json'))) {
    if (-not (Test-Path -LiteralPath $requiredFile)) { throw "PDF component missing: $requiredFile. Run scripts/setup-pdf.ps1 and build yiyi_pdf_worker first." }
}
foreach ($entry in (Get-Content -LiteralPath (Join-Path $pdfDirectory 'checksums.json') -Raw | ConvertFrom-Json)) {
    if ((Get-FileHash -LiteralPath (Join-Path $pdfDirectory $entry.file) -Algorithm SHA256).Hash -ne $entry.sha256) { throw "PDF runtime checksum mismatch: $($entry.file)" }
}
$projectFile = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match($projectFile, 'project\(Yiyi VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b')
if (-not $versionMatch.Success) { throw 'Cannot read the application version from CMakeLists.txt.' }
$version = $versionMatch.Groups[1].Value
if ((Get-Item -LiteralPath $binaryPath).VersionInfo.FileVersion -ne $version) {
    throw 'The executable version does not match CMakeLists.txt. Rebuild before packaging.'
}

# All directory moves below must remain inside this workspace. Preserve the
# previous portable directory instead of deleting unknown files or user data.
function Assert-WorkspacePath([string]$Path) {
    $absolutePath = [System.IO.Path]::GetFullPath($Path)
    if (-not $absolutePath.StartsWith($repoRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to move a path outside the workspace: $absolutePath"
    }
}

$env:PATH = "$QtRoot\bin;" + $env:PATH
# Deploy into a new directory so old DLLs, test files and local settings can
# never leak into the release archive through a reused dist/Yiyi folder.
New-Item -ItemType Directory -Force -Path $packagePath | Out-Null
Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $packagePath 'Yiyi.exe') -Force
Copy-Item -LiteralPath $ocrWorker -Destination $packagePath -Force
Copy-Item -LiteralPath $ocrDirectory -Destination (Join-Path $packagePath 'ocr') -Recurse -Force
Copy-Item -LiteralPath $pdfWorker -Destination $packagePath -Force
$pdfTarget = Join-Path $packagePath 'pdf'
New-Item -ItemType Directory -Force -Path $pdfTarget | Out-Null
foreach ($name in @('pdfium.dll', 'YiyiPdfSans-Regular.otf', 'YiyiPdfSerif-Regular.otf', 'checksums.json', 'pdf-dependencies.json', 'licenses', 'qpdf')) {
    Copy-Item -LiteralPath (Join-Path $pdfDirectory $name) -Destination $pdfTarget -Recurse -Force
}
$deployLog = Join-Path $repoRoot 'build\deployment.log'
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime --qmldir (Join-Path $repoRoot 'qml') (Join-Path $packagePath 'Yiyi.exe') > $deployLog
if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $deployLog -Tail 40; throw 'Qt runtime deployment failed.' }
# The PDF worker uses Qt's font/PDF backend without a visible window.
Copy-Item -LiteralPath (Join-Path $QtRoot 'plugins\platforms\qoffscreen.dll') -Destination (Join-Path $packagePath 'platforms') -Force

# Use the runtime shipped with this Qt build. Its compiler can be newer than the
# separately installed MinGW used to compile the application.
foreach ($runtimeName in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path "$QtRoot\bin" $runtimeName) -Destination (Join-Path $packagePath $runtimeName) -Force
}
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'docs') -Filter '*.md' -File | Copy-Item -Destination $packagePath -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md') -Destination $packagePath -Force
$licensesPath = Join-Path $packagePath 'licenses'
New-Item -ItemType Directory -Force -Path $licensesPath | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'third_party\miniz\LICENSE') -Destination (Join-Path $licensesPath 'miniz-LICENSE.txt') -Force
$licenseRoot = Join-Path $QtRoot 'licenses'
if (Test-Path -LiteralPath $licenseRoot) { Copy-Item -Path (Join-Path $licenseRoot '*') -Destination $licensesPath -Recurse -Force }
$generalLicenses = [System.IO.Path]::GetFullPath((Join-Path $QtRoot '..\..\Licenses'))
if (Test-Path -LiteralPath $generalLicenses) { Copy-Item -Path (Join-Path $generalLicenses '*') -Destination $licensesPath -Recurse -Force }
$qtVersion = Split-Path (Split-Path $QtRoot -Parent) -Leaf
$qtInstallRoot = [System.IO.Path]::GetFullPath((Join-Path $QtRoot '..\..'))
$qtDocs = Join-Path $qtInstallRoot "Docs\Qt-$qtVersion\qtdoc"
foreach ($licenseFile in @('lgpl.html', 'gpl.html', 'qt-gpl-exception-1-0.html', 'licenses-used-in-qt.html')) {
    $licenseSource = Join-Path $qtDocs $licenseFile
    if (Test-Path -LiteralPath $licenseSource) { Copy-Item -LiteralPath $licenseSource -Destination $licensesPath -Force }
}
$compilerLicenses = Join-Path $CompilerRoot 'licenses'
if (Test-Path -LiteralPath $compilerLicenses) {
    $compilerLicenseTarget = Join-Path $licensesPath 'mingw'
    New-Item -ItemType Directory -Force -Path $compilerLicenseTarget | Out-Null
    Get-ChildItem -LiteralPath $compilerLicenses | Copy-Item -Destination $compilerLicenseTarget -Recurse -Force
}
@'
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
'@ | Set-Content -LiteralPath (Join-Path $packagePath 'qt.conf') -Encoding ASCII
$archiveName = "Yiyi-$version-windows-x64.zip"
$archivePath = Join-Path $stagingRoot $archiveName
Compress-Archive -LiteralPath $packagePath -DestinationPath $archivePath -CompressionLevel Optimal
$digest = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
($digest + '  ' + [System.IO.Path]::GetFileName($archivePath)) | Set-Content -LiteralPath ($archivePath + '.sha256') -Encoding ASCII

Assert-WorkspacePath $packagePath
Assert-WorkspacePath $finalPackagePath
Assert-WorkspacePath $backupRoot
if (Test-Path -LiteralPath $finalPackagePath) {
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    $previousPackage = Join-Path $backupRoot 'Yiyi'
    Assert-WorkspacePath $previousPackage
    try {
        Move-Item -LiteralPath $finalPackagePath -Destination $previousPackage
        Write-Output "Previous portable directory preserved: $previousPackage"
    } catch {
        # A running portable app locks its directory. Preserve it and publish
        # beside it, without terminating the user's process or changing its data.
        if (-not (Test-Path -LiteralPath $finalPackagePath) -or (Test-Path -LiteralPath $previousPackage)) { throw }
        $finalPackagePath = Join-Path $distRoot "Yiyi-$version"
        if (Test-Path -LiteralPath $finalPackagePath) { $finalPackagePath = Join-Path $distRoot "Yiyi-$version-$releaseId" }
        Assert-WorkspacePath $finalPackagePath
        Write-Warning "Existing portable directory could not be moved and has been preserved. New package: $finalPackagePath"
    }
}
try {
    Move-Item -LiteralPath $packagePath -Destination $finalPackagePath
} catch {
    if ((Test-Path -LiteralPath (Join-Path $backupRoot 'Yiyi')) -and -not (Test-Path -LiteralPath $finalPackagePath)) {
        Move-Item -LiteralPath (Join-Path $backupRoot 'Yiyi') -Destination $finalPackagePath
    }
    throw
}
foreach ($name in @($archiveName, ($archiveName + '.sha256'))) {
    $destination = Join-Path $distRoot $name
    $stagedFile = Join-Path $stagingRoot $name
    Assert-WorkspacePath $destination
    Assert-WorkspacePath $stagedFile
    if (Test-Path -LiteralPath $destination) {
        New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
        $previousFile = Join-Path $backupRoot $name
        Assert-WorkspacePath $previousFile
        Move-Item -LiteralPath $destination -Destination $previousFile
    }
    Move-Item -LiteralPath $stagedFile -Destination $destination
}
Remove-Item -LiteralPath $stagingRoot
Write-Output "Portable application: $finalPackagePath\Yiyi.exe"
Write-Output "Portable archive: $(Join-Path $distRoot $archiveName)"
