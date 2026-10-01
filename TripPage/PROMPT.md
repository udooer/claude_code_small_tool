# Agent 整理指令

把使用者提供的原始行程 (Google Sheet CSV / 貼上的文字 / 備忘錄) 轉成 `trip.json`，
格式參考 `examples/trip.example.json`。

## 規則
1. **只整理，不編造**：原文沒有的時間、地址、價格、訂位編號一律留空，不要猜。
2. 日期統一 `YYYY-MM-DD`；時間 24 小時制 `HH:MM`；每天 items 依時間排序。
3. `type` 只能是 `transport` / `sight` / `food` / `lodging` / `other`。
4. 住宿放在該天的 `lodging`；跨多晚則每天重複。
5. 「當天重點」→ `highlight`；「注意事項/提醒」→ 該天 `notes`；全程通用的 → `overview.notes`。
6. 有地址就填 `address` (網頁會自動產生 Google Maps 連結)；有現成連結填 `map` / `url`。
7. 訂位/訂單/票券編號 → `booking`；費用 → `cost`。
8. 原文含糊或互相矛盾 (如時間衝突、缺日期) → 不要自行決定，列在回覆最後的「待確認」清單給使用者。
9. 輸出完成後執行 `python TripPage/build.py trip.json` 驗證能正常產生。
