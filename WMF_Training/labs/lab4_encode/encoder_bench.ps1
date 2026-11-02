<#
.SYNOPSIS
    Lab 4 encoder 量測腳本：用多種組態重複執行 dda_encode.exe，彙整成一份可以貼回分析的報告。

.DESCRIPTION
    量測的組態：
      A. 720p，low latency 開 / 關（B-frame 0）/ 關（B-frame 預設）× 15、30、60、120 fps
      B. 640x360 與 1920x1080，low latency 開 / 關（B-frame 0）× 30、60 fps
      C. 720p30 的診斷選項：--sync-gpu、--timer-1ms
      D. 軟體 encoder 對照組：720p 30、60 fps
      E. 和 60 Hz 不成倍數的幀率（23、37 fps），用來檢查 ProcessInput 是否對齊螢幕更新

    每個組態重複 -Repeat 次（重複次數放在最外層，讓同一組態的幾次執行分散在不同時間）。
    第一次執行的輸出檔會用 ffprobe / ffmpeg 分析（B-frame、VUI 色彩資訊、NAL 類型、封包大小）。

    執行期間請不要操作電腦（桌面要保持靜止），並接上電源、不要讓螢幕鎖定。

.EXAMPLE
    cd D:\...\WMF_Training\labs\build\Release
    powershell -ExecutionPolicy Bypass -File ..\..\lab4_encode\encoder_bench.ps1

.EXAMPLE
    # 先快速確認腳本能跑（4 個組態、各 1 次、每次 3 秒）
    powershell -ExecutionPolicy Bypass -File ..\..\lab4_encode\encoder_bench.ps1 -Quick
#>
[CmdletBinding()]
param(
    [string]$ExeDir = '',      # dda_encode.exe 所在資料夾；預設依序找目前資料夾、..\build\Release
    [int]$Repeat = 3,          # 每個組態重複幾次
    [int]$Seconds = 5,         # 每次錄幾秒
    [switch]$Quick,            # 快速模式：少量組態、各 1 次、每次 3 秒
    [string]$OutDir = '',      # 結果資料夾；預設在目前資料夾建立 bench_<日期時間>
    [int]$CooldownSec = 2,     # 每次執行之間休息幾秒
    [switch]$NoFfprobe,        # 不做 ffprobe / ffmpeg 分析
    [string]$FfmpegDir = ''    # ffprobe.exe / ffmpeg.exe 所在資料夾；預設找 PATH 和常見的安裝位置
)

$ErrorActionPreference = 'Continue'   # 'Stop' 會讓原生程式的 stderr 變成例外（PowerShell 5.1）
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$Inv = [System.Globalization.CultureInfo]::InvariantCulture

# ======================================================================
# 小工具
# ======================================================================
function ToD($v) {
    if ($null -eq $v) { return $null }
    if ($v -is [double] -or $v -is [int] -or $v -is [long]) { return [double]$v }
    $s = "$v"
    if ($s -eq '') { return $null }
    $d = 0.0
    if ([double]::TryParse($s, [System.Globalization.NumberStyles]::Float, $Inv, [ref]$d)) { return $d }
    return $null
}

function Fmt($v, [string]$f = '0.0') {
    $d = ToD $v
    if ($null -eq $d) { return '-' }
    return $d.ToString($f, $Inv)
}

function Get-PhaseR([double[]]$t, [double]$P) {
    # Rayleigh 向量長度：時刻對週期 P 取相位後的集中程度。1 = 全部落在同一相位，0 = 平均分散
    if ($t.Count -lt 2 -or $P -le 0) { return $null }
    $c = 0.0; $s = 0.0
    foreach ($x in $t) {
        $ph = 2.0 * [Math]::PI * (($x % $P) / $P)
        $c += [Math]::Cos($ph); $s += [Math]::Sin($ph)
    }
    return [Math]::Sqrt($c * $c + $s * $s) / $t.Count
}

function Get-BestPeriod([double[]]$t, [double]$from, [double]$to, [double]$step) {
    # 在 [from, to] 之間找讓相位最集中的週期，回傳 @(R, 週期)
    $best = -1.0; $bestP = $from
    for ($P = $from; $P -le $to + 1e-9; $P += $step) {
        $Rv = Get-PhaseR $t $P   # 注意：PowerShell 變數不分大小寫，不能叫 $R（會蓋掉別的 $r）
        if ($Rv -gt $best) { $best = $Rv; $bestP = $P }
    }
    return @($best, $bestP)
}

