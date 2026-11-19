param(
    [Parameter(Mandatory=$true)][string]$Docx,
    [Parameter(Mandatory=$true)][string]$Pptx,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$ExportPdf
)
# Opt-in development check only. Use synthetic fixtures, never personal files.
$ErrorActionPreference = 'Stop'
$docxPath = [System.IO.Path]::GetFullPath($Docx)
$pptxPath = [System.IO.Path]::GetFullPath($Pptx)
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$report = [ordered]@{}
$word = $null; $document = $null; $powerpoint = $null; $presentation = $null
$hadPowerPoint = $false
$dontSave = 0
try {
    Write-Output 'Starting an isolated Word automation instance.'
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $word.AutomationSecurity = 3
    Write-Output 'Opening the Word test document read-only.'
    $document = $word.Documents.Open($docxPath, $false, $true, $false)
    Write-Output 'Word opened the document.'
    $report.wordParagraphs = $document.Paragraphs.Count
    $report.wordTables = $document.Tables.Count
    $report.wordOpened = $true
    if ($ExportPdf) {
        $wordPdf = Join-Path $outputPath 'word-native.pdf'
        $document.ExportAsFixedFormat($wordPdf, 17)
        Write-Output 'Word PDF export completed.'
        $report.wordPdfBytes = (Get-Item -LiteralPath $wordPdf).Length
    }
    $document.Close([ref]$dontSave)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($document); $document = $null
    $word.Quit([ref]$dontSave)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($word); $word = $null
    try {
        $active = [Runtime.InteropServices.Marshal]::GetActiveObject('PowerPoint.Application')
        $hadPowerPoint = $true
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($active)
    } catch {}
    $powerpoint = New-Object -ComObject PowerPoint.Application
    Write-Output 'Opening the PowerPoint test document read-only.'
    $presentation = $powerpoint.Presentations.Open($pptxPath, -1, 0, 0)
    $report.pptSlides = $presentation.Slides.Count
    $report.pptOpened = $true
    if ($ExportPdf) {
        $pptPdf = Join-Path $outputPath 'ppt-native.pdf'
        $presentation.SaveAs($pptPdf, 32)
        Write-Output 'PowerPoint PDF export completed.'
        $report.pptPdfBytes = (Get-Item -LiteralPath $pptPdf).Length
    }
    $presentation.Close()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($presentation); $presentation = $null
    if (-not $hadPowerPoint -and $powerpoint.Presentations.Count -eq 0) { $powerpoint.Quit() }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($powerpoint); $powerpoint = $null
    $report.aiCalls = 0
    $report.pdfExportPerformed = [bool]$ExportPdf
    $json = $report | ConvertTo-Json
    $json | Set-Content -LiteralPath (Join-Path $outputPath 'native-office-check.json') -Encoding UTF8
    Write-Output $json
} finally {
    if ($document) { try { $document.Close([ref]$dontSave) } catch {}; [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($document) }
    if ($word) { try { $word.Quit([ref]$dontSave) } catch {}; [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($word) }
    if ($presentation) { try { $presentation.Close() } catch {}; [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($presentation) }
    if ($powerpoint) {
        if (-not $hadPowerPoint) { try { if ($powerpoint.Presentations.Count -eq 0) { $powerpoint.Quit() } } catch {} }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($powerpoint)
    }
    [GC]::Collect(); [GC]::WaitForPendingFinalizers()
}
