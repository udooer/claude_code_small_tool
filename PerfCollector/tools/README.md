# tools/

Optional binaries. PerfCollector uses them when present and falls back to built-in Windows tools.

| File | Purpose | When needed |
|---|---|---|
| `procdump64.exe` / `procdump.exe` | Takes full memory dumps with `-Dump` | Only if you need dumps |
| `wpt\wpr.exe`, `wpt\xperf.exe` + DLLs | ETW recorder | Only on OSes without inbox `wpr.exe` (Win 8.1 / Server 2012 R2 and older). Can also be used to pin a newer WPR version. |

Run `..\Prepare-Tools.ps1` on a machine with internet access and the Windows ADK
(Windows Performance Toolkit feature) to fill this folder automatically.
Binaries are not committed to git (see `.gitignore`).
