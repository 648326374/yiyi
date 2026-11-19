param([string]$OutputDirectory = '', [string]$PythonPath = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cache = Join-Path $repoRoot 'artifacts\pdf-deps'
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repoRoot 'build\pdf' }
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'pdf-dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $cache,$output | Out-Null
function Get-PdfDependency($Entry) {
    $path = Join-Path $cache $Entry.file
    if (-not (Test-Path -LiteralPath $path)) {
        $temporary = $path + '.download'
        Invoke-WebRequest -UseBasicParsing -Uri $Entry.url -OutFile $temporary -TimeoutSec 300
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Checksum mismatch: $($Entry.file)" }
        Move-Item -LiteralPath $temporary -Destination $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Cached checksum mismatch: $path" }
    return $path
}
$archive = Get-PdfDependency $manifest.pdfium
$font = Get-PdfDependency $manifest.font
$serifFont = Get-PdfDependency $manifest.serifFont
$fontTools = Get-PdfDependency $manifest.fonttools
if (-not $PythonPath) {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { throw 'Python 3 is needed only to prepare the PDF font. Specify -PythonPath.' }
    $PythonPath = $python.Source
}
$preparedFont = Join-Path $output 'YiyiPdfSans-Regular.otf'
& $PythonPath (Join-Path $repoRoot 'tools\prepare_pdf_font.py') $font $preparedFont --fonttools-wheel $fontTools
if ($LASTEXITCODE -ne 0) { throw 'PDF font preparation failed.' }
if ((Get-FileHash -LiteralPath $preparedFont -Algorithm SHA256).Hash -ne $manifest.preparedFont.sha256) { throw 'Prepared PDF font checksum mismatch.' }
$preparedSerif = Join-Path $output 'YiyiPdfSerif-Regular.otf'
& $PythonPath (Join-Path $repoRoot 'tools\prepare_pdf_font.py') $serifFont $preparedSerif --fonttools-wheel $fontTools --family Serif
if ($LASTEXITCODE -ne 0) { throw 'PDF serif font preparation failed.' }
if ((Get-FileHash -LiteralPath $preparedSerif -Algorithm SHA256).Hash -ne $manifest.preparedSerifFont.sha256) { throw 'Prepared serif font checksum mismatch.' }
$native = Join-Path $cache 'pdfium'
New-Item -ItemType Directory -Force -Path $native | Out-Null
& tar -xzf $archive -C $native
if ($LASTEXITCODE -ne 0) { throw 'PDFium extraction failed.' }
Copy-Item -LiteralPath (Join-Path $native 'bin\pdfium.dll') -Destination $output -Force
$headers = Join-Path $output 'include'
New-Item -ItemType Directory -Force -Path $headers | Out-Null
Get-ChildItem -LiteralPath (Join-Path $native 'include') -Filter '*.h' | Copy-Item -Destination $headers -Force
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $native 'LICENSE') -Destination (Join-Path $licenses 'PDFium-LICENSE.txt') -Force
Get-ChildItem -LiteralPath (Join-Path $native 'licenses') -File | Copy-Item -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NotoSans-LICENSE.txt') -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NotoSerif-LICENSE.txt') -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'pdf-dependencies.json') -Destination $output -Force
& (Join-Path $PSScriptRoot 'setup-pdf-merge.ps1') -OutputDirectory $output
Write-Output "PDF runtime ready: $output"
