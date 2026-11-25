param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PythonPath = '',
    [switch]$SmokeTest,
    [switch]$Package,
    [switch]$SelfContained
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($Package -and $Configuration -ne 'Release') {
    throw 'Package the Release build to exclude development runtime dependencies.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Install Visual Studio with the Desktop development with C++ workload and Windows SDK 10.0.26100.0.'
}
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild and the x64 Visual C++ tools are required to build Yiyi WinUI.'
}

$vsVersion = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$generator = if (([version]$vsVersion).Major -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
$buildRoot = Join-Path $repoRoot 'build\winui'
$applicationRoot = if ($SelfContained) { Join-Path $repoRoot 'build\winui-self-contained' } else { $buildRoot }
$pdfRuntime = Join-Path $repoRoot 'build\pdf'
$pdfManifest = Join-Path $pdfRuntime 'pdf-dependencies.json'
if (!(Test-Path -LiteralPath (Join-Path $pdfRuntime 'include\fpdfview.h')) -or
    !(Test-Path -LiteralPath (Join-Path $pdfRuntime 'pdfium.dll')) -or
    !(Test-Path -LiteralPath (Join-Path $pdfRuntime 'YiyiPdfSans-Regular.otf')) -or
    !(Test-Path -LiteralPath (Join-Path $pdfRuntime 'YiyiPdfSerif-Regular.otf')) -or
    !(Test-Path -LiteralPath (Join-Path $pdfRuntime 'licenses\PDFium-LICENSE.txt')) -or
    !(Test-Path -LiteralPath $pdfManifest) -or
    ((Get-FileHash -LiteralPath $pdfManifest).Hash -ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'pdf-dependencies.json')).Hash)) {
    & (Join-Path $PSScriptRoot 'setup-pdf.ps1') -OutputDirectory $pdfRuntime -PythonPath $PythonPath
}
$pdfDependencies = Get-Content -LiteralPath $pdfManifest -Raw | ConvertFrom-Json
$qpdfVersion = $pdfDependencies.qpdf.version
$qpdfBin = Join-Path $repoRoot "artifacts\pdf-deps\qpdf-$qpdfVersion\qpdf-$qpdfVersion-mingw64\bin"
if (!(Test-Path -LiteralPath (Join-Path $qpdfBin 'qpdf30.dll')) -or
    !(Test-Path -LiteralPath (Join-Path $pdfRuntime 'include\qpdf\qpdf-c.h')) -or
    !(Test-Path -LiteralPath (Join-Path $pdfRuntime 'licenses\qpdf-LICENSE.txt'))) {
    & (Join-Path $PSScriptRoot 'setup-pdf-merge.ps1') -OutputDirectory $pdfRuntime
}
$ocrRuntime = Join-Path $repoRoot 'build\ocr'
$ocrCoreFiles = @('YiyiOcr.dll', 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'det.onnx', 'rec.onnx', 'cls.onnx', 'keys.txt',
    'dependencies.json', 'checksums.json')
$ocrManifest = Join-Path $ocrRuntime 'dependencies.json'
$missingOcr = $ocrCoreFiles | Where-Object { !(Test-Path -LiteralPath (Join-Path $ocrRuntime $_)) }
if ($missingOcr -or !(Test-Path -LiteralPath (Join-Path $ocrRuntime 'licenses\ONNXRuntime-LICENSE.txt')) -or
    ((Get-FileHash -LiteralPath $ocrManifest).Hash -ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'ocr-dependencies.json')).Hash)) {
    & (Join-Path $PSScriptRoot 'setup-ocr.ps1') -OutputDirectory $ocrRuntime -Generator $generator
}

function Build-WinUIWorker([string]$Name, [string[]]$Options) {
    $source = Join-Path $repoRoot "winui\$Name"
    $build = Join-Path $buildRoot "$Name-worker"
    & cmake -S $source -B $build -G $generator -A x64 @Options
    if ($LASTEXITCODE -ne 0) { throw "WinUI $Name worker configuration failed." }
    & cmake --build $build --config $Configuration --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "WinUI $Name worker build failed." }
}

