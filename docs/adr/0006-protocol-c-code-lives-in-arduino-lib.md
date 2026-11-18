# 協定 C 程式碼的正本放在 `arduino-lib/src/protocol/`

韌體和 Arduino 函式庫要共用同一份協定編解碼程式碼。Arduino 函式庫只會編譯自己資料夾裡的檔案，ESP-IDF 元件卻可以指向任何路徑，所以正本放在 `arduino-lib/src/protocol/`，韌體的 `firmware/components/mxcam_protocol/` 用 CMake 直接引用那裡的檔案。

這份程式碼必須是純 C，不能引用 Arduino 或 ESP-IDF 的標頭檔，這樣在電腦上也能編譯，CI 才能用 `docs/protocol-vectors.json` 測試它。

## Considered Options

- **正本放在最上層 `protocol/`，用腳本複製到 `arduino-lib/`**：目錄比較直觀，但會有兩份檔案，必須靠 CI 檢查有沒有不同步。
