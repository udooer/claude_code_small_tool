<#
.SYNOPSIS
    Portable performance log collector for diagnosing abnormal process CPU usage on Windows.

.DESCRIPTION
    Collects, in one run:
      - ETW trace (WPR, or xperf fallback) with CPU sampling + context switch stacks -> open in WPA
      - Performance counters (system / per-core / per-process / memory / disk / network) -> CSV
      - Process and per-thread CPU snapshots over the capture window
      - System information (OS, hotfixes, CPU, BIOS, power plan, drivers, services, software, AV ...)
      - Event logs (System / Application / performance related channels)
      - Optional full memory dumps of target processes (needs tools\procdump)
      - summary.txt with top CPU consumers and automatic hints
    Everything is packed into a single zip next to the tool (output\).

    Only uses components shipped with Windows (PowerShell 3.0+, wpr.exe, typeperf, wevtutil).
    Binaries placed under tools\ (wpt\wpr.exe, wpt\xperf.exe, procdump*.exe) are preferred when present.

.EXAMPLE
    .\Collect-PerfLogs.ps1                                  # 60 s, Standard level
.EXAMPLE
    .\Collect-PerfLogs.ps1 -ProcessName myapp -Duration 120 -Dump
.EXAMPLE
    .\Collect-PerfLogs.ps1 -Trigger -ProcessName myapp -CpuThreshold 30 -SustainSeconds 10
#>
[CmdletBinding()]
param(
    # Capture length in seconds (fixed mode) or post-trigger length is -PostTriggerSeconds.
    [ValidateRange(5, 3600)][int]$Duration = 60,

    # Target process names (without .exe) and/or PIDs. Used for trigger, thread detail, modules and dumps.
    # Comma separated values are accepted too (powershell -File passes arrays as one string).
    [string[]]$ProcessName,
    [string[]]$ProcessId,

    # Quick = CPU only, Standard = CPU + Disk/File IO, Full = GeneralProfile + CPU + IO + Registry + Network.
    [ValidateSet('Quick', 'Standard', 'Full')][string]$Level = 'Standard',

    # Trigger mode: record into a circular memory buffer and stop automatically when CPU stays high.
    [switch]$Trigger,
    # Percent of the WHOLE machine (like Task Manager). Applies to target processes if given, else total CPU.
    [ValidateRange(1, 100)][int]$CpuThreshold = 80,
    [ValidateRange(1, 3600)][int]$SustainSeconds = 10,
    [ValidateRange(0, 600)][int]$PostTriggerSeconds = 15,
    [ValidateRange(1, 10080)][int]$MaxWaitMinutes = 1440,

    [ValidateRange(1, 60)][int]$SampleInterval = 1,
    [ValidateRange(0, 365)][int]$EventLogDays = 7,

    # Take full dumps (procdump -ma, 3 dumps 10 s apart) of target processes. Needs tools\procdump64.exe.
    [switch]$Dump,
    # Skip the ETW trace (counters + info only, lowest overhead).
    [switch]$NoTrace,
    [switch]$NoZip,
    [string]$OutputDir,

    # Show a simple menu (used when Collect.cmd is double-clicked).
    [switch]$Interactive,
    [switch]$PauseAtEnd
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'
$ScriptVersion = '1.0.0'
$ToolRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$ToolsDir = Join-Path $ToolRoot 'tools'

#region ---------- helpers ----------

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Write-Log {
    param([string]$Message, [ValidateSet('INFO', 'WARN', 'ERROR', 'STEP')][string]$Kind = 'INFO')
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'HH:mm:ss'), $Kind, $Message
    $color = @{ INFO = 'Gray'; WARN = 'Yellow'; ERROR = 'Red'; STEP = 'Cyan' }[$Kind]
    Write-Host $line -ForegroundColor $color
    if ($script:LogFile) { Add-Content -Path $script:LogFile -Value $line -Encoding UTF8 }
}

function Get-Wmi {
    param([string]$Class, [string]$Filter)
    try {
        if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
            if ($Filter) { Get-CimInstance -ClassName $Class -Filter $Filter -ErrorAction Stop }
            else { Get-CimInstance -ClassName $Class -ErrorAction Stop }
        }
        else {
            if ($Filter) { Get-WmiObject -Class $Class -Filter $Filter -ErrorAction Stop }
            else { Get-WmiObject -Class $Class -ErrorAction Stop }
        }
    }
    catch { Write-Log "WMI $Class failed: $($_.Exception.Message)" WARN }
}

# Run a script block and save its output as text. Never throws.
function Save-Text {
    param([string]$File, [scriptblock]$Block)
    $path = Join-Path $script:InfoDir $File
    try { & $Block 2>&1 | Out-File -FilePath $path -Encoding UTF8 -Width 4096 }
    catch { "ERROR: $($_.Exception.Message)" | Out-File -FilePath $path -Encoding UTF8 }
}

function Save-Csv {
    param([string]$File, [scriptblock]$Block)
    $path = Join-Path $script:InfoDir $File
    try { & $Block | Export-Csv -Path $path -NoTypeInformation -Encoding UTF8 }
    catch { "ERROR: $($_.Exception.Message)" | Out-File -FilePath ($path + '.error.txt') -Encoding UTF8 }
}

function Get-System32Path {
    # 32-bit host on 64-bit OS would be redirected to SysWOW64, which has no wpr.exe.
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
        return Join-Path $env:SystemRoot 'Sysnative'
    }
    Join-Path $env:SystemRoot 'System32'
}

function Find-Tool {
    param([string[]]$Names, [switch]$SystemToo)
    foreach ($n in $Names) {
        foreach ($dir in @((Join-Path $ToolsDir 'wpt'), $ToolsDir)) {
            $p = Join-Path $dir $n
            if (Test-Path $p) { return $p }
        }
    }
    if ($SystemToo) {
        foreach ($n in $Names) {
            $p = Join-Path (Get-System32Path) $n
            if (Test-Path $p) { return $p }
        }
    }
    $null
}

function Invoke-Native {
    # Runs a console program, logs its output, returns exit code.
    param([string]$Exe, [string[]]$Arguments, [string]$OutFile)
    Write-Log ("> {0} {1}" -f (Split-Path -Leaf $Exe), ($Arguments -join ' '))
    $out = & $Exe @Arguments 2>&1
    $code = $LASTEXITCODE
    if ($OutFile) { $out | Out-File -FilePath $OutFile -Encoding UTF8 -Append }
    foreach ($l in $out) { if ("$l".Trim()) { Write-Log "  $l" } }
    $code
}

#endregion

#region ---------- performance counter name localization ----------

# typeperf only accepts counter names in the OS display language, so translate the English
# names through the Perflib registry tables. Falls back to the English name when not found.
function Get-PerfNameTranslator {
    $map = @{}
    try {
        $base = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib'
        $en = (Get-ItemProperty -Path "$base\009" -Name Counter -ErrorAction Stop).Counter
        $loc = (Get-ItemProperty -Path "$base\CurrentLanguage" -Name Counter -ErrorAction Stop).Counter
        $locById = @{}
        for ($i = 0; $i -lt $loc.Count - 1; $i += 2) { $locById[$loc[$i]] = $loc[$i + 1] }
        for ($i = 0; $i -lt $en.Count - 1; $i += 2) {
            $name = $en[$i + 1]
            if (-not $map.ContainsKey($name) -and $locById.ContainsKey($en[$i])) { $map[$name] = $locById[$en[$i]] }
        }
    }
    catch { Write-Log "Counter name translation unavailable ($($_.Exception.Message)); using English names" WARN }
    $map
}

