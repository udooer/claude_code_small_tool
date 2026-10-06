# Lab 帶讀：一步一步走過 Lab 1~6

這份文件帶你親手跑過每一題的參考實作。每一題分成四段：

1. **跑起來**：指令，以及你應該看到什麼。
2. **程式導讀**：只看關鍵的那幾行，每一行對應一個核心觀念。
3. **動手弄壞它**：故意改錯一個地方，親眼看到症狀。之後在學員作業裡看到同樣的症狀，就能直接認出原因。
4. **對照 mentor 指南**：這一題的參考實作對應指南裡的哪些 checklist 項目。

建議一次走一題。每題大約 20~40 分鐘。

> 下面的輸出都是**示意**，實際數字會因你的 GPU、驅動、解析度而不同。
> 建置方式見 [README.md](README.md)。以下指令假設你在 `build\Release\` 目錄。

---

## 開始之前：先跑兩個不需要螢幕的測試

```bat
portable_tests.exe
codec_loopback.exe
codec_loopback.exe --hw
```

- `portable_tests`：檢查 YUV 公式、NAL 切割、TCP 訊框、BMP 寫檔。結尾印出 `ALL PASSED` 才對。
- `codec_loopback`：產生測試畫面 → 編碼 → 解碼，檢查張數與顏色，用來確認這台機器的 Media Foundation 環境沒問題。加上 `--hw` 會改用硬體 encoder。
  - `--hw` 走的是和 Lab 4/6 相同的路徑：D3D11 device + device manager + GPU NV12 texture。
  - **實機紀錄（Intel Quick Sync）**：硬體 encoder 如果只餵 CPU 記憶體、不給 D3D device（`--hw-sysmem`），Intel QSV 會在 `ProcessOutput` 回 `0x8000FFFF E_UNEXPECTED`。這是很好的教材：硬體 encoder 是為「GPU texture 進、bitstream 出」設計的，**device manager 不是可有可無的效能選項**。
  - 混合顯卡筆電會列出多個硬體 encoder（例如 Intel + NVIDIA）。程式會優先挑和 D3D11 device **同一張 GPU** 的那一個，並標示 `<- same GPU as our D3D11 device`。
  - **硬體 encoder 完全不能用時**：先跑 `codec_loopback.exe --probe`，它會試遍各種組態並印出 GPU driver 版本。如果全部失敗，用 FFmpeg 的 Media Foundation encoder 交叉比對：
    ```bat
    ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 3 -c:v h264_mf -hw_encoding 1 -y mf_hw_test.mp4
    ```
    FFmpeg 也失敗 → 是 driver 的問題，請更新顯卡 driver（Intel 可用 Intel Driver & Support Assistant）。FFmpeg 成功 → 是我們程式的問題，請回報。
    Lab 4 與 Lab 6 在硬體 encoder 一開始就失敗時，會自動改用軟體 encoder 繼續執行。
