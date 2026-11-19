# 顏色辨識自己用純 C 寫，不用 esp-dl 的 color_detect

esp-dl 有 `espressif/color_detect` 元件，但它是 C++、依賴整個 esp-dl，沒辦法在電腦上單元測試，也看不到它怎麼處理色相繞回、低飽和度這些細節。我們自己寫 `mxcam_color`（純 C）：RGB565BE → 每 2×2 取樣成 160×120 的 HSV → 依學到的顏色做門檻 → 連通區域 → 面積過濾，座標再放大回 320×240。容許範圍的對應方式跟展示模式（`pc-app/src/device/demo/recognize.ts`）一致：色相 ±(tolerance × 0.6)°、飽和度和亮度 ±(0.25 + tolerance/200)、飽和度低於 0.15 不配對。

這樣展示模式和真板子的學習行為相同，演算法也能用 CI 的主機測試把關。