function Convert-CounterPath {
    param([string]$Path, [hashtable]$Map)
    if ($Path -notmatch '^\\([^\\(]+)(\([^)]*\))?\\(.+)$') { return $Path }
    $obj = $Matches[1]; $inst = $Matches[2]; $ctr = $Matches[3]
    if ($Map.ContainsKey($obj)) { $obj = $Map[$obj] }
    if ($Map.ContainsKey($ctr)) { $ctr = $Map[$ctr] }
    '\{0}{1}\{2}' -f $obj, $inst, $ctr
}

$CounterList = @(
    '\Processor Information(*)\% Processor Time'
    '\Processor Information(*)\% Privileged Time'
    '\Processor Information(*)\% User Time'
    '\Processor Information(*)\% Interrupt Time'
    '\Processor Information(*)\% DPC Time'
    '\Processor Information(*)\% Processor Performance'
    '\Processor Information(*)\Processor Frequency'
    '\System\Processor Queue Length'
    '\System\Context Switches/sec'
    '\System\System Calls/sec'
    '\System\Processes'
    '\System\Threads'
    '\Memory\Available MBytes'
    '\Memory\% Committed Bytes In Use'
    '\Memory\Committed Bytes'
    '\Memory\Pages/sec'
    '\Memory\Page Faults/sec'
    '\Memory\Pool Nonpaged Bytes'
    '\Memory\Pool Paged Bytes'
    '\PhysicalDisk(*)\% Disk Time'
    '\PhysicalDisk(*)\Avg. Disk Queue Length'
    '\PhysicalDisk(*)\Avg. Disk sec/Read'
    '\PhysicalDisk(*)\Avg. Disk sec/Write'
    '\PhysicalDisk(*)\Disk Bytes/sec'
    '\Network Interface(*)\Bytes Total/sec'
    '\Process(*)\% Processor Time'
    '\Process(*)\% Privileged Time'
    '\Process(*)\ID Process'
    '\Process(*)\Thread Count'
    '\Process(*)\Handle Count'
    '\Process(*)\Private Bytes'
    '\Process(*)\Working Set'
    '\Process(*)\IO Data Bytes/sec'
    '\Process(*)\Page Faults/sec'
)

#endregion

#region ---------- process / thread snapshots ----------

function Get-ProcessTimes {
    # pid -> snapshot of CPU time. Locale independent, used for trigger and window statistics.
    $h = @{}
    foreach ($p in Get-Process) {
        if ($p.Id -eq 0) { continue }
        $cpu = $null
        try { $cpu = $p.TotalProcessorTime.TotalMilliseconds } catch { }
        if ($null -ne $cpu) { $h[$p.Id] = New-Object PSObject -Property @{ Id = $p.Id; Name = $p.ProcessName; CpuMs = $cpu } }
    }
    $h
}

function Get-ThreadTimes {
    param([int[]]$Pids)
    $list = @()
    foreach ($procId in $Pids) {
        $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if (-not $p) { continue }
        foreach ($t in $p.Threads) {
            try {
                $wait = ''
                if ("$($t.ThreadState)" -eq 'Wait') { $wait = "$($t.WaitReason)" }
                $list += New-Object PSObject -Property @{
                    Key = "$procId/$($t.Id)"; Pid = $procId; Process = $p.ProcessName; Tid = $t.Id
                    CpuMs = $t.TotalProcessorTime.TotalMilliseconds
                    UserMs = $t.UserProcessorTime.TotalMilliseconds
                    Priority = $t.CurrentPriority; State = "$($t.ThreadState)"; WaitReason = $wait
                    StartAddress = ('0x{0:X}' -f [int64]$t.StartAddress)
                }
            }
            catch { }
        }
    }
    $list
}

function Save-ProcessSnapshot {
    param([string]$File)
    Save-Csv $File {
        Get-Wmi Win32_Process | Select-Object ProcessId, ParentProcessId, Name, SessionId, Priority,
            ThreadCount, HandleCount,
            @{ n = 'WorkingSetMB'; e = { [math]::Round($_.WorkingSetSize / 1MB, 1) } },
            @{ n = 'PrivateMB'; e = { [math]::Round($_.PrivatePageCount / 1MB, 1) } },
            @{ n = 'KernelSec'; e = { [math]::Round($_.KernelModeTime / 1e7, 1) } },
            @{ n = 'UserSec'; e = { [math]::Round($_.UserModeTime / 1e7, 1) } },
            CreationDate, ExecutablePath, CommandLine
    }
}

function Resolve-Targets {
    $pids = @()
    foreach ($i in @($ProcessId | ForEach-Object { "$_" -split ',' })) { if ($i.Trim() -match '^\d+$') { $pids += [int]$i.Trim() } }
    foreach ($n in @($ProcessName | ForEach-Object { "$_" -split ',' })) {
        $n = $n.Trim() -replace '\.exe$', ''
        if (-not $n) { continue }
        $pids += @(Get-Process -Name $n -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    }
    @($pids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue } | Sort-Object -Unique)
}

#endregion

#region ---------- collectors ----------

