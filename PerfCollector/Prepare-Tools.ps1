<#
.SYNOPSIS
    Run ONCE on a machine with internet / Windows ADK to fill tools\ before copying PerfCollector to a USB drive.
      - tools\procdump64.exe, procdump.exe      (Sysinternals, for -Dump)
      - tools\wpt\                              (WPR/xperf from the Windows Performance Toolkit, for
                                                 machines without inbox wpr.exe, e.g. Server 2012 R2 / Win 8.1)
.EXAMPLE
    .\Prepare-Tools.ps1
    .\Prepare-Tools.ps1 -WptSource "C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit"
#>
param(
    [string]$WptSource = "${env:ProgramFiles(x86)}\Windows Kits\10\Windows Performance Toolkit",
    [switch]$SkipDownload
)
$ErrorActionPreference = 'Stop'
$tools = Join-Path $PSScriptRoot 'tools'
New-Item -ItemType Directory -Path $tools -Force | Out-Null

if (-not $SkipDownload) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    foreach ($f in 'procdump64.exe', 'procdump.exe') {
        $dst = Join-Path $tools $f
        Write-Host "Downloading $f ..."
        try { Invoke-WebRequest -Uri "https://live.sysinternals.com/$f" -OutFile $dst -UseBasicParsing; Unblock-File $dst }
        catch { Write-Warning "Download of $f failed: $($_.Exception.Message). Get it from https://learn.microsoft.com/sysinternals/downloads/procdump" }
    }
}

if (Test-Path (Join-Path $WptSource 'wpr.exe')) {
    $dst = Join-Path $tools 'wpt'
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    # Recorder side only (wpr/xperf and their DLLs); WPA itself stays on the analysis machine.
    Get-ChildItem $WptSource -File | Where-Object { $_.Extension -in '.exe', '.dll', '.manifest', '.config' -and $_.Name -notmatch '^(wpa|WPAExporter|wpr?ui)' } |
        Copy-Item -Destination $dst -Force
    Write-Host "Copied WPT recorder files to $dst"
}
else {
    Write-Host "WPT not found at '$WptSource' (optional: Windows 10/11 and Server 2016+ have inbox wpr.exe)."
}
Get-ChildItem $tools -Recurse -File | Format-Table FullName, Length -AutoSize