function Copy-WinUIPdfRuntime([string]$Destination) {
    $pdfOutput = Join-Path $Destination 'pdf'
    $pdfLicenses = Join-Path $pdfOutput 'licenses'
    $qpdfOutput = Join-Path $pdfOutput 'qpdf'
    New-Item -ItemType Directory -Path $pdfLicenses,$qpdfOutput -Force | Out-Null
    $workerRuntime = Join-Path $buildRoot "pdf-worker\$Configuration\pdf"
    foreach ($name in @('pdfium.dll', 'YiyiPdfSans-Regular.otf', 'YiyiPdfSerif-Regular.otf')) {
        Copy-Item -LiteralPath (Join-Path $workerRuntime $name) -Destination $pdfOutput -Force
    }
    foreach ($name in @('qpdf.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $workerRuntime "qpdf\$name") -Destination $qpdfOutput -Force
    }
    Copy-Item -LiteralPath $pdfManifest -Destination $pdfOutput -Force
    Get-ChildItem -LiteralPath (Join-Path $pdfRuntime 'licenses') -File | Copy-Item -Destination $pdfLicenses -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses\gcc-GPL-3.0.txt'),
        (Join-Path $PSScriptRoot 'licenses\gcc-Runtime-Library-Exception.txt'),
        (Join-Path $PSScriptRoot 'licenses\winpthreads-LICENSE.txt') -Destination $pdfLicenses -Force
}

function Copy-WinUIOcrRuntime([string]$Destination) {
    $ocrOutput = Join-Path $Destination 'ocr'
    $licenses = Join-Path $ocrOutput 'licenses'
    New-Item -ItemType Directory -Path $licenses -Force | Out-Null
    foreach ($name in $ocrCoreFiles) {
        Copy-Item -LiteralPath (Join-Path $ocrRuntime $name) -Destination $ocrOutput -Force
    }
    Get-ChildItem -LiteralPath $ocrRuntime -Filter '*.dll' -File |
        Where-Object { $_.Name -match '^(concrt|msvcp|vccorlib|vcruntime)140(?:_[a-z0-9_]+)?\.dll$' } |
        Copy-Item -Destination $ocrOutput -Force
    Get-ChildItem -LiteralPath (Join-Path $ocrRuntime 'licenses') -File | Copy-Item -Destination $licenses -Force
}

Build-WinUIWorker 'pdf' @("-DYIYI_PDF_RUNTIME=$pdfRuntime", "-DYIYI_QPDF_TOOL_DIR=$qpdfBin", '-DBUILD_TESTING=OFF')
Build-WinUIWorker 'ocr'
Build-WinUIWorker 'office'

$project = Join-Path $repoRoot 'winui\Yiyi.WinUI.vcxproj'
$appLocal = $SelfContained.IsPresent.ToString().ToLowerInvariant()
$buildProperties = @('/v:minimal', "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:WindowsAppSDKSelfContained=$appLocal")

$auditSourceWarning = '\bNU1900\s*:'
$restoreOutput = & $msbuild $project /t:Restore @buildProperties
$restoreExitCode = $LASTEXITCODE
if ($restoreExitCode -eq 0 -and ($restoreOutput -match $auditSourceWarning)) {
    $restoreOutput | Where-Object { $_ -notmatch $auditSourceWarning }
    Write-Output 'NuGet audit data unavailable (NU1900). Retrying restore with fresh HTTP data.'
    & $msbuild $project /t:Restore @buildProperties /p:RestoreForce=true /p:RestoreNoHttpCache=true
    $restoreExitCode = $LASTEXITCODE
} else {
    $restoreOutput
}
if ($restoreExitCode -ne 0) { throw 'Yiyi WinUI NuGet restore failed.' }

& $msbuild $project /m @buildProperties
if ($LASTEXITCODE -ne 0) { throw 'Yiyi WinUI build failed.' }

function Test-WinUIStartup([string]$Executable) {
    $dataDir = Join-Path $repoRoot ('artifacts\winui-smoke-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
    $log = Join-Path $dataDir 'startup.log'
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.Arguments = '--smoke-test --data-dir "{0}"' -f $dataDir
    $startInfo.WorkingDirectory = Split-Path -Parent $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $startInfo.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $errorOutput = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "WinUI startup smoke test timed out after 30 seconds."
        }
        $message = $errorOutput.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($log, $message, [Text.Encoding]::UTF8)
        if ($process.ExitCode -ne 0) {
            Write-Output $message
            throw "WinUI startup smoke test failed with exit code $($process.ExitCode)."
        }
    } finally {
        $process.Dispose()
    }
    Write-Output 'WinUI startup smoke test passed.'
}

