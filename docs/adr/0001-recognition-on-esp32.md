# 辨識在 ESP32-S3 上跑，電腦只負責設定

舊版是 ESP32 把 JPEG 傳給電腦，由電腦上的 OpenCV / pupil-apriltags 做辨識。新版把辨識搬到 ESP32-S3 上，電腦只在設定時連接，因為產品要像 HUSKYLENS 一樣，可以單獨裝在 MATRIX Mini R4 機器人上運作。

代價是功能受限於 S3 的算力：只跑傳統演算法（顏色、AprilTag）和小型 esp-dl 模型（人臉偵測），而且同一時間只跑一個功能。
