# PerfCollector — 可攜式 CPU / 效能問題收集工具

把整個 `PerfCollector` 資料夾複製到 USB 或網路磁碟，在問題機台**點兩下 `Collect.cmd`** 就會收集分析 CPU 異常所需的資料，結束後產生一個 zip 帶回分析。

- **不需要安裝**：只使用 Windows 內建元件（Windows PowerShell 3.0+、`wpr.exe`、`typeperf`、`wevtutil`）。
- `tools\` 放了 `procdump`、WPT 時會優先使用它們，這樣舊系統也能錄 ETW、也能抓 dump（見下方〈準備工具〉）。
- 需要系統管理員權限。沒有的話會自動跳 UAC 提權。

## 收集的內容

| 輸出 | 內容 | 用途 |
|---|---|---|
| `trace_<主機>.etl` | WPR ETW trace：CPU 取樣、context switch、ready thread stack，Standard 以上再加 Disk/File IO | 用 **WPA** 看到函式層級的熱點 |
| `counters_001.csv` | 每秒效能計數器：總 CPU、每顆核心、Privileged/Interrupt/DPC、處理器效能%/頻率、Queue Length、Context Switch、記憶體、磁碟、網路，以及**每個 process** 的 CPU/Thread/Handle/Private Bytes/IO | 看時間趨勢，可以用 Excel 或 PerfMon 開 |
| `process_cpu_window.csv` | 錄製期間各 process 的 CPU 使用量 | 快速排名 |
| `thread_cpu_window.csv` | 目標 process（或前 5 名）**每個 thread** 的 CPU、優先權、狀態、Start address | TID 可以直接對到 WPA 的 Thread ID |
| `processes_before/after.csv` | 所有 process 的 PID、父 PID、命令列、Handle、記憶體、CPU 時間 | 比對錄製前後的變化 |
| `sysinfo\` | systeminfo、OS、Hotfix、CPU/BIOS/記憶體、**電源計畫**、驅動程式、服務、已安裝軟體、開機啟動項、排程工作、網路、磁碟、**防毒/Defender 狀態與排除清單**、WER 報告清單、目標 process 的 module 版本 | 檢查環境差異 |
| `eventlogs\` | System、Application、Diagnostics-Performance、Resource-Exhaustion、Thermal、WMI-Activity、Defender（`.evtx`，已內嵌訊息文字），另附 `errors_warnings.csv` | 對照事件時間點 |
| `dumps\` | （選用 `-Dump`）目標 process 的 3 份 full dump，每份間隔 10 秒 | 在 Visual Studio / WinDbg 看當下的 stack |
| `summary.txt` | **自動摘要**：Top CPU process、最忙的 thread、核心/中斷/DPC/降頻/記憶體指標，以及判斷提示 | 第一眼先看這個 |

## 使用方式

### 1. 點兩下（選單模式）

```
1) 立即錄 60 秒，Standard（建議）
2) 指定 process 錄製（加上 thread 明細，可選擇抓 dump）
3) 等 CPU 飆高時自動錄（Trigger 模式）
4) 立即錄 60 秒，Full（provider 較多，檔案較大）
5) 只收系統資訊和計數器（不錄 ETW）
```

### 2. 指令列

```bat
:: 預設：60 秒、Standard
Collect.cmd -Duration 60

:: 指定 process（名稱或 PID，可用逗號分隔），同時抓 3 份 dump
Collect.cmd -ProcessName myapp -Duration 120 -Dump
Collect.cmd -ProcessId 1234,5678

:: 偶發異常：myapp 佔整機 >= 25% 並持續 10 秒時自動停止，觸發後再多錄 15 秒
Collect.cmd -Trigger -ProcessName myapp -CpuThreshold 25 -SustainSeconds 10

:: 整機 CPU >= 90% 持續 30 秒時觸發，最多等 8 小時
Collect.cmd -Trigger -CpuThreshold 90 -SustainSeconds 30 -MaxWaitMinutes 480