$output = Join-Path $applicationRoot $Configuration
$binary = Join-Path $output 'Yiyi.WinUI.exe'
Copy-Item -LiteralPath (Join-Path $buildRoot "pdf-worker\$Configuration\yiyi_pdf_worker.exe") -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $buildRoot "ocr-worker\$Configuration\yiyi_ocr_worker.exe") -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $buildRoot "office-worker\$Configuration\yiyi_word_preview.exe") -Destination $output -Force
Copy-WinUIPdfRuntime $output
Copy-WinUIOcrRuntime $output
Write-Output "WinUI application is ready at $binary"
if ($Package) {
    $packageStage = Join-Path $buildRoot ('package-' + [Guid]::NewGuid().ToString('N'))
    $folderName = if ($SelfContained) { 'Yiyi-WinUI-SelfContained' } else { 'Yiyi-WinUI' }
    $packagePath = Join-Path $packageStage $folderName
    $distRoot = Join-Path $repoRoot 'dist'
    $version = (Get-Item -LiteralPath $binary).VersionInfo.FileVersion
    $archiveSuffix = if ($SelfContained) { '-self-contained' } else { '' }
    $archiveName = "Yiyi-$version-winui-windows-x64$archiveSuffix.zip"
    $archive = Join-Path $distRoot $archiveName
    New-Item -ItemType Directory -Path $packagePath,$distRoot -Force | Out-Null
    try {
        $payload = @('Yiyi.WinUI.exe', 'Yiyi.WinUI.pri', 'App.xbf', 'MainWindow.xbf',
            'CppWinRT-LICENSE.txt', 'WindowsAppSDK-LICENSE.txt', 'yiyi_pdf_worker.exe', 'yiyi_ocr_worker.exe', 'yiyi_word_preview.exe')
        if ($SelfContained) {
            $runtimeFiles = @(Get-Content -LiteralPath (Join-Path $output 'WindowsAppSDK-files.txt'))
            if ($runtimeFiles -notcontains 'Microsoft.WindowsAppRuntime.dll') {
                throw 'The self-contained build did not produce the Windows App Runtime payload.'
            }
            $payload += $runtimeFiles
            $payload += @('Microsoft.Web.WebView2.Core.dll', 'WindowsAppSDK-NOTICE.txt')
        } else {
            $payload += 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
        }
        foreach ($name in ($payload | Sort-Object -Unique)) {
            $destination = Join-Path $packagePath $name
            $parent = Split-Path -Parent $destination
            if (!(Test-Path -LiteralPath $parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Copy-Item -LiteralPath (Join-Path $output $name) -Destination $destination
        }
        Copy-WinUIPdfRuntime $packagePath
        Copy-WinUIOcrRuntime $packagePath
        Copy-Item -LiteralPath (Join-Path $repoRoot 'third_party\miniz\LICENSE') -Destination (Join-Path $packagePath 'Miniz-LICENSE.txt')
        if ($SmokeTest) {
            Test-WinUIStartup (Join-Path $packagePath 'Yiyi.WinUI.exe')
        }
        $stagedArchive = Join-Path $packageStage $archiveName
        Compress-Archive -LiteralPath $packagePath -DestinationPath $stagedArchive -CompressionLevel Optimal
        Move-Item -LiteralPath $stagedArchive -Destination $archive -Force
        Write-Output "WinUI portable archive is ready at $archive"
    } finally {
        $resolvedStage = (Resolve-Path -LiteralPath $packageStage).Path
        if (-not $resolvedStage.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Package staging path is outside the WinUI build directory ($resolvedStage)."
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
} elseif ($SmokeTest) {
    Test-WinUIStartup $binary
}
