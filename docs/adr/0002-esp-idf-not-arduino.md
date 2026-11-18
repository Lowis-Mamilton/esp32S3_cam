# 韌體用 ESP-IDF，不用 Arduino

在 ESP32 上跑模型需要 esp-dl / esp-who，它們是 ESP-IDF 的元件，在 Arduino 底下很難整合，版本也常常落後。所以韌體改用 ESP-IDF v5.x，放棄舊版的 Arduino IDE、`.ino` 和 TFT_eSPI，螢幕改用 `esp_lcd` 驅動。

另一個好處是螢幕和鏡頭的設定都能放進 repo，不像 TFT_eSPI 要去改函式庫自己的 `User_Setup.h`。

注意：給 MATRIX Mini R4 用的**函式庫**仍然是 Arduino 函式庫，因為 R4 本身是 Arduino UNO R4 WiFi 板型。只有 MXCam 自己的韌體不用 Arduino。