function Collect-SystemInfo {
    Write-Log 'Collecting system information' STEP
    Save-Text 'systeminfo.txt' { systeminfo.exe }
    Save-Text 'os.txt' { Get-Wmi Win32_OperatingSystem | Format-List * }
    Save-Text 'hardware.txt' {
        '==== ComputerSystem ===='; Get-Wmi Win32_ComputerSystem | Format-List *
        '==== Processor ===='; Get-Wmi Win32_Processor | Format-List *
        '==== BIOS ===='; Get-Wmi Win32_BIOS | Format-List *
        '==== BaseBoard ===='; Get-Wmi Win32_BaseBoard | Format-List Manufacturer, Product, Version
        '==== PhysicalMemory ===='; Get-Wmi Win32_PhysicalMemory | Format-Table BankLabel, Capacity, Speed, ConfiguredClockSpeed, Manufacturer, PartNumber -AutoSize
        '==== VideoController ===='; Get-Wmi Win32_VideoController | Format-List Name, DriverVersion, DriverDate
    }
    Save-Csv 'hotfix.csv' { Get-Wmi Win32_QuickFixEngineering | Select-Object HotFixID, Description, InstalledOn, InstalledBy }
    Save-Text 'powercfg.txt' {
        '==== active scheme ===='; powercfg.exe /getactivescheme
        '==== schemes ===='; powercfg.exe /list
        '==== active scheme details ===='; powercfg.exe /query
    }
    Save-Text 'drivers.csv' { driverquery.exe /v /fo csv }
    Save-Csv 'drivers_signed.csv' { Get-Wmi Win32_PnPSignedDriver | Select-Object DeviceName, Manufacturer, DriverVersion, DriverDate, InfName, DeviceID }
    Save-Csv 'services.csv' { Get-Wmi Win32_Service | Select-Object Name, DisplayName, State, StartMode, ProcessId, StartName, PathName }
    Save-Text 'tasklist_svc.txt' { tasklist.exe /svc }
    Save-Csv 'software.csv' {
        $keys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
        Get-ItemProperty $keys -ErrorAction SilentlyContinue | Where-Object { $_.PSObject.Properties['DisplayName'] -and $_.DisplayName } |
            Select-Object DisplayName, DisplayVersion, Publisher, InstallDate | Sort-Object DisplayName
    }
    Save-Csv 'startup.csv' { Get-Wmi Win32_StartupCommand | Select-Object Name, Command, Location, User }
    Save-Text 'scheduled_tasks.csv' { schtasks.exe /query /fo csv /v }
    Save-Text 'environment.txt' { Get-ChildItem env: | Sort-Object Name | Format-Table -AutoSize -Wrap }
    Save-Text 'network.txt' {
        '==== ipconfig ===='; ipconfig.exe /all
        '==== netstat -ano ===='; netstat.exe -ano
    }
    Save-Text 'disk.txt' {
        '==== LogicalDisk ===='; Get-Wmi Win32_LogicalDisk | Format-Table DeviceID, FileSystem, @{n='SizeGB';e={[math]::Round($_.Size/1GB,1)}}, @{n='FreeGB';e={[math]::Round($_.FreeSpace/1GB,1)}} -AutoSize
        '==== DiskDrive ===='; Get-Wmi Win32_DiskDrive | Format-Table Model, InterfaceType, MediaType, @{n='SizeGB';e={[math]::Round($_.Size/1GB,1)}}, Status -AutoSize
        if (Get-Command Get-PhysicalDisk -ErrorAction SilentlyContinue) {
            '==== PhysicalDisk ===='; Get-PhysicalDisk | Format-Table FriendlyName, MediaType, BusType, HealthStatus, OperationalStatus -AutoSize
        }
    }
    Save-Text 'antivirus.txt' {
        '==== SecurityCenter2 ===='
        try { Get-WmiObject -Namespace root\SecurityCenter2 -Class AntiVirusProduct -ErrorAction Stop | Format-List displayName, productState, pathToSignedProductExe } catch { 'n/a (server OS)' }
        if (Get-Command Get-MpComputerStatus -ErrorAction SilentlyContinue) {
            '==== Defender status ===='; Get-MpComputerStatus | Format-List *
            '==== Defender exclusions ===='; Get-MpPreference | Format-List ExclusionPath, ExclusionProcess, ExclusionExtension, ScanAvgCPULoadFactor
        }
    }
    Save-Text 'wer_reports.txt' {
        foreach ($d in "$env:ProgramData\Microsoft\Windows\WER\ReportArchive", "$env:ProgramData\Microsoft\Windows\WER\ReportQueue") {
            "==== $d ===="
            Get-ChildItem $d -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 100 | Format-Table LastWriteTime, Name -AutoSize
        }
    }
}

function Collect-TargetInfo {
    param([int[]]$Pids)
    foreach ($procId in $Pids) {
        $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if (-not $p) { continue }
        $f = "target_{0}_{1}.txt" -f $p.ProcessName, $procId
        Save-Text $f {
            '==== process ===='; Get-Wmi Win32_Process -Filter "ProcessId=$procId" | Format-List *
            '==== file version ===='; try { $p.MainModule.FileVersionInfo | Format-List * } catch { "n/a: $($_.Exception.Message)" }
            '==== modules ===='
            try { $p.Modules | Sort-Object ModuleName | Format-Table ModuleName, FileVersion, FileName -AutoSize } catch { tasklist.exe /m /fi "PID eq $procId" }
        }
    }
}

function Collect-EventLogs {
    Write-Log "Exporting event logs (last $EventLogDays days)" STEP
    $wevt = Join-Path (Get-System32Path) 'wevtutil.exe'
    $ms = [int64]$EventLogDays * 86400000
    $query = "*[System[TimeCreated[timediff(@SystemTime) <= $ms]]]"
    $channels = @(
        'System', 'Application',
        'Microsoft-Windows-Diagnostics-Performance/Operational',
        'Microsoft-Windows-Resource-Exhaustion-Detector/Operational',
        'Microsoft-Windows-Kernel-Power/Thermal-Operational',
        'Microsoft-Windows-WMI-Activity/Operational',
        'Microsoft-Windows-Windows Defender/Operational'
    )
    $locale = (Get-Culture).Name
    foreach ($ch in $channels) {
        $file = Join-Path $script:EvtDir (($ch -replace '[\\/ ]', '_') + '.evtx')
        & $wevt epl $ch $file "/q:$query" 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $file)) {
            # Embed message text so the log renders on the analysis machine.
            & $wevt al $file "/l:$locale" 2>&1 | Out-Null
        }
    }
    # Readable digest of errors / warnings.
    try {
        $since = (Get-Date).AddDays(-$EventLogDays)
        Get-WinEvent -FilterHashtable @{ LogName = 'System', 'Application'; Level = 1, 2, 3; StartTime = $since } -MaxEvents 3000 -ErrorAction Stop |
            Select-Object TimeCreated, LogName, LevelDisplayName, ProviderName, Id, @{ n = 'Message'; e = { ("$($_.Message)" -replace '\s+', ' ') } } |
            Export-Csv -Path (Join-Path $script:EvtDir 'errors_warnings.csv') -NoTypeInformation -Encoding UTF8
    }
    catch { Write-Log "Event digest: $($_.Exception.Message)" WARN }
}

#endregion

#region ---------- tracing ----------

