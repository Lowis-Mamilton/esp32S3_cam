# 使用全新的版本化協定，不相容 HUSKYLENS / K230 / M-Vision

MXCam 用自己的 MXCam Protocol（`'M''X' | ver | cmd | seq | len | payload | crc16`），USB、UART、I2C 共用，主機連線後先做能力查詢。這延續 ESP32-S3 產品規劃書「不維持舊 HUSKYLENS／K230 相容協定」的決定。

## Considered Options

- **相容 HUSKYLENS 2 Protocol V2**：可以參考 DFRobot 的函式庫，但 MATRIX Mini R4 本來就沒有官方 HUSKYLENS 支援，相容的好處不大。它的檢查碼只是 8-bit 加總，座標也綁定 640×480。
- **模仿 MATRIX M-Vision Camera 的 UART 格式**：R4 現有的 `MiniR4SmartCamReader` 和 MATRIXblock 積木不用改就能用，但格式受限於 M-Vision，而且官方沒有公開規格。

## Consequences

- R4 的 Arduino 函式庫和之後的 MATRIXblock 積木都要自己寫。
- 協定層要寫成可以替換的模組，之後如果要加「M-Vision 相容模式」才不會牽動演算法和顯示的程式。