# 直方圖：每 2 ms 一格，0..40 ms 共 20 格，最後一格是 >= 40 ms
function Get-Hist([double[]]$vals) {
    $h = New-Object int[] 21
    foreach ($v in $vals) {
        if ($v -lt 0) { continue }
        $i = [int][Math]::Floor($v / 2.0)
        if ($i -gt 20) { $i = 20 }
        $h[$i]++
    }
    return ($h -join ' ')
}

function Run-Native([string]$exe, [string[]]$argList) {
    $out = & $exe @argList 2>&1 | ForEach-Object { "$_" }
    return [pscustomobject]@{ Lines = @($out); Code = $LASTEXITCODE }
}

# ======================================================================
# 找執行檔與工具
# ======================================================================
if (-not $ExeDir) {
    $cands = @((Get-Location).Path, (Join-Path $PSScriptRoot '..\build\Release'), (Join-Path $PSScriptRoot '..\build'))
    foreach ($c in $cands) { if (Test-Path (Join-Path $c 'dda_encode.exe')) { $ExeDir = $c; break } }
}
if (-not $ExeDir -or -not (Test-Path (Join-Path $ExeDir 'dda_encode.exe'))) {
    Write-Host '找不到 dda_encode.exe。請在 build\Release 資料夾執行，或用 -ExeDir 指定。' -ForegroundColor Red
    exit 1
}
$Exe = (Resolve-Path (Join-Path $ExeDir 'dda_encode.exe')).Path
$CaptureExe = Join-Path (Split-Path $Exe) 'dda_capture.exe'

$Ffprobe = $null; $Ffmpeg = $null
if (-not $NoFfprobe) {
    # 順序：-FfmpegDir > PATH > 常見的解壓縮 / 安裝位置
    $ffDirs = @()
    if ($FfmpegDir) { $ffDirs += $FfmpegDir }
    $p = Get-Command ffprobe -ErrorAction SilentlyContinue
    if ($p) { $ffDirs += (Split-Path $p.Path) }
    $patterns = @('C:\ffmpeg\bin', 'C:\ffmpeg*\bin', 'D:\ffmpeg*\bin', "$env:ProgramFiles\ffmpeg*\bin",
        "$env:USERPROFILE\ffmpeg*\bin", "$env:USERPROFILE\Downloads\ffmpeg*\bin", "$env:USERPROFILE\Downloads\ffmpeg*\ffmpeg*\bin",
        "$env:USERPROFILE\Desktop\ffmpeg*\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*FFmpeg*\ffmpeg*\bin")
    foreach ($pat in $patterns) {
        foreach ($d in @(Resolve-Path $pat -ErrorAction SilentlyContinue)) { $ffDirs += $d.Path }
    }
    foreach ($d in $ffDirs) {
        if (-not $Ffprobe -and (Test-Path (Join-Path $d 'ffprobe.exe'))) { $Ffprobe = (Join-Path $d 'ffprobe.exe') }
        if (-not $Ffmpeg -and (Test-Path (Join-Path $d 'ffmpeg.exe'))) { $Ffmpeg = (Join-Path $d 'ffmpeg.exe') }
    }
}