function Start-Trace {
    if ($NoTrace) { return $null }
    $wpr = Find-Tool -Names 'wpr.exe' -SystemToo
    if ($wpr) {
        # WPR refuses to start while another WPR session runs.
        & $wpr -cancel 2>&1 | Out-Null
        $available = @()
        foreach ($l in (& $wpr -profiles 2>&1)) { if ("$l" -match '^\s*([A-Za-z][\w\.]*)\b') { $available += $Matches[1] } }
        $wanted = switch ($Level) {
            'Quick' { @('CPU') }
            'Standard' { @('CPU', 'DiskIO', 'FileIO') }
            'Full' { @('GeneralProfile', 'CPU', 'DiskIO', 'FileIO', 'Registry', 'Network') }
        }
        # Drop profiles this wpr build does not know (only when the listing could be parsed).
        if ($available -contains 'CPU' -or $available -contains 'GeneralProfile') {
            $wanted = @($wanted | Where-Object { $available -contains $_ })
            if ($wanted.Count -eq 0) { $wanted = @('GeneralProfile') }
        }
        $wprArgs = @()
        foreach ($w in $wanted) { $wprArgs += '-start'; $wprArgs += $w }
        if (-not $Trigger) { $wprArgs += '-filemode' }   # memory (circular) mode in trigger mode
        Write-Log ("Starting WPR ({0}, {1})" -f ($wanted -join '+'), $(if ($Trigger) { 'circular memory' } else { 'file' })) STEP
        $code = Invoke-Native $wpr $wprArgs
        if ($code -eq 0) { return @{ Kind = 'wpr'; Exe = $wpr } }
        Write-Log "WPR failed to start (exit $code)" ERROR
    }
    $xperf = Find-Tool -Names 'xperf.exe' -SystemToo
    if ($xperf) {
        Write-Log 'Starting xperf kernel trace (fallback)' STEP
        & $xperf -stop 2>&1 | Out-Null
        $kfile = Join-Path $env:TEMP 'perfcollector_kernel.etl'
        $xargs = @('-on', 'PROC_THREAD+LOADER+PROFILE+CSWITCH+DISPATCHER+INTERRUPT+DPC+DISK_IO+HARD_FAULTS',
                   '-stackwalk', 'Profile+CSwitch+ReadyThread', '-BufferSize', '1024', '-MinBuffers', '256', '-MaxBuffers', '1024')
        if ($Trigger) { $xargs += '-FileMode'; $xargs += 'Circular'; $xargs += '-MaxFile'; $xargs += '1024' }
        $xargs += '-f'; $xargs += $kfile
        $code = Invoke-Native $xperf $xargs
        if ($code -eq 0) { return @{ Kind = 'xperf'; Exe = $xperf; Temp = $kfile } }
        Write-Log "xperf failed to start (exit $code)" ERROR
    }
    Write-Log 'No ETW tracer available (wpr.exe / xperf.exe). Continuing with counters only. Put WPT into tools\wpt to enable.' WARN
    $null
}

function Stop-Trace {
    param($Trace)
    if (-not $Trace) { return }
    Write-Log 'Stopping trace and merging (can take a few minutes, do not close the window)' STEP
    $etl = Join-Path $script:OutRoot ("trace_{0}.etl" -f $env:COMPUTERNAME)
    if ($Trace.Kind -eq 'wpr') {
        $code = Invoke-Native $Trace.Exe @('-stop', $etl, "PerfCollector $Level")
    }
    else {
        $code = Invoke-Native $Trace.Exe @('-d', $etl)
        Remove-Item $Trace.Temp -ErrorAction SilentlyContinue
    }
    if ($code -ne 0) { Write-Log "Trace stop returned $code" ERROR } else { Write-Log "Trace saved: $etl" }
}

# Counters are written in segments (counters_001.csv, ...) so that a long trigger-mode wait
# keeps only the most recent ones instead of growing without bound.
function Start-Counters {
    param([int]$Segment = 1)
    if ($Segment -eq 1) {
        Write-Log 'Starting performance counters (typeperf)' STEP
        $script:CounterMap = Get-PerfNameTranslator
    }
    $typeperf = Join-Path (Get-System32Path) 'typeperf.exe'
    $csv = Join-Path $script:OutRoot ('counters_{0:D3}.csv' -f $Segment)
    $script:CounterCsv = $csv
    $quoted = @($CounterList | ForEach-Object { '"' + (Convert-CounterPath $_ $script:CounterMap) + '"' })
    $argLine = ($quoted -join ' ') + " -si $SampleInterval -f CSV -o `"$csv`" -y"
    try {
        Start-Process -FilePath $typeperf -ArgumentList $argLine -NoNewWindow -PassThru `
            -RedirectStandardOutput (Join-Path $script:OutRoot ('typeperf_{0:D3}_stdout.txt' -f $Segment)) `
            -RedirectStandardError (Join-Path $script:OutRoot ('typeperf_{0:D3}_stderr.txt' -f $Segment))
    }
    catch { Write-Log "typeperf failed: $($_.Exception.Message)" ERROR; $null }
}

function Stop-Counters {
    param($Proc)
    if ($Proc -and -not $Proc.HasExited) {
        Start-Sleep -Seconds $SampleInterval
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    }
}

function Start-Dumps {
    param([int[]]$Pids)
    if (-not $Dump) { return @() }
    $pd = Find-Tool -Names 'procdump64.exe', 'procdump.exe'
    if (-not $pd) { Write-Log 'Dump requested but tools\procdump64.exe not found; skipping dumps (see tools\README.md)' WARN; return @() }
    if (-not $Pids -or $Pids.Count -eq 0) { Write-Log 'Dump requested but no target process; skipping' WARN; return @() }
    New-Item -ItemType Directory -Path $script:DumpDir -Force | Out-Null
    $procs = @()
    foreach ($procId in $Pids) {
        Write-Log "Taking 3 full dumps of PID $procId (10 s apart)" STEP
        $procs += Start-Process -FilePath $pd -ArgumentList "-accepteula -ma -n 3 -s 10 $procId `"$script:DumpDir`"" `
            -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $script:DumpDir "procdump_$procId.log")
    }
    $procs
}

#endregion

#region ---------- summary ----------

function Split-CsvLine {
    param([string]$Line)
    $t = $Line.Trim()
    if ($t.StartsWith('"')) { $t = $t.Substring(1) }
    if ($t.EndsWith('"')) { $t = $t.Substring(0, $t.Length - 1) }
    $t -split '","'
}

function Get-Stat {
    param([double[]]$Values)
    $v = @($Values | Where-Object { $null -ne $_ })
    if ($v.Count -eq 0) { return $null }
    $m = $v | Measure-Object -Average -Maximum -Minimum
    New-Object PSObject -Property @{ Avg = $m.Average; Max = $m.Maximum; Min = $m.Minimum; Last = $v[-1] }
}

# Parses typeperf CSV (last $MaxRows samples) into: column header -> double[]
function Read-CounterCsv {
    param([string]$Path, [int]$MaxRows = 100000)
    $enc = [System.Text.Encoding]::Default
    $lines = [System.IO.File]::ReadAllLines($Path, $enc)
    if ($lines.Count -lt 2) { return $null }
    $header = @(Split-CsvLine $lines[0])
    $first = [math]::Max(1, $lines.Count - $MaxRows)
    $cols = @{}
    for ($c = 1; $c -lt $header.Count; $c++) { $cols[$c] = New-Object System.Collections.Generic.List[double] }
    $inv = [Globalization.CultureInfo]::InvariantCulture
    for ($r = $first; $r -lt $lines.Count; $r++) {
        if (-not $lines[$r].Trim()) { continue }
        $f = @(Split-CsvLine $lines[$r])
        for ($c = 1; $c -lt [math]::Min($f.Count, $header.Count); $c++) {
            $d = 0.0
            if ([double]::TryParse($f[$c].Trim(), [Globalization.NumberStyles]::Float, $inv, [ref]$d)) { $cols[$c].Add($d) }
        }
    }
    $result = @{}
    for ($c = 1; $c -lt $header.Count; $c++) { $result[$header[$c]] = $cols[$c].ToArray() }
    $result
}

