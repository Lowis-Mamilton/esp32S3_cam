# MXCam Studio 改用類似 MATRIXblock 的淺色積木編輯器外觀

MXCam 是給 MATRIX Mini R4 用的，學生寫 R4 程式用的是 MATRIXblock（積木式編輯器）。原本 Studio 是深色、琥珀橘強調色的「儀器」風格，跟學生手上的另一個工具完全不像。決定：Studio 改成類似 MATRIXblock 的淺色外觀——深青綠上方列、左側彩色圓形分類列、點格工作區、藍色積木樣式的參數、像程式碼面板的程式分頁、像對話框的設定頁。只借用版面語彙和配色感覺，不使用 MATRIX 的 logo、名稱或圖片，產品名稱仍是「MXCam Studio」。細節記在 `docs/studio-ui.md` §9。

只做淺色，不做深色模式：MATRIXblock 只有淺色，維護兩套配色的成本不值得。即時畫面保留深色外框，因為攝影機畫面在深色背景上比較好判讀。

## Considered Options

- **維持深色儀器風格**：本身沒有問題，但跟學生的主要工具差異大，教室裡切換兩個工具時要重新適應。
- **淺色 + 深色兩套**：多一倍的顏色驗證工作（辨識框顏色、積木對比），目前沒有需求。
- **直接套用 MATRIX 的品牌元素（logo、名稱）**：不是 MATRIX 的產品，不能這樣做。

## Consequences

- 辨識框顏色 `--id-1`～`--id-8` 不受影響，仍與韌體 TFT 的 `k_id_colors` 一致；改主題時不能動這組顏色。
- 所有顏色仍集中在 `pc-app/src/theme.css` 的 `:root`；`color-scheme: light`。
- 參數控制的「積木」只是外觀（CSS），元件與無障礙角色（slider、checkbox）不變，測試不依賴樣式。
- 之後若 MATRIXblock 改版，Studio 不需要跟著改；這是「相似」而不是「一致」的承諾。
