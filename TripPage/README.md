# TripPage

把旅遊行程（Google Sheet / 文字）整理成手機友善的靜態網頁，部署到免費主機，旅途中用網址查看。

```
 Google Sheet / 文字 ──► Claude 整理 ──► trips/<名稱>.json ──► build.py ──► 靜態 HTML ──► GitHub Pages
   (輸入)             (PROMPT.md)        (固定格式)                                  https://<帳號>.github.io/<repo>/<名稱>/
```

## 流程

1. **輸入**
   - Google Sheet：設為「知道連結者可檢視」，`python TripPage/fetch_sheet.py "<網址>" > raw.csv`（或直接把內容貼給 Claude）
   - 文字：直接貼給 Claude
2. **整理**：請 Claude「依 `TripPage/PROMPT.md` 把 raw 內容整理成 `TripPage/trips/<名稱>.json`」。含糊或矛盾處它會列出「待確認」，不會亂猜。
3. **預覽**：`python TripPage/build.py TripPage/trips/<名稱>.json -o dist` → 開 `dist/index.html`
4. **發佈**：commit + push 到 `main`，GitHub Actions 自動把所有 `trips/*.json` 建置並部署。
5. **使用**：手機開網址 →「加入主畫面」。

## 網頁功能

- 上方日期分頁，**自動跳到「今天」**那一天；其他日期可手動切換
- 每日：當天重點、天氣、住宿（地圖/撥號）、時間軸（交通/景點/餐食顏色區分）、注意事項
- 地圖按鈕由地址自動產生 Google Maps 連結；電話可直接撥打
- 行李清單可勾選（記在手機本機）、緊急聯絡電話
- 單一 HTML、無外部資源、深色模式、可列印；載入一次後離線也能看

## 一次性設定

1. repo → Settings → Pages → Source 選 **GitHub Actions**
2. 把本分支合併到 `main`（workflow 只在 `main` push 時觸發，也可手動 Run workflow）

## 隱私提醒

行程含住宿地址與訂位編號。GitHub Pages 網址是公開的（有網址就能看），且 public repo 內的 JSON 也看得到：

- 檔名取不易猜的名稱，例如 `kyoto-7f3a9c2e.json`；網頁已設 `noindex`
- 訂位編號等敏感資料可不要放進行程
- 更保守：使用 private repo（GitHub Pages 需付費方案）或改用 Cloudflare Pages + Access，`dist/` 資料夾可直接上傳

## 欄位格式

見 `examples/trip.example.json`。必填只有 `days[].date` 與 `items[].title`，其餘皆選填。
