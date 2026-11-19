# MXCam Studio 跟著 MATRIXblock 新版（beta）的淺色外觀

MXCam 是給 MATRIX Mini R4 用的，學生寫 R4 程式用的是 MATRIXblock（積木式編輯器）。原本 Studio 是深色、琥珀橘強調色的「儀器」風格，跟學生手上的另一個工具完全不像。決定：Studio 的外觀跟著 **MATRIXblock 新版（beta，strawbArry_beta.0.1）** 走——白色上方列、膠囊分頁、白色圓角卡片、淡青綠標籤、左側彩色圓形分類列、點格工作區、深青藍積木樣式的參數（白色膠囊數值欄）、分組清單式的設定頁、對話框樣式的確認。顏色、圓角、陰影直接用 beta 的數值（強調色 `#008184`、底色 `#F5F6F7`、圓角 8 / 10 / 14 / 20 px 等），樣式則是自己重寫的 CSS。細節記在 `docs/studio-ui.md` §9。

不參考 v1.0.x 舊版（深青綠實心上方列、藍色積木）：使用者指定以新版為準，舊版之後會被取代。

只做淺色，不做深色模式：MATRIXblock 只有淺色，維護兩套配色的成本不值得。即時畫面保留深色外框，因為攝影機畫面在深色背景上比較好判讀。

## Considered Options

- **維持深色儀器風格**：本身沒有問題，但跟學生的主要工具差異大，教室裡切換兩個工具時要重新適應。
- **照 MATRIXblock v1.0.x 舊版**：第一次改版做過，使用者要求改成新版的樣子。
- **淺色 + 深色兩套**：多一倍的顏色驗證工作（辨識框顏色、積木對比），目前沒有需求。
- **直接複製 beta 的 CSS、圖片或套用 MATRIX 品牌**：esp32S3_cam 是公開 repo，beta 的程式碼在私有 repo；而且 MXCam 不是 MATRIX 的產品。只沿用數值，不複製檔案、logo、名稱或文字。

## Consequences

- 辨識框顏色 `--id-1`～`--id-8` 不受影響，仍與韌體 TFT 的 `k_id_colors` 一致；改主題時不能動這組顏色。
- 所有顏色仍集中在 `pc-app/src/theme.css` 的 `:root`；`color-scheme: light`。
- 參數控制的「積木」只是外觀（CSS），元件與無障礙角色（slider、checkbox）不變，測試不依賴樣式。
- 這是「相似」而不是「一致」的承諾：MATRIXblock 之後再改版，Studio 不需要立刻跟上，有需要時再開新的 ADR。
