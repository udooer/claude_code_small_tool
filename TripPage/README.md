# TripPage

把旅遊行程（Google Sheet / 文字）整理成手機友善的靜態網頁，部署到免費主機，旅途中用網址查看。

```
 Google Sheet / 文字 ──► Claude 整理 ──► trips/<名稱>.json ──► build.py ──► 靜態 HTML ──► GitHub Pages
   (輸入)             (PROMPT.md)        (固定格式)                                  https://<帳號>.github.io/<repo>/<名稱>/
```

## 流程

1. **輸入**
   - Google Sheet：設為「知道連結者可檢視」，`python TripPage/fetch_sheet.py "<網址>" > TripPage/raw.csv`（或直接貼給 Claude）
   - **多分頁的 Sheet（每天一頁）建議用 xlsx 匯入**：檔案 → 下載 → Microsoft Excel (.xlsx)，本機執行
     `python TripPage/import_xlsx.py 行程.xlsx --year 2026 --title "標題" -o TripPage/trips/<亂碼名稱>.json`
     （完整讀取儲存格，不會被截斷，資料也不經過任何第三方；分頁名稱需為 `10/9 (Day 1) 維也納` 格式）
   - 文字：直接貼給 Claude
2. **整理**：請 Claude「依 `TripPage/PROMPT.md` 把 raw 內容整理成 `TripPage/trips/<名稱>.json`」。含糊或矛盾處會列出「待確認」，不會亂猜。
3. **建置（加密）**：`pip install cryptography`，然後
   `python TripPage/build.py TripPage/trips/<名稱>.json -o TripPage/site/<亂碼名稱> --password "長密碼"`
4. **發佈**（擇一）
   - **Netlify**：把 `TripPage/site/<亂碼名稱>/` 拖進 Netlify Drop
   - **GitHub Pages**：commit `TripPage/site/` 並 push 到 `main`，Actions 自動部署
5. **使用**：手機開網址 → 輸入一次密碼（之後記在手機上）→「加入主畫面」。

## 網頁功能

- 上方日期分頁，**自動跳到「今天」**那一天；其他日期可手動切換
- 每日：當天重點、天氣、住宿（地圖/撥號）、時間軸（交通/景點/餐食顏色區分）、注意事項
- 地圖按鈕由地址自動產生 Google Maps 連結；電話可直接撥打
- 行李清單可勾選（記在手機本機）、緊急聯絡電話
- 單一 HTML、無外部資源、深色模式、可列印；載入一次後離線也能看

## 一次性設定（僅 GitHub Pages）

repo → Settings → Pages → Source 選 **GitHub Actions**；本分支合併到 `main` 後生效。

## 隱私與開源安全

- `--password` 以 PBKDF2-SHA256（60 萬次）+ AES-256-GCM 加密整個頁面；沒有密碼只看得到輸入框，**加密後的檔案可以安全放 public repo 或任何免費主機**。
- `.gitignore` 已排除 `TripPage/trips/`（明文 JSON）、`dist/`、`raw*`、`*.csv`，**明文行程不會進 repo**。Actions 也會拒絕部署未加密頁面。
- 密碼請用 4 個以上英文單字（例如 `correct horse battery staple`）；這是靜態加密，擋不住拿到檔案後離線暴力猜密碼，所以密碼強度就是安全性。
- 不要把密碼寫進 repo、commit 訊息或 issue。忘記密碼只能重新建置。
- 重新建置且改密碼後，手機會自動要求輸入新密碼。

## 欄位格式

見 `examples/trip.example.json`。必填只有 `days[].date` 與 `items[].title`，其餘皆選填。
