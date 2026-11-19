# 需要畫面資料的指令延後回應；推送走非阻塞佇列

LEARN 必須在下一張畫面取色，但指令是在 USB 接收 task 裡處理的。我們讓 service 記下「待處理的 LEARN（含 seq）」，由畫面處理流程在下一張畫面完成後送出回應，而不是讓主機輪詢（BUSY＋重試）。主機只要把 LEARN 的逾時設 ≥ 1 秒。

同時把推送（FRAME、RESULTS）改成 `mxcam_link_push()`：放進一個小佇列由專門的傳送 task 送出，佇列滿了就丟掉最舊的 FRAME、RESULTS 只保留最新一筆。推送序號集中在 link 裡。這樣畫面處理流程永遠不會因為 USB 卡住而停頓（M1 的「不能拖慢螢幕」限制）。

## Considered Options

- **BUSY＋主機輪詢**：service 保持同步、簡單，但主機要寫重試邏輯，R4 端（M4）的函式庫也得跟著做，對學生程式不友善。