function Find-Columns {
    # Returns @{ instance = values } for an English counter path like '\Process(*)\% Processor Time'.
    param([hashtable]$Data, [string]$EnglishPath, [hashtable]$Map)
    $loc = Convert-CounterPath $EnglishPath $Map
    if ($loc -notmatch '^\\([^\\(]+)(\([^)]*\))?\\(.+)$') { return @{} }
    $obj = [regex]::Escape($Matches[1]); $ctr = [regex]::Escape($Matches[3])
    $re = "^\\\\[^\\]+\\$obj(?:\(([^)]*)\))?\\$ctr$"
    $out = @{}
    foreach ($k in $Data.Keys) {
        if ($k -match $re) {
            $inst = if ($Matches.Count -gt 1 -and $Matches[1]) { $Matches[1] } else { '' }
            $out[$inst] = $Data[$k]
        }
    }
    $out
}

function Write-Summary {
    param([int]$WindowSeconds, $ProcWindow, $ThreadRows, [int[]]$Targets)
    $sb = New-Object System.Text.StringBuilder
    function Add([string]$s = '') { [void]$sb.AppendLine($s) }
    $os = Get-Wmi Win32_OperatingSystem
    $cs = Get-Wmi Win32_ComputerSystem
    $cpu = @(Get-Wmi Win32_Processor)
    $plan = (powercfg.exe /getactivescheme) -join ' '
    $hints = New-Object System.Collections.Generic.List[string]

    Add "PerfCollector $ScriptVersion summary"
    Add ("Computer : {0}   Collected: {1}" -f $env:COMPUTERNAME, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'))
    if ($os) { Add ("OS       : {0} {1} build {2}  LastBoot {3}" -f $os.Caption, $os.OSArchitecture, $os.BuildNumber, $os.LastBootUpTime) }
    if ($cpu) { Add ("CPU      : {0}  x{1} sockets, {2} cores, {3} logical" -f $cpu[0].Name.Trim(), $cpu.Count, ($cpu | Measure-Object NumberOfCores -Sum).Sum, $script:Logical) }
    if ($cs) { Add ("Memory   : {0:N1} GB" -f ($cs.TotalPhysicalMemory / 1GB)) }
    Add "PowerPlan: $plan"
    Add ("Mode     : {0}, Level {1}, window {2}s, targets: {3}" -f $(if ($Trigger) { 'Trigger' } else { 'Fixed' }), $Level, $WindowSeconds, $(if ($Targets) { $Targets -join ',' } else { '(none)' }))
    if ($script:TriggerReason) { Add "Trigger  : $script:TriggerReason" }
    Add ''
    if ($plan -notmatch '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' -and $plan -notmatch 'e9a42b02-d5df-448d-aa00-03f14749eb61') {
        $hints.Add('Power plan is not High/Ultimate performance: CPU may run at reduced frequency (check "% Processor Performance").')
    }

    $data = $null
    if ($script:CounterCsv -and (Test-Path $script:CounterCsv)) {
        try { $data = Read-CounterCsv -Path $script:CounterCsv -MaxRows ([math]::Ceiling($WindowSeconds / $SampleInterval) + 2) }
        catch { Add "Counter CSV parse failed: $($_.Exception.Message)" }
    }
    if ($data) {
        $m = $script:CounterMap
        Add '==== System CPU (counters, capture window) ===='
        $tot = Find-Columns $data '\Processor Information(*)\% Processor Time' $m
        $priv = Find-Columns $data '\Processor Information(*)\% Privileged Time' $m
        $intr = Find-Columns $data '\Processor Information(*)\% Interrupt Time' $m
        $dpc = Find-Columns $data '\Processor Information(*)\% DPC Time' $m
        $perf = Find-Columns $data '\Processor Information(*)\% Processor Performance' $m
        $freq = Find-Columns $data '\Processor Information(*)\Processor Frequency' $m
        $s = if ($tot.ContainsKey('_Total')) { Get-Stat $tot['_Total'] }
        if ($s) {
            Add ("Total CPU        avg {0,6:N1}%  max {1,6:N1}%" -f $s.Avg, $s.Max)
            if ($s.Avg -ge 85) { $hints.Add('Machine CPU is saturated (avg >= 85%).') }
        }
        foreach ($pair in @(@('Privileged', $priv), @('Interrupt', $intr), @('DPC', $dpc))) {
            if ($pair[1].ContainsKey('_Total')) {
                $st = Get-Stat $pair[1]['_Total']
                if ($st) { Add ("{0,-16} avg {1,6:N1}%  max {2,6:N1}%" -f $pair[0], $st.Avg, $st.Max) }
            }
        }
        if ($intr.ContainsKey('_Total') -and $dpc.ContainsKey('_Total')) {
            $a = (Get-Stat $intr['_Total']).Avg + (Get-Stat $dpc['_Total']).Avg
            if ($a -ge 5) { $hints.Add(("Interrupt+DPC averages {0:N1}%: likely a driver issue. In WPA open Computation > DPC/ISR." -f $a)) }
        }
        if ($priv.ContainsKey('_Total') -and $tot.ContainsKey('_Total')) {
            $pa = (Get-Stat $priv['_Total']).Avg; $ta = (Get-Stat $tot['_Total']).Avg
            if ($ta -gt 20 -and $pa / $ta -gt 0.5) { $hints.Add('More than half of CPU time is kernel (privileged): look at drivers, file system filters (AV), syscalls, paging.') }
        }
        if ($perf.ContainsKey('_Total')) {
            $st = Get-Stat $perf['_Total']
            Add ("Perf vs nominal  avg {0,6:N1}%  min {1,6:N1}%" -f $st.Avg, $st.Min)
            if ($st.Avg -lt 80) { $hints.Add(("Processor Performance avg {0:N0}% of nominal: throttling / power saving (thermal, power plan, BIOS)." -f $st.Avg)) }
        }
        if ($freq.ContainsKey('_Total')) { Add ("Frequency MHz    avg {0,6:N0}" -f (Get-Stat $freq['_Total']).Avg) }
        $cores = @()
        foreach ($k in $tot.Keys) { if ($k -notmatch '_Total') { $st = Get-Stat $tot[$k]; if ($st) { $cores += New-Object PSObject -Property @{ Core = $k; Avg = $st.Avg; Max = $st.Max } } } }
        if ($cores.Count -gt 0) {
            $hot = $cores | Sort-Object Avg -Descending | Select-Object -First 4
            Add ('Hottest cores    ' + (($hot | ForEach-Object { '{0}={1:N0}%' -f $_.Core, $_.Avg }) -join '  '))
            if ($hot[0].Avg -ge 90 -and $s -and $s.Avg -lt 60) { $hints.Add("Core $($hot[0].Core) is pinned (~100%) while total is lower: single-threaded hot loop, affinity, or interrupt steering.") }
        }
        foreach ($c in @(@('\System\Processor Queue Length', 'Run queue length'), @('\System\Context Switches/sec', 'Context sw/sec'), @('\System\System Calls/sec', 'Syscalls/sec'))) {
            $col = Find-Columns $data $c[0] $m
            if ($col.ContainsKey('')) {
                $st = Get-Stat $col['']
                Add ("{0,-16} avg {1,10:N0}  max {2,10:N0}" -f $c[1], $st.Avg, $st.Max)
                if ($c[1] -eq 'Run queue length' -and $st.Avg -gt 2 * $script:Logical) { $hints.Add('Processor queue length > 2x logical CPUs: threads are waiting for CPU (Ready time in WPA).') }
            }
        }
        Add ''
        Add '==== Memory / Disk ===='
        $avail = Find-Columns $data '\Memory\Available MBytes' $m
        if ($avail.ContainsKey('')) {
            $st = Get-Stat $avail['']
            Add ("Available MB     min {0,10:N0}  avg {1,10:N0}" -f $st.Min, $st.Avg)
            if ($cs -and $st.Min -lt ($cs.TotalPhysicalMemory / 1MB) * 0.1) { $hints.Add('Available memory dropped below 10%: paging can burn CPU (check Hard Faults in WPA).') }
        }
        $commit = Find-Columns $data '\Memory\% Committed Bytes In Use' $m
        if ($commit.ContainsKey('')) { Add ("Commit in use    max {0,10:N1}%" -f (Get-Stat $commit['']).Max) }
        $pages = Find-Columns $data '\Memory\Pages/sec' $m
        if ($pages.ContainsKey('')) { Add ("Pages/sec        avg {0,10:N0}  max {1,10:N0}" -f (Get-Stat $pages['']).Avg, (Get-Stat $pages['']).Max) }
        $disk = Find-Columns $data '\PhysicalDisk(*)\% Disk Time' $m
        foreach ($k in ($disk.Keys | Sort-Object)) { $st = Get-Stat $disk[$k]; if ($st) { Add ("Disk {0,-11} busy avg {1,6:N0}%  max {2,6:N0}%" -f $k, $st.Avg, $st.Max) } }
        Add ''

        $pcpu = Find-Columns $data '\Process(*)\% Processor Time' $m
        $ppriv = Find-Columns $data '\Process(*)\% Privileged Time' $m
        $ppid = Find-Columns $data '\Process(*)\ID Process' $m
        $pthr = Find-Columns $data '\Process(*)\Thread Count' $m
        $phnd = Find-Columns $data '\Process(*)\Handle Count' $m
        $pprv = Find-Columns $data '\Process(*)\Private Bytes' $m
        $rows = @()
        foreach ($k in $pcpu.Keys) {
            if ($k -eq '_Total' -or $k -eq 'Idle') { continue }
            $st = Get-Stat $pcpu[$k]
            if (-not $st) { continue }
            $pv = if ($ppriv.ContainsKey($k)) { (Get-Stat $ppriv[$k]).Avg } else { 0 }
            $rows += New-Object PSObject -Property @{
                Instance = $k
                Pid = $(if ($ppid.ContainsKey($k)) { [int](Get-Stat $ppid[$k]).Last } else { 0 })
                AvgPct = $st.Avg / $script:Logical; MaxPct = $st.Max / $script:Logical
                KernelShare = $(if ($st.Avg -gt 0) { 100 * $pv / $st.Avg } else { 0 })
                Threads = $(if ($pthr.ContainsKey($k)) { (Get-Stat $pthr[$k]).Last } else { 0 })
                Handles = $(if ($phnd.ContainsKey($k)) { (Get-Stat $phnd[$k]).Last } else { 0 })
                HandleGrowth = $(if ($phnd.ContainsKey($k)) { $h = $phnd[$k]; if ($h.Count -gt 1) { $h[-1] - $h[0] } else { 0 } } else { 0 })
                PrivateMB = $(if ($pprv.ContainsKey($k)) { (Get-Stat $pprv[$k]).Last / 1MB } else { 0 })
                PrivateGrowthMB = $(if ($pprv.ContainsKey($k)) { $v = $pprv[$k]; if ($v.Count -gt 1) { ($v[-1] - $v[0]) / 1MB } else { 0 } } else { 0 })
            }
        }
        Add ("==== Top processes by CPU (% of whole machine, {0} logical CPUs) ====" -f $script:Logical)
        Add ("{0,-28} {1,7} {2,7} {3,7} {4,8} {5,8} {6,9} {7,10} {8,10}" -f 'Process', 'PID', 'Avg%', 'Max%', 'Kernel%', 'Threads', 'Handles', 'PrivMB', 'PrivDelta')
        $oneCore = 100.0 / $script:Logical
        foreach ($r in ($rows | Sort-Object AvgPct -Descending | Select-Object -First 20)) {
            Add ("{0,-28} {1,7} {2,7:N1} {3,7:N1} {4,8:N0} {5,8:N0} {6,9:N0} {7,10:N0} {8,10:N0}" -f $r.Instance, $r.Pid, $r.AvgPct, $r.MaxPct, $r.KernelShare, $r.Threads, $r.Handles, $r.PrivateMB, $r.PrivateGrowthMB)
            if ($r.AvgPct -ge 5 -and $script:Logical -gt 1 -and [math]::Abs($r.AvgPct - $oneCore) / $oneCore -lt 0.12) {
                $hints.Add(("{0} (PID {1}) uses ~exactly one core ({2:N1}%): typical of a single thread spinning / busy loop." -f $r.Instance, $r.Pid, $r.AvgPct))
            }
            if ($r.AvgPct -ge 5 -and $r.KernelShare -ge 60) { $hints.Add(("{0} (PID {1}) spends {2:N0}% of its CPU in kernel mode: syscalls, I/O, AV filter or driver." -f $r.Instance, $r.Pid, $r.KernelShare)) }
            foreach ($wk in 'MsMpEng', 'WmiPrvSE', 'TiWorker', 'SearchIndexer', 'svchost', 'System', 'csrss', 'dwm', 'Interrupts') {
                if ($r.Instance -match "^$wk(#\d+)?$" -and $r.AvgPct -ge 10) { $hints.Add(("{0} is high ({1:N0}%): see notes for this process in README (AV scan / WMI client / Windows Update / services)." -f $r.Instance, $r.AvgPct)) }
            }
        }
        foreach ($r in ($rows | Where-Object { $_.HandleGrowth -gt 2000 -or $_.PrivateGrowthMB -gt 200 })) {
            $hints.Add(("{0} (PID {1}) grew +{2:N0} handles / +{3:N0} MB private during the window: possible leak." -f $r.Instance, $r.Pid, $r.HandleGrowth, $r.PrivateGrowthMB))
        }
        Add ''
    }
    else { Add 'Counter data not available (see typeperf_stderr.txt).'; Add '' }

    if ($ProcWindow) {
        Add '==== Top processes by CPU time over the window (Get-Process delta) ===='
        foreach ($r in ($ProcWindow | Select-Object -First 10)) { Add ("{0,-28} PID {1,7}  {2,6:N1}%  ({3:N0} ms CPU)" -f $r.Name, $r.Id, $r.Pct, $r.CpuMs) }
        Add ''
    }
    if ($ThreadRows) {
        Add '==== Busiest threads (TID matches WPA "Thread ID"; % of one core) ===='
        foreach ($t in ($ThreadRows | Select-Object -First 15)) {
            Add ("{0,-24} PID {1,7} TID {2,7}  {3,6:N1}%  user {4,5:N0}%  pri {5,2} {6} {7} start {8}" -f $t.Process, $t.Pid, $t.Tid, $t.CorePct, $t.UserShare, $t.Priority, $t.State, $t.WaitReason, $t.StartAddress)
        }
        Add ''
    }
    Add '==== Hints (automatic, verify in WPA) ===='
    if ($hints.Count -eq 0) { Add '- Nothing obvious from counters. Open the .etl in WPA: CPU Usage (Sampled) by Process > Thread > Stack.' }
    foreach ($h in ($hints | Select-Object -Unique)) { Add "- $h" }
    Add ''
    Add 'Next step: open trace_*.etl in WPA, load symbols, Computation > CPU Usage (Sampled), group Process > Thread ID > Stack.'
    $sb.ToString() | Out-File -FilePath (Join-Path $script:OutRoot 'summary.txt') -Encoding UTF8
    Write-Host ''
    Write-Host $sb.ToString()
}

#endregion

#region ---------- main ----------

function Show-Menu {
    Write-Host ''
    Write-Host "  PerfCollector $ScriptVersion - CPU / performance log collector" -ForegroundColor Cyan
    Write-Host '  ---------------------------------------------------------'
    Write-Host '   1) Capture now: 60 s, Standard (recommended)'
    Write-Host '   2) Capture now for a specific process (+ thread detail, optional dumps)'
    Write-Host '   3) Wait for a CPU spike, then capture automatically (trigger mode)'
    Write-Host '   4) Capture now: 60 s, Full (more providers, bigger file)'
    Write-Host '   5) System info + counters only (no ETW trace)'
    Write-Host '   Q) Quit'
    $c = Read-Host '  Select'
    $r = @{}
    switch ($c) {
        '1' { }
        '2' {
            $r.ProcessName = @((Read-Host '  Process name (e.g. myapp, comma separated)') -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            $d = Read-Host '  Duration seconds [60]'; if ($d) { $r.Duration = [int]$d }
            if ((Read-Host '  Take memory dumps? (y/N)') -match '^[yY]') { $r.Dump = $true }
        }
        '3' {
            $n = Read-Host '  Process name to watch (empty = whole machine)'
            if ($n) { $r.ProcessName = @($n -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
            $t = Read-Host '  CPU threshold % of whole machine [80]'; if ($t) { $r.CpuThreshold = [int]$t }
            $s = Read-Host '  Must stay above threshold for N seconds [10]'; if ($s) { $r.SustainSeconds = [int]$s }
            $r.Trigger = $true
        }
        '4' { $r.Level = 'Full' }
        '5' { $r.NoTrace = $true }
        default { exit 0 }
    }
    $r
}

# Relaunch elevated: kernel tracing, other users' processes and some logs need admin.
if (-not (Test-IsAdmin)) {
    Write-Host 'Administrator rights required, relaunching elevated...' -ForegroundColor Yellow
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
    foreach ($kv in $PSBoundParameters.GetEnumerator()) {
        if ($kv.Value -is [switch]) { if ($kv.Value) { $argList += "-$($kv.Key)" } }
        elseif ($kv.Value -is [array]) { $argList += "-$($kv.Key)"; $argList += (($kv.Value | ForEach-Object { "`"$_`"" }) -join ',') }
        else { $argList += "-$($kv.Key)"; $argList += "`"$($kv.Value)`"" }
    }
    $argList += '-PauseAtEnd'
    try { Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $argList -Verb RunAs }
    catch { Write-Host "Elevation cancelled: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
    exit 0
}

if ($Interactive) {
    $sel = Show-Menu
    foreach ($k in $sel.Keys) { Set-Variable -Name $k -Value $sel[$k] }
    $PauseAtEnd = $true
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
if (-not $OutputDir) { $OutputDir = Join-Path $ToolRoot 'output' }
$script:OutRoot = Join-Path $OutputDir ("PerfLogs_{0}_{1}" -f $env:COMPUTERNAME, $stamp)
$script:InfoDir = Join-Path $script:OutRoot 'sysinfo'
$script:EvtDir = Join-Path $script:OutRoot 'eventlogs'
$script:DumpDir = Join-Path $script:OutRoot 'dumps'
foreach ($d in $script:OutRoot, $script:InfoDir, $script:EvtDir) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
$script:LogFile = Join-Path $script:OutRoot 'collect.log'
$script:TriggerReason = $null
$script:CounterCsv = $null
$script:CounterMap = @{}
$script:Logical = 0
$cs0 = Get-Wmi Win32_ComputerSystem
if ($cs0) { $script:Logical = [int]$cs0.NumberOfLogicalProcessors }
if ($script:Logical -le 0) { $script:Logical = [Environment]::ProcessorCount }

Write-Log "PerfCollector $ScriptVersion on $env:COMPUTERNAME, PowerShell $($PSVersionTable.PSVersion), output: $script:OutRoot" STEP
Write-Log ("Args: " + (($PSBoundParameters.GetEnumerator() | ForEach-Object { "-$($_.Key) $($_.Value)" }) -join ' '))

# Free space check: an ETL can reach several GB at Full level.
try {
    $drive = (Get-Item $OutputDir).PSDrive.Name
    $free = (Get-PSDrive $drive).Free
    if ($free -lt 2GB) { Write-Log ("Only {0:N1} GB free on {1}: - consider -OutputDir on another drive" -f ($free / 1GB), $drive) WARN }
} catch { }

$targets = @(Resolve-Targets)
if (($ProcessName -or $ProcessId) -and $targets.Count -eq 0) { Write-Log 'Target process not running yet (will be resolved again at trigger/end).' WARN }
elseif ($targets.Count -gt 0) { Write-Log ("Target PIDs: " + ($targets -join ', ')) }

Save-ProcessSnapshot 'processes_before.csv'
$counterProc = Start-Counters
$trace = Start-Trace
$dumpProcs = @()

$abort = $false
$windowStart = Get-Date
$procT0 = $null

if ($Trigger) {
    $what = if ($ProcessName -or $ProcessId) { 'target process(es)' } else { 'total CPU' }
    Write-Log "Trigger mode: waiting for $what >= $CpuThreshold% for $SustainSeconds s (max $MaxWaitMinutes min). Press S to capture now, Q to abort." STEP
    $deadline = (Get-Date).AddMinutes($MaxWaitMinutes)
    $above = 0.0
    $prev = Get-ProcessTimes; $prevTime = Get-Date
    $segment = 1; $segStart = Get-Date
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (((Get-Date) - $segStart).TotalMinutes -ge 10) {
            # Rotate: keep the current and previous segment only.
            $segment++; $segStart = Get-Date
            $old = $counterProc
            $counterProc = Start-Counters -Segment $segment
            Stop-Counters $old
            Get-ChildItem $script:OutRoot -Filter ('*_{0:D3}*' -f ($segment - 2)) -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
            Get-ChildItem $script:OutRoot -Filter ('counters_{0:D3}.csv' -f ($segment - 2)) -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        }
        $now = Get-ProcessTimes; $nowTime = Get-Date
        $elapsedMs = ($nowTime - $prevTime).TotalMilliseconds
        if ($ProcessName -or $ProcessId) {
            $tg = @(Resolve-Targets)
            $used = 0.0
            foreach ($procId in $tg) { if ($now.ContainsKey($procId) -and $prev.ContainsKey($procId)) { $used += $now[$procId].CpuMs - $prev[$procId].CpuMs } }
            $pct = 100.0 * $used / ($elapsedMs * $script:Logical)
        }
        else {
            $pt = Get-Wmi Win32_PerfFormattedData_PerfOS_Processor "Name='_Total'"
            $pct = if ($pt) { [double]$pt.PercentProcessorTime } else { 0 }
        }
        $prev = $now; $prevTime = $nowTime
        if ($pct -ge $CpuThreshold) { $above += $elapsedMs / 1000 } else { $above = 0 }
        Write-Host ("`r  {0}  CPU {1,5:N1}%  above threshold {2,4:N0}s / {3}s   " -f (Get-Date -Format 'HH:mm:ss'), $pct, $above, $SustainSeconds) -NoNewline
        $key = $null
        try { if ([Console]::KeyAvailable) { $key = [Console]::ReadKey($true).KeyChar } } catch { }
        if ($key -eq 's' -or $key -eq 'S') { $script:TriggerReason = 'manual (S key)'; break }
        if ($key -eq 'q' -or $key -eq 'Q') { $abort = $true; break }
        if ($above -ge $SustainSeconds) { $script:TriggerReason = ("{0} at {1:N1}% for {2:N0}s at {3}" -f $what, $pct, $above, (Get-Date -Format 'HH:mm:ss')); break }
    }
    Write-Host ''
    if (-not $script:TriggerReason -and -not $abort) { $script:TriggerReason = "timeout after $MaxWaitMinutes min (no spike)" }
    if ($abort) { Write-Log 'Aborted by user; saving what was recorded' WARN } else { Write-Log "TRIGGERED: $script:TriggerReason" STEP }
    # The spike already happened inside the circular buffer; record a bit more after it.
    $targets = @(Resolve-Targets)
    $windowStart = Get-Date
    $procT0 = Get-ProcessTimes
    $thrPids = if ($targets.Count -gt 0) { $targets } else { @($procT0.Values | ForEach-Object { $_.Id }) }
    $thrT0 = Get-ThreadTimes $thrPids
    $dumpProcs = Start-Dumps $targets
    $wait = if ($abort) { 0 } else { $PostTriggerSeconds }
    Start-Sleep -Seconds $wait
}
else {
    $procT0 = Get-ProcessTimes
    $thrPids = if ($targets.Count -gt 0) { $targets } else { @($procT0.Values | ForEach-Object { $_.Id }) }
    $thrT0 = Get-ThreadTimes $thrPids
    $dumpProcs = Start-Dumps $targets
    Write-Log "Recording for $Duration s - reproduce the problem now. (Ctrl+C is safe only before this point)" STEP
    for ($i = 0; $i -lt $Duration; $i++) {
        Write-Progress -Activity 'Recording' -Status ("{0}/{1} s" -f $i, $Duration) -PercentComplete (100 * $i / $Duration)
        Start-Sleep -Seconds 1
    }
    Write-Progress -Activity 'Recording' -Completed
}

# ---- end of capture window ----
$windowSec = [math]::Max(1, ((Get-Date) - $windowStart).TotalSeconds)
$procT1 = Get-ProcessTimes
$thrT1 = Get-ThreadTimes $thrPids
$windowMs = $windowSec * 1000

$procWindow = @()
foreach ($k in $procT1.Keys) {
    if ($procT0.ContainsKey($k)) {
        $d = $procT1[$k].CpuMs - $procT0[$k].CpuMs
        $procWindow += New-Object PSObject -Property @{ Name = $procT1[$k].Name; Id = $k; CpuMs = $d; Pct = 100.0 * $d / ($windowMs * $script:Logical) }
    }
}
$procWindow = @($procWindow | Sort-Object CpuMs -Descending)
$procWindow | Select-Object Name, Id, @{ n = 'CpuPctOfMachine'; e = { [math]::Round($_.Pct, 2) } }, @{ n = 'CpuMs'; e = { [math]::Round($_.CpuMs) } } |
    Export-Csv (Join-Path $script:OutRoot 'process_cpu_window.csv') -NoTypeInformation -Encoding UTF8

# Thread detail only for targets, or the top 5 processes when no target was given.
$focus = if ($targets.Count -gt 0) { $targets } else { @($procWindow | Select-Object -First 5 | ForEach-Object { $_.Id }) }
$t0 = @{}; foreach ($t in $thrT0) { $t0[$t.Key] = $t }
$threadRows = @()
foreach ($t in $thrT1) {
    if ($focus -notcontains $t.Pid -or -not $t0.ContainsKey($t.Key)) { continue }
    $d = $t.CpuMs - $t0[$t.Key].CpuMs
    $du = $t.UserMs - $t0[$t.Key].UserMs
    $threadRows += New-Object PSObject -Property @{
        Process = $t.Process; Pid = $t.Pid; Tid = $t.Tid; CpuMs = [math]::Round($d)
        CorePct = 100.0 * $d / $windowMs; UserShare = $(if ($d -gt 0) { 100.0 * $du / $d } else { 0 })
        Priority = $t.Priority; State = $t.State; WaitReason = $t.WaitReason; StartAddress = $t.StartAddress
    }
}
$threadRows = @($threadRows | Sort-Object CpuMs -Descending)
$threadRows | Select-Object Process, Pid, Tid, CpuMs, @{ n = 'PctOfOneCore'; e = { [math]::Round($_.CorePct, 1) } },
    @{ n = 'UserPct'; e = { [math]::Round($_.UserShare) } }, Priority, State, WaitReason, StartAddress |
    Export-Csv (Join-Path $script:OutRoot 'thread_cpu_window.csv') -NoTypeInformation -Encoding UTF8

Stop-Trace $trace
Stop-Counters $counterProc
Save-ProcessSnapshot 'processes_after.csv'

if ($dumpProcs.Count -gt 0) {
    Write-Log 'Waiting for procdump to finish' STEP
    foreach ($p in $dumpProcs) { try { $p.WaitForExit(600000) | Out-Null } catch { } }
}

Collect-TargetInfo $(if ($targets.Count -gt 0) { $targets } else { $focus | Select-Object -First 3 })
Collect-SystemInfo
if ($EventLogDays -gt 0) { Collect-EventLogs }

Write-Log 'Building summary' STEP
try { Write-Summary -WindowSeconds $windowSec -ProcWindow $procWindow -ThreadRows $threadRows -Targets $targets }
catch { Write-Log "Summary failed: $($_.Exception.Message)" ERROR }

$final = $script:OutRoot
if (-not $NoZip) {
    Write-Log 'Compressing' STEP
    $zip = $script:OutRoot + '.zip'
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
        $script:LogFile = $null   # stop writing into the folder being zipped
        [System.IO.Compression.ZipFile]::CreateFromDirectory($script:OutRoot, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
        Remove-Item -Recurse -Force $script:OutRoot -ErrorAction SilentlyContinue
        $final = $zip
    }
    catch { Write-Log "Zip failed ($($_.Exception.Message)); please zip the folder manually" WARN }
}

Write-Host ''
Write-Host "DONE. Send this file for analysis:" -ForegroundColor Green
Write-Host "  $final" -ForegroundColor Green
if ($PauseAtEnd) { Read-Host 'Press Enter to close' | Out-Null }

#endregion