if (-not $OutDir) { $OutDir = Join-Path (Get-Location).Path ('bench_' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# ======================================================================
# 組態
# ======================================================================
$LL = @()
$NLLB0 = @('--no-low-latency', '--bframes', '0')
$NLL = @('--no-low-latency')

$Configs = New-Object System.Collections.Generic.List[object]
function Add-Cfg([string]$group, [string]$name, [string[]]$extra) {
    $Configs.Add([pscustomobject]@{ Group = $group; Name = $name; Extra = $extra })
}

if ($Quick) {
    $Repeat = 1
    $Seconds = 3
    Add-Cfg 'A' 'LL_720p_30' ($LL + @('--fps', '30'))
    Add-Cfg 'A' 'NLL_B0_720p_30' ($NLLB0 + @('--fps', '30'))
    Add-Cfg 'E' 'LL_720p_37' ($LL + @('--fps', '37'))
    Add-Cfg 'D' 'SW_720p_30' (@('--software', '--fps', '30'))
} else {
    $modes = @(
        [pscustomobject]@{ N = 'LL'; A = $LL },
        [pscustomobject]@{ N = 'NLL_B0'; A = $NLLB0 },
        [pscustomobject]@{ N = 'NLL'; A = $NLL }
    )
    foreach ($fps in @(15, 30, 60, 120)) {
        foreach ($m in $modes) { Add-Cfg 'A' ('{0}_720p_{1}' -f $m.N, $fps) ($m.A + @('--fps', "$fps")) }
    }
    foreach ($res in @(@('640', '360', '360p'), @('1920', '1080', '1080p'))) {
        foreach ($fps in @(30, 60)) {
            foreach ($m in $modes[0..1]) {
                Add-Cfg 'B' ('{0}_{1}_{2}' -f $m.N, $res[2], $fps) ($m.A + @('--fps', "$fps", '--width', $res[0], '--height', $res[1]))
            }
        }
    }
    Add-Cfg 'C' 'LL_720p_30_syncgpu' ($LL + @('--fps', '30', '--sync-gpu'))
    Add-Cfg 'C' 'LL_720p_30_timer1ms' ($LL + @('--fps', '30', '--timer-1ms'))
    Add-Cfg 'C' 'NLL_B0_720p_30_syncgpu' ($NLLB0 + @('--fps', '30', '--sync-gpu'))
    Add-Cfg 'D' 'SW_720p_30' (@('--software', '--fps', '30'))
    Add-Cfg 'D' 'SW_720p_60' (@('--software', '--fps', '60'))
    Add-Cfg 'E' 'LL_720p_23' ($LL + @('--fps', '23'))
    Add-Cfg 'E' 'LL_720p_37' ($LL + @('--fps', '37'))
    Add-Cfg 'E' 'NLL_B0_720p_37' ($NLLB0 + @('--fps', '37'))
}

# ======================================================================
# 系統資訊
# ======================================================================
$Sys = New-Object System.Collections.Generic.List[string]
$RefreshHz = 60.0
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $Sys.Add(('OS       : {0} {1} (build {2})' -f $os.Caption, $os.Version, $os.BuildNumber))
} catch { $Sys.Add('OS       : (unavailable)') }
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $Sys.Add(('CPU      : {0}' -f $cpu.Name.Trim()))
} catch {}
try {
    $hz = @()
    foreach ($g in (Get-CimInstance Win32_VideoController)) {
        $Sys.Add(('GPU      : {0} | driver {1} | {2}x{3} @ {4} Hz' -f $g.Name, $g.DriverVersion,
                $g.CurrentHorizontalResolution, $g.CurrentVerticalResolution, $g.CurrentRefreshRate))
        if ($g.CurrentRefreshRate -gt 1) { $hz += [double]$g.CurrentRefreshRate }
    }
    if ($hz.Count -gt 0) { $RefreshHz = ($hz | Measure-Object -Maximum).Maximum }
} catch {}
try {
    $bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($bat) {
        $st = switch ($bat.BatteryStatus) { 1 { 'on battery' } 2 { 'AC power' } default { "status $($bat.BatteryStatus)" } }
        $Sys.Add(('Power    : {0}, battery {1}%' -f $st, $bat.EstimatedChargeRemaining))
    } else { $Sys.Add('Power    : no battery (desktop)') }
} catch {}
try { $Sys.Add(('PowerPlan: {0}' -f ((powercfg /getactivescheme) -join ' ').Trim())) } catch {}
$Sys.Add(('Refresh  : {0} Hz used for phase analysis' -f $RefreshHz))
$Sys.Add(('Tools    : ffprobe={0}, ffmpeg={1}' -f $(if ($Ffprobe) { $Ffprobe } else { 'not found' }), $(if ($Ffmpeg) { $Ffmpeg } else { 'not found' })))
$Sys.Add(('Settings : repeat={0}, seconds={1}, configs={2}, quick={3}' -f $Repeat, $Seconds, $Configs.Count, [bool]$Quick))
$CaptureList = @()
if (Test-Path $CaptureExe) { $CaptureList = (Run-Native $CaptureExe @('--list')).Lines }

# ======================================================================
# 預先測試：確認 dda_encode 是新版（會印 RESULT 行）而且能正常執行
# ======================================================================
Write-Host '預先測試 dda_encode.exe（1 秒）...'
$pre = Run-Native $Exe @('--seconds', '1', '--out', (Join-Path $OutDir 'preflight.h264'), '--frame-log', (Join-Path $OutDir 'preflight_frames.csv'))
$pre.Lines | Set-Content -Encoding UTF8 (Join-Path $OutDir 'preflight.log')
if (-not ($pre.Lines | Where-Object { $_ -like 'RESULT *' })) {
    Write-Host 'dda_encode.exe 沒有印出 RESULT 行：可能是舊版，或執行失敗。請 git pull 後重新執行 build.bat。最後幾行輸出：' -ForegroundColor Red
    $pre.Lines | Select-Object -Last 10 | ForEach-Object { Write-Host ('  ' + $_) }
    exit 1
}

