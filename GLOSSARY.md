# MXCam

用 ESP32-S3 做的視覺感測器，功能對標 HUSKYLENS 2，搭配 MATRIX Mini R4 使用。

## 裝置與軟體

**MXCam**：
ESP32-S3 擴展板＋鏡頭＋TFT 組成的視覺感測器本體，負責取像、辨識與顯示。
_Avoid_: 鏡頭、相機板、smart cam

**MXCam Studio**：
在電腦瀏覽器上執行的設定軟體，用來看即時畫面、切換功能、學習和存設定。
_Avoid_: PC app、上位機、電腦端程式

**主機**：
向 MXCam 發出請求的一方，可能是 MATRIX Mini R4，也可能是 MXCam Studio。
_Avoid_: master、控制器

## 辨識

**功能**：
MXCam 同一時間執行的一種辨識，例如顏色、AprilTag、人臉偵測。
_Avoid_: 演算法、模式、app

**結果**：
某個功能在一張畫面上找到的一個目標，含 ID、位置與大小。
_Avoid_: block、物件、偵測框

**ID**：
結果的編號。顏色的 ID 由學習時分配；AprilTag 的 ID 就是 tag 本身的編號。
_Avoid_: 標籤、類別

**學習**：
在 MXCam Studio 上指定一個目標，讓 MXCam 記住它並分配一個 ID。第一版只有顏色需要學習。
_Avoid_: 訓練、learn

## 通訊

**能力查詢**：
主機連線後的第一個請求，MXCam 回報協定版本、韌體版本和支援的功能。
_Avoid_: knock、握手

**訂閱**：
只有 MXCam Studio 使用：請 MXCam 透過 USB 或藍牙持續主動推送畫面與結果。
_Avoid_: 串流模式、推播
