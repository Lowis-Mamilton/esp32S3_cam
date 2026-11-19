# 辨識用真實影像，TFT 顯示鏡像

使用者拿板子時鏡頭和螢幕都朝向自己（M1 實機確認時，使用者覺得「真實影像」左右反了，認可的是像照鏡子的鏡像畫面）。M1 為此改了 LCD 的 mirror，結果送給辨識的鏡頭原始畫面變成鏡像，AprilTag 鏡像後無法解碼（M2 實機：OpenCV 在原始畫面上找不到 tag，左右翻轉後讀到 ID 12）。決定：鏡頭感測器開啟水平鏡像（`set_hmirror(1)`），讓畫面緩衝區、辨識、FRAME 推送的 JPEG 和協定座標（ResultRecord、LEARN 區域）一律是**真實影像**；TFT 用 LCD 的 X 鏡像（`esp_lcd_panel_mirror(true, false)`）顯示鏡像畫面。疊加的框畫在真實影像座標上，跟著畫面一起被鏡像，所以不用改；文字和圖示則以「螢幕上看到的位置」為準，用 `mxo_draw_text_mirrored` / `mxo_mirror_x` 反向畫進畫面，在 TFT 上才是正的。

## Consequences

- 2026-11-19 實機驗證（784715a 之後的修正）：OpenCV `DICT_APRILTAG_36h11` 在 FRAME 推送的原始 JPEG 上直接讀到 ID 12（翻轉後反而讀不到），板子自己的 RESULTS 同時回報 tag 12。
- 主機（MXCam Studio）若要跟 TFT 一樣顯示鏡像，要自己把畫面和框左右翻轉（框中心 x' = 320 − x），框選學習時把螢幕座標換回真實影像座標；協定本身不變（protocol.md §7 註明座標是真實影像）。
- TFT 上的所有文字都必須用鏡像畫法；新增的疊加文字若直接用 `mxo_draw_text` 會在螢幕上左右顛倒。
- 鏡頭若改成另一種拿法（螢幕背對使用者），只要把 LCD mirror 和文字畫法改回來，辨識與協定不受影響。