# ======================================================================
# 開始前提示
# ======================================================================
$total = $Configs.Count * $Repeat
$estSec = 0
foreach ($c in $Configs) {
    $fps = 30; $i = [Array]::IndexOf($c.Extra, '--fps'); if ($i -ge 0) { $fps = [int]$c.Extra[$i + 1] }
    $isLL = ($c.Extra -notcontains '--no-low-latency') -and ($c.Extra -notcontains '--software')
    $run = $Seconds
    if ($isLL -and $fps -gt 45) { $run = $Seconds * $fps / 45.0 }
    $estSec += ($run + 2 + $CooldownSec)
}
$estSec *= $Repeat
Write-Host ''
Write-Host ('組態 {0} 個 × {1} 次 = {2} 次執行，預估約 {3:0} 分鐘。' -f $Configs.Count, $Repeat, $total, ($estSec / 60))
Write-Host ('結果資料夾：{0}' -f $OutDir)
if (-not $Ffprobe) { Write-Host '注意：找不到 ffprobe，會略過檔案分析（把 ffmpeg 的 bin 加進 PATH 可以啟用）。' -ForegroundColor Yellow }
Write-Host '執行期間請不要操作電腦、不要移動滑鼠，並接上電源。5 秒後開始...' -ForegroundColor Yellow
Start-Sleep -Seconds 5

# ======================================================================
# 執行
# ======================================================================
$Stats = @('encode', 'late', 'gpuwait', 'wait', 'pi', 'i2n', 'lat')
$Cols = @('group', 'config', 'rep', 'exit_code', 'hw', 'async', 'd3d', 'll', 'bframes', 'sync_gpu', 'timer1ms', 'width', 'height',
    'fps', 'frames_in', 'frames_out', 'keyframes', 'new', 'repeated', 'wall_ms', 'real_fps', 'bytes', 'mbps')
foreach ($s in $Stats) { foreach ($k in @('avg', 'min', 'p50', 'p95', 'max')) { $Cols += ('{0}_{1}' -f $s, $k) } }
$Cols += @('enc_first', 'enc_rest_avg', 'pi_spread', 'R_end_vs', 'R_start_vs', 'P_vs', 'R_end_tm', 'R_start_tm', 'R_end_best', 'P_end_best',
    'R_start_best', 'P_start_best', 'late_frames', 'pi_hist', 'late_hist', 'access_lost',
    'fallback', 'encoder', 'error')

$Rows = New-Object System.Collections.Generic.List[object]
$RawResults = New-Object System.Collections.Generic.List[string]
$n = 0
$PeriodMs = 1000.0 / $RefreshHz