:: 輸出到其他磁碟（USB 太小或太慢時）
Collect.cmd -OutputDir D:\perf
```

| 參數 | 預設 | 說明 |
|---|---|---|
| `-Duration` | 60 | 固定模式的錄製秒數 |
| `-ProcessName` / `-ProcessId` | — | 目標 process，影響 trigger 判斷、thread 明細、module 清單和 dump |
| `-Level` | Standard | `Quick`＝只錄 CPU；`Standard`＝CPU + DiskIO + FileIO；`Full`＝再加 GeneralProfile、Registry、Network |
| `-Trigger` | off | 用環狀記憶體 buffer 持續錄，條件成立時自動存檔，所以**飆高前的資料也會留下來** |
| `-CpuThreshold` | 80 | 佔**整機**的百分比（和工作管理員相同）。有指定目標時套用到目標 process，否則套用到總 CPU |
| `-SustainSeconds` | 10 | 要持續超過門檻幾秒才觸發 |
| `-PostTriggerSeconds` | 15 | 觸發後再多錄幾秒 |
| `-MaxWaitMinutes` | 1440 | Trigger 模式最多等多久，逾時也會存檔 |
| `-SampleInterval` | 1 | 計數器取樣間隔（秒） |
| `-EventLogDays` | 7 | 匯出最近幾天的事件記錄，設 0 表示不匯出 |
| `-Dump` | off | 用 procdump 抓 full dump，需要 `tools\procdump64.exe` |
| `-NoTrace` | off | 不錄 ETW，負擔最低 |
| `-NoZip` | off | 不壓縮，保留資料夾 |
| `-OutputDir` | `.\output` | 輸出位置 |

Trigger 模式等待時按 **S** 會立即存檔，按 **Q** 會中止（已錄的內容仍會存下來）。

結果會放在 `output\PerfLogs_<主機>_<時間>.zip`。

## 準備工具（在有網路的機器上做一次）

```powershell
.\Prepare-Tools.ps1
```

- 下載 `procdump64.exe` / `procdump.exe` 到 `tools\`，`-Dump` 會用到。
- 如果這台機器有安裝 Windows ADK 的 *Windows Performance Toolkit*，會把 `wpr.exe`、`xperf.exe` 和相關 DLL 複製到 `tools\wpt\`。這樣在**沒有內建 wpr 的舊系統**（Win 8.1 / Server 2012 R2 以前）也能錄；沒有 WPR 時會自動改用 xperf。

Windows 10/11 和 Server 2016 以上都內建 `wpr.exe`，不準備 `tools\` 也可以錄 trace。

## 分析流程

1. 先看 `summary.txt`，大方向通常就出來了。例如：
   - 某 process 剛好吃滿 1 顆核心：單一 thread 卡在迴圈或 busy wait。`thread_cpu_window.csv` 會列出是哪個 TID。
   - Interrupt + DPC 偏高：驅動程式問題。在 WPA 開 **DPC/ISR**。
   - Kernel 比例高：syscall、I/O、防毒 filter 或 paging。
   - Processor Performance 遠低於 100%：降頻，原因可能是電源計畫、溫度或 BIOS。
   - Handle 或 Private Bytes 一直增加：可能有洩漏，會連帶造成 GC 或 paging 吃 CPU。
2. 用 **WPA** 開 `trace_*.etl`：
   - Trace → Configure Symbol Paths：`srv*C:\symbols*https://msdl.microsoft.com/download/symbols;<自家 PDB 目錄>` → Load Symbols。
   - **CPU Usage (Sampled)**：依 Process → Thread ID → Stack 分組，找 Weight 最大的路徑。
   - **CPU Usage (Precise)**：看 Ready Time（搶不到 CPU）和 Wait（lock / I/O）。
3. 有 dump 的話，用 WinDbg（`!runaway`、`~*k`）或 Visual Studio 看那個時間點的 stack。

### 常見高 CPU 的系統 process

| Process | 可能原因 | 進一步檢查 |
|---|---|---|
| `MsMpEng` | Defender 正在即時掃描你的程式或檔案 | `antivirus.txt` 的排除清單；在 WPA 看它在掃哪些檔案（FileIO） |
| `WmiPrvSE` | 有 WMI client 頻繁查詢 | `eventlogs\Microsoft-Windows-WMI-Activity_Operational.evtx` 會列出 ClientProcessId |
| `svchost` | 某個服務 | 對照 `tasklist_svc.txt` 的 PID 找出是哪個服務 |
| `System` / `Interrupts` | 驅動程式或 DPC/ISR | WPA 的 DPC/ISR 表，以及 `System` 程序 stack 上的 `.sys` |
| `TiWorker` / `TrustedInstaller` | Windows Update | `hotfix.csv` 和事件記錄 |
| `csrss` / `dwm` | 視窗訊息量過大或 GPU 問題 | 看是哪個應用程式在洗訊息 |

## 注意事項

- WPR 一次只能有一個 session。工具開始前會先執行 `wpr -cancel`，所以**正在跑的其他 WPR 錄製會被取消**。
- 錄製時會在系統碟暫存資料。Full 等級的 ETL 可能到數 GB，請確認磁碟空間足夠；空間不足時工具會提示。
- 不要在 trace 合併時關閉視窗（`Stopping trace and merging...` 那一步）。
- 收集內容包含命令列、環境變數、軟體清單、網路連線等機台資訊，交給外部前請先確認是否符合資安規範。
- 如果 `ExecutionPolicy` 被 GPO 鎖死（MachinePolicy），`-ExecutionPolicy Bypass` 也會失效，請洽 IT。
