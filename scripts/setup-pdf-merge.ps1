param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cache = Join-Path $repoRoot 'artifacts\pdf-deps'
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repoRoot 'build\pdf' }
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'pdf-dependencies.json') -Raw | ConvertFrom-Json
$entry = $manifest.qpdf
New-Item -ItemType Directory -Force -Path $cache,$output | Out-Null
$archive = Join-Path $cache $entry.file
if (-not (Test-Path -LiteralPath $archive)) {
    $temporary = $archive + '.download'
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $entry.url -OutFile $temporary -TimeoutSec 1200
    } catch {
        # The release asset API is an alternate route to the same official file.
        Invoke-WebRequest -UseBasicParsing -Uri $entry.apiUrl -Headers @{Accept='application/octet-stream'} -OutFile $temporary -TimeoutSec 1200
    }
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $entry.sha256) { throw 'qpdf download checksum mismatch.' }
    Move-Item -LiteralPath $temporary -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $entry.sha256) { throw 'Cached qpdf checksum mismatch.' }
$extracted = Join-Path $cache ('qpdf-' + $entry.version)
Expand-Archive -LiteralPath $archive -DestinationPath $extracted -Force
$native = Join-Path $extracted ('qpdf-' + $entry.version + '-mingw64')
$runtime = Join-Path $output 'qpdf'
$headers = Join-Path $output 'include\qpdf'
$licenses = Join-Path $output 'licenses'
New-Item -ItemType Directory -Force -Path $runtime,$headers,$licenses | Out-Null
# The library uses the same MinGW runtime DLL names as Qt. Deploy the newer
# runtime shipped with Qt at the application root (package.ps1), not a second
# competing runtime in this directory. zlib, JPEG and OpenSSL are linked in.
Copy-Item -LiteralPath (Join-Path $native 'bin\qpdf30.dll') -Destination (Join-Path $runtime 'qpdf.dll') -Force
Get-ChildItem -LiteralPath (Join-Path $native 'include\qpdf') -File | Copy-Item -Destination $headers -Force
Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'qpdf-*.txt' -File | Copy-Item -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'qpdf-NOTICE.md') -Destination $licenses -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'pdf-dependencies.json') -Destination $output -Force

# Refresh only files present in this runtime. The standalone merge setup also
# works before the font/PDFium setup, and preserves hashes for existing fonts.
$files = @('pdfium.dll', 'YiyiPdfSans-Regular.otf', 'YiyiPdfSerif-Regular.otf', 'pdf-dependencies.json', 'qpdf/qpdf.dll')
$hashes = @($files | Where-Object { Test-Path -LiteralPath (Join-Path $output $_) } | ForEach-Object {
    @{file=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $output $_) -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'checksums.json') -Encoding UTF8
Write-Output "PDF merge runtime ready: $runtime"