for ($rep = 1; $rep -le $Repeat; $rep++) {
    foreach ($c in $Configs) {
        $n++
        $base = Join-Path $OutDir ('{0}_r{1}' -f $c.Name, $rep)
        $h264 = $base + '.h264'
        $frames = $base + '_frames.csv'
        $argList = @('--seconds', "$Seconds", '--out', $h264, '--frame-log', $frames) + $c.Extra
        Write-Host -NoNewline ('[{0,3}/{1}] {2,-24} rep {3} ... ' -f $n, $total, $c.Name, $rep)

        $r = Run-Native $Exe $argList
        $r.Lines | Set-Content -Encoding UTF8 ($base + '.log')

        $row = [ordered]@{}
        foreach ($col in $Cols) { $row[$col] = '' }
        $row.group = $c.Group; $row.config = $c.Name; $row.rep = $rep; $row.exit_code = $r.Code
        $row.access_lost = @($r.Lines | Where-Object { $_ -match 'ACCESS_LOST' }).Count
        $row.fallback = [int](@($r.Lines | Where-Object { $_ -match 'Falling back to the software encoder' }).Count -gt 0)

        $res = $r.Lines | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
        if ($res) {
            $RawResults.Add(('{0} rep={1} {2}' -f $c.Name, $rep, $res))
            foreach ($m in [regex]::Matches($res, '(\w+)=("([^"]*)"|\S+)')) {
                $k = $m.Groups[1].Value
                $v = $m.Groups[2].Value
                if ($m.Groups[3].Success) { $v = $m.Groups[3].Value }
                if ($row.Contains($k)) { $row[$k] = $v }
            }
            $w = ToD $row.wall_ms; $fi = ToD $row.frames_in
            if ($w -and $fi) { $row.real_fps = ($fi * 1000.0 / $w).ToString('0.00', $Inv) }
        } else {
            $row.error = (($r.Lines | Select-Object -Last 3) -join ' | ')
        }

        # ---- 每幀明細分析 ----
        if (Test-Path $frames) {
            $fr = @(Import-Csv $frames)
            if ($fr.Count -ge 3) {
                $enc = @($fr | ForEach-Object { ToD $_.encode_ms })
                $row.enc_first = $enc[0].ToString('0.00', $Inv)
                $rest = @($enc[1..($enc.Count - 1)] | Where-Object { $_ -ge 0 })
                if ($rest.Count) { $row.enc_rest_avg = (($rest | Measure-Object -Average).Average).ToString('0.00', $Inv) }

                $piV = @($fr | Select-Object -Skip 1 | ForEach-Object { ToD $_.process_input_ms } | Where-Object { $_ -ge 0 } | Sort-Object)
                if ($piV.Count -ge 10) {
                    $lo = $piV[[int][Math]::Floor(0.05 * ($piV.Count - 1))]
                    $hi = $piV[[int][Math]::Floor(0.95 * ($piV.Count - 1))]
                    $row.pi_spread = ($hi - $lo).ToString('0.00', $Inv)   # p95 - p5
                }

                $ends = @($fr | Select-Object -Skip 1 | ForEach-Object { ToD $_.submit_end_ms } | Where-Object { $_ -ge 0 })
                $starts = @($fr | Select-Object -Skip 1 | ForEach-Object { (ToD $_.submit_end_ms) - (ToD $_.encode_ms) })
                if ($ends.Count -ge 10) {
                    $endT = [double[]]$ends; $startT = [double[]]$starts
                    # (1) 螢幕更新：WMI 會把 59.94 Hz 報成 59，所以在 59.6..60.2 Hz 附近細找
                    $vs = Get-BestPeriod $endT 16.61 16.78 0.002
                    $row.R_end_vs = $vs[0].ToString('0.000', $Inv)
                    $row.P_vs = $vs[1].ToString('0.000', $Inv)
                    $row.R_start_vs = (Get-PhaseR $startT $vs[1]).ToString('0.000', $Inv)
                    # (2) Windows 預設計時器刻度 15.625 ms
                    $row.R_end_tm = (Get-PhaseR $endT 15.625).ToString('0.000', $Inv)
                    $row.R_start_tm = (Get-PhaseR $startT 15.625).ToString('0.000', $Inv)
                    # (3) 不預設答案：15.0..17.6 ms 之間最集中的週期
                    $be = Get-BestPeriod $endT 15.0 17.6 0.01
                    $row.R_end_best = $be[0].ToString('0.000', $Inv); $row.P_end_best = $be[1].ToString('0.00', $Inv)
                    $bs = Get-BestPeriod $startT 15.0 17.6 0.01
                    $row.R_start_best = $bs[0].ToString('0.000', $Inv); $row.P_start_best = $bs[1].ToString('0.00', $Inv)
                }
                $row.pi_hist = Get-Hist ([double[]]@($fr | Select-Object -Skip 1 | ForEach-Object { ToD $_.process_input_ms }))
                $row.late_hist = Get-Hist ([double[]]@($fr | Select-Object -Skip 1 | ForEach-Object { ToD $_.late_ms }))
                $fpsV = ToD $row.fps
                if ($fpsV) {
                    $limit = 1000.0 / $fpsV
                    $row.late_frames = @($fr | Where-Object { (ToD $_.late_ms) -gt $limit }).Count
                }
            }
        }

        $Rows.Add([pscustomobject]$row)
        if ($res) {
            Write-Host ('real {0} fps | pi p50 {1} ms | lat p50 {2} ms | {3} Mbps' -f (Fmt $row.real_fps), (Fmt $row.pi_p50 '0.00'),
                (Fmt $row.lat_p50 '0.00'), (Fmt $row.mbps '0.00'))
        } else {
            Write-Host ('FAILED (exit {0}) {1}' -f $r.Code, $row.error) -ForegroundColor Red
        }

        if ($rep -gt 1 -and (Test-Path $h264)) { Remove-Item $h264 -ErrorAction SilentlyContinue }   # 只保留第 1 次的檔案給 ffprobe
        Start-Sleep -Seconds $CooldownSec
    }
}

$Rows | Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $OutDir 'runs.csv')
$RawResults | Set-Content -Encoding UTF8 (Join-Path $OutDir 'raw_results.txt')

# ======================================================================
# ffprobe / ffmpeg 分析（每個組態第 1 次的檔案）
# ======================================================================
$Probe = New-Object System.Collections.Generic.List[object]
if ($Ffprobe) {
    Write-Host ''
    Write-Host '分析輸出檔（ffprobe / ffmpeg）...'
    foreach ($c in $Configs) {
        $file = Join-Path $OutDir ('{0}_r1.h264' -f $c.Name)
        if (-not (Test-Path $file)) { continue }
        $o = [ordered]@{ config = $c.Name; nb_frames = ''; has_b = ''; profile = ''; level = ''; color_range = ''; color_space = '';
            color_primaries = ''; color_transfer = ''; key_n = ''; key_avg_kb = ''; p_avg_kb = ''; p_min_b = ''; p_max_kb = '';
            vui_present = ''; full_range = ''; primaries = ''; transfer = ''; matrix = ''; max_ref = ''; reorder = ''; nal = '' }

        foreach ($l in (Run-Native $Ffprobe @('-v', 'error', '-count_frames', '-select_streams', 'v:0', '-show_entries',
                    'stream=profile,level,nb_read_frames,has_b_frames,color_range,color_space,color_primaries,color_transfer',
                    '-of', 'default=nw=1', $file)).Lines) {
            if ($l -match '^(\w+)=(.*)$') {
                $k = $Matches[1]; $v = $Matches[2]
                if ($k -eq 'nb_read_frames') { $o.nb_frames = $v }
                elseif ($k -eq 'has_b_frames') { $o.has_b = $v }
                elseif ($o.Contains($k)) { $o[$k] = $v }
            }
        }

        $keys = @(); $ps = @()
        foreach ($l in (Run-Native $Ffprobe @('-v', 'error', '-select_streams', 'v:0', '-show_entries', 'packet=size,flags',
                    '-of', 'csv=p=0', $file)).Lines) {
            if ($l -match '^(\d+),(\S*)') {
                $sz = [double]$Matches[1]
                if ($Matches[2] -like '*K*') { $keys += $sz } else { $ps += $sz }
            }
        }
        if ($keys.Count) { $o.key_n = $keys.Count; $o.key_avg_kb = (($keys | Measure-Object -Average).Average / 1024).ToString('0.0', $Inv) }
        if ($ps.Count) {
            $mo = $ps | Measure-Object -Average -Minimum -Maximum
            $o.p_avg_kb = ($mo.Average / 1024).ToString('0.0', $Inv)
            $o.p_min_b = $mo.Minimum
            $o.p_max_kb = ($mo.Maximum / 1024).ToString('0.0', $Inv)
        }

        if ($Ffmpeg) {
            # 只追蹤前 60 張：trace_headers 的輸出很大，整個檔案太慢
            $tr = (Run-Native $Ffmpeg @('-hide_banner', '-nostats', '-i', $file, '-c', 'copy', '-frames:v', '60', '-bsf:v', 'trace_headers',
                    '-f', 'null', '-')).Lines
            $nal = @{}
            foreach ($l in $tr) {
                if ($l -match 'nal_unit_type\s+[01]+\s*=\s*(\d+)') {
                    $t = [int]$Matches[1]
                    if ($nal.ContainsKey($t)) { $nal[$t]++ } else { $nal[$t] = 1 }
                }
            }
            $o.nal = (($nal.Keys | Sort-Object | ForEach-Object { '{0}:{1}' -f $_, $nal[$_] }) -join ' ')
            $first = @{
                vui_present = 'video_signal_type_present_flag'; full_range = 'video_full_range_flag';
                primaries = 'colour_primaries'; transfer = 'transfer_characteristics'; matrix = 'matrix_coefficients';
                max_ref = 'max_num_ref_frames'; reorder = 'max_num_reorder_frames'
            }
            foreach ($k in $first.Keys) {
                $pat = $first[$k] + '\s+[01]+\s*=\s*(\d+)'
                $hit = $tr | Where-Object { $_ -match $pat } | Select-Object -First 1
                if ($hit -and ($hit -match $pat)) { $o[$k] = $Matches[1] }
            }
        }
        $Probe.Add([pscustomobject]$o)
        Write-Host ('  {0}' -f $c.Name)
    }
    $Probe | Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $OutDir 'probe.csv')
}

# ======================================================================
# 彙整報告
# ======================================================================
function Agg($rows, [string]$col, [string]$how = 'mean') {
    $v = @($rows | ForEach-Object { ToD $_.$col } | Where-Object { $null -ne $_ })
    if ($v.Count -eq 0) { return $null }
    $m = $v | Measure-Object -Average -Minimum -Maximum
    if ($how -eq 'min') { return $m.Minimum }
    if ($how -eq 'max') { return $m.Maximum }
    return $m.Average
}

$Report = New-Object System.Collections.Generic.List[string]
$Report.Add('=== encoder_bench report ===')
$Report.Add(('Date     : {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
foreach ($s in $Sys) { $Report.Add($s) }
if ($CaptureList.Count) { $Report.Add('dda_capture --list:'); foreach ($l in $CaptureList) { $Report.Add('  ' + $l) } }
$enc = $Rows | Where-Object { $_.encoder } | Select-Object -ExpandProperty encoder -Unique
foreach ($e in $enc) { $Report.Add(('Encoder  : {0}' -f $e)) }
$Report.Add('')
$Report.Add('All times in ms. Values are the mean over runs unless noted; "min"/"max" columns are across runs.')
$Report.Add('pi = ProcessInput, lat = input->output latency, late = how late the frame was submitted vs schedule.')
$Report.Add('')

$hdr = '{0,-24} {1,5} {2,13} {3,7} {4,7} {5,7} {6,7} {7,7} {8,7} {9,7} {10,7} {11,6}'
$Report.Add('--- Table 1: throughput and latency ---')
$Report.Add(($hdr -f 'config', 'ok', 'real fps(min)', 'enc avg', 'pi p50', 'pi p95', 'lat p50', 'lat p95', 'lat max', 'late95', 'Mbps', 'kf'))
foreach ($c in $Configs) {
    $rs = @($Rows | Where-Object { $_.config -eq $c.Name })
    $ok = @($rs | Where-Object { $_.wall_ms -ne '' -and $_.frames_out -eq $_.frames_in }).Count
    $fpsTxt = '{0}({1})' -f (Fmt (Agg $rs 'real_fps')), (Fmt (Agg $rs 'real_fps' 'min'))
    $Report.Add(($hdr -f $c.Name, ('{0}/{1}' -f $ok, $rs.Count), $fpsTxt, (Fmt (Agg $rs 'encode_avg') '0.00'),
            (Fmt (Agg $rs 'pi_p50') '0.00'), (Fmt (Agg $rs 'pi_p95') '0.00'), (Fmt (Agg $rs 'lat_p50') '0.00'),
            (Fmt (Agg $rs 'lat_p95') '0.00'), (Fmt (Agg $rs 'lat_max' 'max') '0.0'), (Fmt (Agg $rs 'late_p95') '0.0'),
            (Fmt (Agg $rs 'mbps') '0.00'), (Fmt (Agg $rs 'keyframes') '0')))
}
$Report.Add('')

$hdr2 = '{0,-24} {1,7} {2,7} {3,7} {4,7} {5,8} {6,7} {7,7} {8,7} {9,7} {10,8} {11,6}'
$Report.Add('--- Table 2: diagnostics ---')
$Report.Add('wait = waiting for NeedInput, i2n = ProcessInput return -> next NeedInput, first = Encode() of frame 0,')
$Report.Add('spread = ProcessInput p95-p5 (ms), lateF = frames submitted more than one frame period late.')
$Report.Add(($hdr2 -f 'config', 'wait', 'i2n p50', 'i2n p95', 'gpuwait', 'first', 'rest', 'spread', 'lateF', '', '', ''))
foreach ($c in $Configs) {
    $rs = @($Rows | Where-Object { $_.config -eq $c.Name })
    $Report.Add(($hdr2 -f $c.Name, (Fmt (Agg $rs 'wait_avg') '0.00'), (Fmt (Agg $rs 'i2n_p50') '0.00'), (Fmt (Agg $rs 'i2n_p95') '0.00'),
            (Fmt (Agg $rs 'gpuwait_avg') '0.00'), (Fmt (Agg $rs 'enc_first') '0.0'), (Fmt (Agg $rs 'enc_rest_avg') '0.00'),
            (Fmt (Agg $rs 'pi_spread') '0.00'), (Fmt (Agg $rs 'late_frames') '0'), '', '', ''))
}
$Report.Add('')

$hdr5 = '{0,-24} {1,7} {2,7} {3,7} {4,7} {5,7} {6,8} {7,7} {8,8} {9,7}'
$Report.Add('--- Table 2b: phase locking of Encode() end / start times ---')
$Report.Add('R = Rayleigh concentration of the times modulo a period: 1 = always the same phase, ~0.1 = random for ~150 frames.')
$Report.Add('vs = display refresh (best period in 16.61..16.78 ms), tm = Windows timer tick 15.625 ms,')
$Report.Add('best = most concentrated period anywhere in 15.0..17.6 ms. Locked ends with unlocked starts = waiting for that clock.')
$Report.Add(($hdr5 -f 'config', 'Rend vs', 'Rsta vs', 'P vs', 'Rend tm', 'Rsta tm', 'Rend bst', 'P end', 'Rsta bst', 'P sta'))
foreach ($c in $Configs) {
    $rs = @($Rows | Where-Object { $_.config -eq $c.Name })
    $Report.Add(($hdr5 -f $c.Name, (Fmt (Agg $rs 'R_end_vs') '0.000'), (Fmt (Agg $rs 'R_start_vs') '0.000'), (Fmt (Agg $rs 'P_vs') '0.000'),
            (Fmt (Agg $rs 'R_end_tm') '0.000'), (Fmt (Agg $rs 'R_start_tm') '0.000'), (Fmt (Agg $rs 'R_end_best') '0.000'),
            (Fmt (Agg $rs 'P_end_best') '0.00'), (Fmt (Agg $rs 'R_start_best') '0.000'), (Fmt (Agg $rs 'P_start_best') '0.00')))
}
$Report.Add('')

function Sum-Hist($rs, [string]$col) {
    $tot = New-Object int[] 21
    foreach ($x in $rs) {
        $parts = "$($x.$col)" -split ' '
        if ($parts.Count -ne 21) { continue }
        for ($i = 0; $i -lt 21; $i++) { $tot[$i] += [int]$parts[$i] }
    }
    return $tot
}
foreach ($hc in @(@('pi_hist', 'ProcessInput'), @('late_hist', 'submit lateness'))) {
    $Report.Add(('--- Table 5: {0} histogram, frames per 2 ms bin, all runs summed (frame 0 excluded) ---' -f $hc[1]))
    $binHdr = ((0..19 | ForEach-Object { '{0,4}' -f ($_ * 2) }) -join '') + ' >=40'   # 先組好：逗號比 + 先結合
    $Report.Add(('{0,-24} {1}' -f 'config \ ms', $binHdr))
    foreach ($c in $Configs) {
        $rs = @($Rows | Where-Object { $_.config -eq $c.Name })
        $tot = Sum-Hist $rs $hc[0]
        $Report.Add(('{0,-24} {1}' -f $c.Name, (($tot | ForEach-Object { '{0,4}' -f $_ }) -join '')))
    }
    $Report.Add('')
}

$Report.Add('--- Table 3: run-to-run variation (per run: real fps / pi p50 / lat p50) ---')
foreach ($c in $Configs) {
    $rs = @($Rows | Where-Object { $_.config -eq $c.Name })
    $parts = $rs | ForEach-Object { '{0}/{1}/{2}' -f (Fmt $_.real_fps), (Fmt $_.pi_p50 '0.0'), (Fmt $_.lat_p50 '0.0') }
    $Report.Add(('{0,-24} {1}' -f $c.Name, ($parts -join '   ')))
}
$Report.Add('')

if ($Probe.Count) {
    $hdr3 = '{0,-24} {1,6} {2,3} {3,-9} {4,3} {5,-14} {6,4} {7,6} {8,6} {9,7} {10,7} {11,-10} {12}'
    $Report.Add('--- Table 4: bitstream (first run of each config) ---')
    $Report.Add('VUI = video_signal_type_present / full_range / primaries / transfer / matrix (from the SPS),')
    $Report.Add('NAL = nal_unit_type:count in the first 60 frames (1 = P slice, 5 = IDR, 6 = SEI, 7 = SPS, 8 = PPS, 9 = AUD, 12 = filler).')
    $Report.Add(($hdr3 -f 'config', 'frames', 'hasB', 'profile', 'lvl', 'range/space', 'kf', 'kf KB', 'P KB', 'P min B', 'P maxKB', 'VUI', 'NAL'))
    foreach ($p in $Probe) {
        $vui = '{0}/{1}/{2}/{3}/{4}' -f $p.vui_present, $p.full_range, $p.primaries, $p.transfer, $p.matrix
        $Report.Add(($hdr3 -f $p.config, $p.nb_frames, $p.has_b, $p.profile, $p.level, ('{0}/{1}' -f $p.color_range, $p.color_space),
                $p.key_n, $p.key_avg_kb, $p.p_avg_kb, $p.p_min_b, $p.p_max_kb, $vui, $p.nal))
    }
    $Report.Add('')
}

$bad = @($Rows | Where-Object { $_.error -or ("$($_.exit_code)" -ne '0') -or (ToD $_.access_lost) -gt 0 -or $_.fallback -eq 1 })
if ($bad.Count) {
    $Report.Add('--- Problems ---')
    foreach ($b in $bad) { $Report.Add(('{0} rep {1}: exit={2} access_lost={3} fallback={4} {5}' -f $b.config, $b.rep, $b.exit_code, $b.access_lost, $b.fallback, $b.error)) }
    $Report.Add('')
}

$summary = Join-Path $OutDir 'summary.txt'
$Report | Set-Content -Encoding UTF8 $summary
Write-Host ''
$Report | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ('完成。請把這個檔案的完整內容貼回來：{0}' -f $summary) -ForegroundColor Green
Write-Host ('每次執行的明細在 {0}（runs.csv、raw_results.txt、*_frames.csv、*.log）' -f $OutDir)
