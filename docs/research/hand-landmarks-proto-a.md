# 原型 A：手部 21 點關鍵點模型在電腦上的轉換、量化與評估

> 日期：2026-11-19 ・ 狀態：原型結果（還沒定案；定案時寫進 `docs/SPEC.md`／新 ADR）
> 前一份：`docs/research/hand-landmarks.md`（研究建議先做原型，並訂了 4 個門檻）。這一份只做 **Phase A（電腦端）**：轉換、量化、在電腦上模擬量化後的推論、規則手勢。**沒有上機**，速度與記憶體（門檻 1、4）留給 Phase B。
> 數字全部是本原型在電腦上量的（ESP-PPQ 模擬 ESP32-S3 的量化推論），除非另外註明。

## 1. 結論

| 項目 | 結果 |
|---|---|
| TFLite → ONNX | **成功**。`tflite2onnx` 0.4.1 轉出的 ONNX 與原始 `.tflite` 在 120 張裁切上輸出一致（關鍵點最大差 0.0002 px／224，分數差 < 2e-6）。運算子只有 Conv 47、Clip（ReLU6）32、Add 10、MaxPool 1、ReduceMean 1、Gemm 3、Sigmoid 2，ESP-PPQ 全部吃得下。 |
| 全 int8（研究的方案 a） | **不行**：與 float 的平均點誤差 **11.8 %** 裁切邊長（轉正裁切也有 8.8 %），規則手勢在轉正裁切上只對 15/30。 |
| 研究建議的混精度（第一層 Conv＋FC 頭 int16，方案 b） | **也不行**：10.8 %（w8a16 版 11.8 %），幾乎沒改善。 |
| 原因 | 不是頭、也不是輸入，而是 **16 層 depthwise conv**：同一層各通道的權重範圍差 **18–80 倍**（BN 摺進權重的結果），ESP32-S3 只有 per-tensor、2 的次方 scale，小通道幾乎被量化成 0。ESP-PPQ 的逐層誤差分析排前三名全是 depthwise 層；跨層等化（equalization）只找得到 5 對可等化的層（ReLU6 擋住），沒有幫助；改 `mse` 校正也沒有幫助。 |
| 有效的做法 | **depthwise 全部改 int16**（其餘 30 層一般 Conv 仍 int8）：轉正裁切誤差 **1.39 %**；再加第一層＋FC 頭 int16（`int8+dw16+heads16`）：**1.30 %（最大 2.38 %）**，規則手勢 **30/30**。int16 的層只佔 20 % 的 MACs（29.8 M／145.6 M）。 |
| 不旋轉（軸對齊）裁切 | 研究擔心的事是真的：手是側的（讚、打電話、橫著指）時，float 模型在軸對齊裁切上就會亂掉（點誤差達 hand size 的 30 %，手勢錯）。**量化後更怕旋轉**。解法：先用一次推論的結果把裁切轉正再跑一次（MediaPipe 追蹤式）；量化版還要「存在分數 < 0.9 時多試 ±90° 再轉正」（`adaptive+r`），量化版在 30 隻手上 **29–30/30**。 |
| `.espdl` 大小（不含測試資料，就是要放 flash 的大小） | int8 **1,149,056 B**；建議版 `int8+dw16+heads16` **1,348,288 B**；全 int16 2,161,520 B。比研究估的 1.0–1.1 MB 大一些，1.5 MB 的 `hand_lmk` 分割區仍放得下。 |
| **門檻 2**（量化誤差 ≤ 3 % 裁切邊長） | 全 int8、研究的混精度：**不通過**。`int8+dw16+heads16`：在實際拿來輸出關鍵點的轉正裁切上 **通過**（平均 1.30 %、最大 2.38 %）；在軸對齊裁切上平均 3.9–4.7 %（不過 float 本身在那種裁切上就已偏離真值 4–5 %，而流程只拿它來估方向）。 |
| **門檻 3**（規則手勢不輸分類器） | **電腦端有條件通過**：量化版＋轉正裁切 30/30、從軸對齊框開始的完整流程（`adaptive+r`）29–30/30；分類器實機在 M8a 測試畫面是 7/7，那 7 隻手規則版也全對。但樣本只有 15 隻手（含鏡像 30），都是乾淨背景的範例圖，**一定要在 Phase B 用實機＋真手再驗**。 |
| Phase B 放哪一版 | **主：`hand_landmark_lite_int8+dw16+heads16.espdl`**；備：`int8+auto8`（只有 8 層 int16，15 % MACs，轉正誤差 1.39 %）與全 `int16`（誤差 ≈ 0，但最大最慢）。見第 7 節。 |
| 最大的新風險 | ① 速度：int16 的 depthwise＋42 個 int8↔int16 轉換（RequantizeLinear）＋32 個獨立的 Clip（ReLU6 沒有被併進 Conv）會比研究估的 ~250 ms 慢，**門檻 1（≤ 300 ms）有風險**；② 第一次抓到手（沒有上一張可追蹤）平均要 **2.6 次**推論（`adaptive+r`），所以一定要做追蹤，否則一張要 ~0.8 s。 |

## 2. 環境與版本

工作目錄在 repo 外：`C:/Users/yjz11/.claude/jobs/d99bd9d8/tmp/handlm-proto/`（venv、資料、模型、輸出都在這裡；`.espdl` 留給 Phase B）。

| 套件 | 版本 | 備註 |
|---|---|---|
| Python | 3.11.15（uv 管理） | ESP-PPQ 要求 ≥ 3.11、< 3.13 |
| **esp-ppq** | **1.3.14**（2026-10-30） | esp-dl 3.3.15（commit `512ed0b`，2026-11-06）發佈時最新的版本；esp-dl 文件只寫 `pip install esp-ppq`，沒有指定對應版本。匯出的檔頭是 `EDL2`，和 esp-dl 3.3.15 的 `hand_detect`／`hand_gesture_recognition` 模型相同，fbs_loader 支援。 |
| torch | 2.15.0+cpu | `uv pip install "esp-ppq[cpu]" --torch-backend=cpu` 帶進來 |
| onnx／onnxruntime／onnxsim | 1.19.1／1.32.0／0.8.1 | onnx、onnxsim 版本由 esp-ppq 限定 |
| tflite2onnx／tflite | 0.4.1／2.18.0 | 不需要 TensorFlow（tf2onnx 要整包 TF，沒用） |
| ai-edge-litert | 2.3.0 | 跑原始 `.tflite` 當基準（Windows 有 wheel） |
| numpy／pillow | 2.4.6／13.0.0 | **沒有用 OpenCV**：opencv-python 4.12 會把 numpy 降回 < 2.3（和 esp-ppq 裝好的 2.4.6 衝突），而且下載很慢；裁切／仿射改用 Pillow，結果一樣 |

`tools/handlm/requirements.txt` 有固定版本。

## 3. 重現步驟

腳本在 `tools/handlm/`（只有小腳本，沒有模型與 venv）。以下 `W` 是 repo 外的工作目錄，`T=tools/handlm`。所有讀下載資料的 Python 都用 `-I`；ESP-PPQ 會印 Unicode 進度條，Windows 要加 `-X utf8`（`-I` 會忽略 `PYTHONIOENCODING`）。

```bash
# 1. 環境
uv venv --python 3.11 $W/.venv
uv pip install --python $W/.venv/Scripts/python.exe "esp-ppq[cpu]==1.3.14" --torch-backend=cpu
uv pip install --python $W/.venv/Scripts/python.exe -r $T/requirements.txt
PY="$W/.venv/Scripts/python.exe -I -X utf8"

# 2. 資料（各放自己的資料夾）
#   模型：https://storage.googleapis.com/mediapipe-assets/hand_landmark_lite.tflite
#   esp-dl 範例圖（MIT）存成 $W/gesture-imgs/ 下這兩個檔名：
#     examples/hand_gesture_recognition/gestures.png          -> examples_hand_gesture_recognition_gestures.png
#     examples/hand_gesture_recognition/gestures/gesture.jpg  -> examples_hand_gesture_recognition_gestures_gesture.jpg
#   MediaPipe 測試資料（Apache-2.0）存到 $W/dl/mp_testdata/：
for f in thumb_up pointing_up pointing_up_rotated victory fist; do
  curl -sO https://storage.googleapis.com/mediapipe-assets/$f.jpg
  curl -sO https://storage.googleapis.com/mediapipe-assets/${f}_landmarks.pbtxt
done

# 3. 轉換（約 10 秒）
$PY $T/convert.py hand_landmark_lite.tflite $W/models/hand_landmark_lite.onnx

# 4. 樣本、評估裁切、校正集（約 30 秒）
$PY $T/build_sets.py $W/models/hand_landmark_lite.onnx $W/gesture-imgs $W/dl/mp_testdata $W/data

# 5. 量化＋匯出＋模擬推論（int8 類每個約 1 分鐘，auto8 約 3 分鐘，int16 約 4 分鐘，CPU）
for v in int8 mixed16 mixed_a16 int8+eq int8+mse int8+auto4 int8+auto8 int8+dw16 int8+dw16+heads16 int16; do
  $PY $T/quantize.py $W/models/hand_landmark_lite.onnx $W/data/calib_all.npy $W/data/eval.npz $W/quant $v
done
$PY $T/quantize.py $W/models/hand_landmark_lite.onnx $W/data/calib_all.npy $W/data/eval.npz $W/quant int8+dw16+heads16 0 notest
#   校正集泛化檢查：用不含 9 張手勢格子圖的校正集
$PY $T/quantize.py $W/models/hand_landmark_lite.onnx $W/data/calib_holdout.npy $W/data/eval.npz $W/quant_holdout int8+dw16+heads16
cp $W/quant_holdout/int8+dw16+heads16.preds.npz $W/quant/holdout.preds.npz

# 6. 評估
$PY $T/evaluate.py hand_landmark_lite.tflite $W/models/hand_landmark_lite.onnx $W/data $W/quant int8 ... int16 holdout
$PY $T/crop_strategies.py $W/models/hand_landmark_lite.onnx $W/gesture-imgs $W/dl/mp_testdata 1.5 0.9 [$W/quant/<variant>.native]
$PY $T/viz.py $W/models/hand_landmark_lite.onnx $W/data out.jpg rot   # 疊點的對照圖（目視檢查）
```

| 腳本 | 做什麼 |
|---|---|
| `convert.py` | tflite2onnx 轉換、拿掉世界座標頭（`[1,63]`）、輸出改名 `landmarks`／`presence`／`handedness`、onnxsim。輸入本來就是 NCHW `[1,3,224,224]`，值域 0–1 RGB。 |
| `hlm_common.py` | 裁切（仿射矩陣，連續座標）、MediaPipe 的「關鍵點 → 轉正 ROI」（`hand_landmark_landmarks_to_roi`：12 個點、手腕→中指 MCP 定方向、放大 2.0、往上移 0.1）、三種推論器（ONNX、TFLite、ESP-PPQ `.native`）、樣本清單 |
| `build_sets.py` | 樣本手、評估裁切（每隻手 4 種）、校正集（`calib_all`、`calib_holdout`） |
| `quantize.py` | ESP-PPQ `espdl_quantize_onnx`（target `esp32s3`）＋變體設定＋匯出 `.espdl`＋在每張評估裁切上跑 ESP-PPQ 的量化模擬（`TorchExecutor`），存 `.preds.npz` 與 `.native` |
| `evaluate.py` | TFLite vs ONNX、float vs 真值、量化 vs float、presence／handedness、規則手勢 |
| `crop_strategies.py` | 從軸對齊框開始的各種裁切策略（單次、轉正、試 ±90°…），可換成量化模型 |
| `gesture_rules.py` | 21 點 → 手勢 ID 1–9 的規則 |

## 4. 資料

| 資料 | 來源／授權 | 用途 |
|---|---|---|
| 9 種手勢格子圖（one … dislike 各一隻手） | esp-dl `examples/hand_gesture_recognition/gestures.png`（MIT），M8a 測試畫面也是用它 | 評估（有標籤）＋校正 |
| 人比 1 | esp-dl `examples/hand_gesture_recognition/gestures/gesture.jpg`（MIT） | 評估＋校正 |
| thumb_up、pointing_up、pointing_up_rotated、victory、fist | MediaPipe 測試資料（`storage.googleapis.com/mediapipe-assets`，MediaPipe repo 用的測試檔，Apache-2.0）。**附官方 21 點真值**（`*_landmarks.pbtxt`） | 評估（真值）＋校正 |

- 每隻手再做左右鏡像 → **30 隻手**（15 原圖＋15 鏡像）。沒有另外下載其他資料集，校正集全部是上面這些圖的合成增強；只在本機用，不散布。
- **參考點**：MediaPipe 的 5 張用官方真值；另外 10 張沒有真值，用 float 模型在轉正裁切上反覆 4 次收斂的結果當「準真值」（pseudo）。
- **評估裁切**（每隻手 4 張，共 120 張）：`rot`＝MediaPipe 式轉正裁切（等於追蹤模式拿到的裁切）；`aa13`／`aa15`／`aa20`＝軸對齊正方形，邊長＝「類偵測框」長邊 ×1.3／1.5／2.0。類偵測框＝21 點外框四邊各加 10 %（模擬 `hand_detect` 的框；`hand_detect` 本身 PC 上沒得跑）。超出原圖的部分補黑。
- **校正集**：`calib_all` 300 張＝30 隻手 × 10 次隨機增強（一半軸對齊框 ×1.15–1.9、一半轉正框 ×0.85–1.25；再隨機轉 ±30°、平移 σ=7 %、亮度 ×0.55–1.35、色偏 ±10 %、對比 ×0.7–1.3、20 % 模糊、雜訊）；`calib_holdout` 300 張＝只用 12 隻**非格子圖**的手 ×25（拿來看校正集是否過度貼合評估圖）。

## 5. 結果

### 5.1 TFLite vs ONNX（float）

120 張評估裁切：關鍵點最大差 **0.000198 px**（224 px 為單位），presence 最大差 1.6e-6，handedness 2.1e-6。tflite2onnx 對 fp16 權重印出的「float16 not supported/tested」警告不影響結果。

### 5.2 float 模型的準度與軸對齊裁切的影響

| 裁切 | 手數 | 參考 | 平均點誤差（% 裁切邊長） | 平均點誤差（% 手的大小） | presence 最小／中位數 |
|---|---|---|---|---|---|
| rot（轉正） | 10 | 真值 | **1.19** | 2.06 | 0.988／0.995 |
| aa13 | 10 | 真值 | 4.54 | 7.08 | 0.821／0.992 |
| aa15 | 10 | 真值 | 4.82 | 8.68 | 0.689／0.990 |
| aa20 | 10 | 真值 | 4.75 | 11.39 | 0.335／0.948 |
| aa13 | 20 | 準真值 | 3.77 | 5.88 | 0.774／0.993 |
| aa15 | 20 | 準真值 | 4.06 | 7.31 | 0.531／0.984 |
| aa20 | 20 | 準真值 | 6.34 | 15.20 | 0.064／0.851 |

- 直立的手在軸對齊裁切上沒問題；**手是側的**（讚、打電話、pointing_up_rotated 那種橫著指）就會整組點跑掉，presence 也會掉（0.5–0.8）。框放越大越糟（×2.0 時 30 隻手只剩 18 隻手勢對）。
- 「手的大小」＝參考點外框的長邊。

### 5.3 量化變體（ESP-PPQ 模擬 vs float，% 裁切邊長）

| 變體 | 說明 | 全部 120 張平均 | rot 平均／最大 | aa13／aa15／aa20 | rot 的 presence 最大差 | `.espdl`（含測試資料） | 規則手勢 rot |
|---|---|---|---|---|---|---|---|
| int8 | 全部 w8a8 | 11.82 | 8.84／20.4 | 13.9／12.0／12.5 | 0.53 | 1,299,920 | 15/30 |
| int8+eq | ＋跨層等化 | 12.34 | 8.86／16.5 | 14.8／13.0／12.7 | 0.63 | 1,301,344 | 14/30 |
| int8+mse | 啟動值校正改 mse | 11.71 | 9.12／19.9 | 13.7／11.9／12.2 | 0.45 | 1,299,744 | 13/30 |
| mixed_a16 | 第一層＋3 個 FC 頭 w8a16 | 11.81 | 8.98／21.0 | 13.8／12.1／12.4 | 0.50 | 1,454,624 | 15/30 |
| mixed16 | 第一層＋3 個 FC 頭 w16a16（研究的方案 b） | 10.84 | 8.01／19.9 | 13.0／11.1／11.2 | 0.50 | 1,500,240 | 18/30 |
| int8+auto4 | 逐層誤差最大的 4 層 int16 | 5.71 | 3.25／22.5 | 6.5／6.3／6.9 | 0.13 | 1,320,960 | 27/30 |
| int8+auto8 | 逐層誤差最大的 8 層 int16（6 層 depthwise） | 3.06 | 1.39／2.64 | 3.6／3.3／4.0 | 0.011 | 1,493,104 | 30/30 |
| int8+dw16 | 16 層 depthwise int16 | 3.62 | 1.39／2.66 | 4.3／4.2／4.6 | 0.030 | 1,450,800 | 30/30 |
| **int8+dw16+heads16** | 16 層 depthwise＋第一層＋FC 頭 int16 | 3.44 | **1.30／2.38** | 4.7／3.9／3.9 | 0.038 | 1,649,920 | **30/30** |
| 同上（holdout 校正集） | 校正集不含格子圖 | 3.48 | 1.37／2.40 | 4.6／3.9／4.1 | 0.038 | — | 30/30 |
| int16 | 全部 w16a16 | 0.04 | 0.01／0.02 | 0.03／0.04／0.08 | 0.000 | 2,460,208 | 30/30 |

- 「含測試資料」的 `.espdl` 內嵌了一張輸入（第一張評估裁切 grid_one/rot）與對應輸出，Phase B 可用 esp-dl 的 `model->test()` 對 PC 結果；輸入是 int8 時佔 150 KB、int16 時 300 KB。**放 flash 的版本**用 `notest` 匯出：int8 1,149,056 B、int8+dw16 1,299,728 B、**int8+dw16+heads16 1,348,288 B**、int16 2,161,520 B。
- 每個變體各層用的精度在 `quant/<variant>.layers.txt`；auto 選的層在 `quant/<variant>.auto16.txt`。
- 16 層 depthwise 的權重，各通道最大值的「最大／中位數」比是 4.8–79.9（中位數約 27）——per-tensor int8 下，中位數通道只剩 2–3 bit 有效精度，這就是全 int8 失敗的原因。
- holdout 校正集的結果和 `calib_all` 幾乎一樣（1.37 vs 1.30 %），校正集沒有過度貼合評估圖。
- 匯出的圖：47 Conv、32 Clip（ReLU6 **沒有**被併進 Conv，以 LUT 另外跑）、10 Add、3 Gemm、2 Sigmoid、1 MaxPool、1 ReduceMean，`int8+dw16+heads16` 另有 **42 個 RequantizeLinear**（int8↔int16 邊界）。ReLU6 換成 ReLU 不可行：32 層都有 > 6 的值（最多 3.3 % 的元素、最大 68.7）。
- 量化後關鍵點輸出：int8 頭的 exponent 是 1（步距 2 px＝0.9 % 裁切邊長）；FC 頭 int16 時 exponent −7（步距 1/128 px）。

### 5.4 從軸對齊框開始的完整流程（`crop_strategies.py`，30 隻手，誤差以「手的大小」為單位）

| 策略 | 推論次數（平均） | float：誤差平均／最大、手勢 | int8+dw16+heads16（框 ×1.5） | int8+dw16+heads16（框 ×1.3） | int8+auto8（×1.5） |
|---|---|---|---|---|---|
| single：軸對齊跑一次 | 1 | 7.8／32.4 %，25/30 | 11.2／37.0 %，20/30 | 11.1／43.4 %，20/30 | 9.6／29.9 %，21/30 |
| refine：再用結果轉正跑一次 | 2 | 2.3／7.1 %，**30/30** | 8.9／31.9 %，21/30 | 7.8／35.7 %，23/30 | 7.8／29.6 %，21/30 |
| rot3：0°／±90° 三個取 presence 最高 | 3 | 2.4／13.2 %，29/30 | 4.1／14.5 %，29/30 | 3.2／13.4 %，29/30 | 3.6／12.3 %，27/30 |
| adaptive：presence < 0.9 才試 ±90° | 1.4–1.7 | 3.8／17.5 %，29/30 | 4.4／14.7 %，29/30 | 3.2／13.4 %，29/30 | 3.9／12.3 %，27/30 |
| **adaptive+r**：adaptive 再轉正跑一次 | 2.3–2.7 | **1.7／7.1 %，30/30** | **3.1／6.8 %，29/30** | **2.7／6.6 %，30/30** | 3.0／6.7 %，28/30 |

（float 框 ×1.3 時：single 26/30、refine 30/30、adaptive+r 1.7 %、30/30、平均 2.33 次。）

- float 模型只要「跑一次 → 轉正 → 再跑一次」就夠；**量化模型在側手的軸對齊裁切上估出的方向太差，轉正一次救不回來**，需要先用 presence 判斷要不要試 ±90°。
- 進入追蹤後（上一張的 21 點 → 這一張的轉正裁切）每張只要 **1 次**推論，對應 5.3 的 `rot` 欄：誤差 1.30 %、手勢 30/30。
- 失敗的那隻是 grid_ok 鏡像（OK 被判成 4）：拇指尖與食指尖的距離剛好在門檻邊緣。

### 5.5 presence／handedness

| 模型 | rot 的 presence 最小值 | rot 上左右手與 float 相同 | 鏡像對的左右手相反 |
|---|---|---|---|
| float | 0.981 | 30/30 | 15/15 |
| int8 | 0.453 | 15/30（等於亂猜） | 8/15 |
| mixed16 | 0.500 | 14/30 | 9/15 |
| int8+dw16 | 0.961 | 30/30 | 15/15 |
| int8+dw16+heads16 | 0.953 | 29/30 | 14/15 |
| int8+auto8 | 0.984 | 30/30 | 15/15 |

- 全 int8 的 presence 中位數掉到 0.67、左右手是隨機的；dw16 類恢復正常。
- 軸對齊的側手裁切上，量化版 presence 會掉到 0.01（float 0.06–0.7），正好拿來當「要不要試 ±90°／重跑偵測」的訊號（門檻 0.9 好用）。
- handedness 的定義（MediaPipe 假設輸入是自拍鏡像）與 MXCam「辨識用真實影像、顯示才鏡像」（ADR-0010）的對應，Phase B 要用真手定清楚；這裡只驗證「鏡像後會反過來」與「量化後不變」。

### 5.6 規則手勢（`gesture_rules.py`）

規則（手掌大小＝手腕到中指 MCP）：四指「伸直」＝指尖到手腕距離 > 1.12 × PIP 到手腕距離，且 PIP 關節角 > 120°；拇指「伸直」＝指尖到食指 MCP > 0.55 手掌、IP 關節角 > 130°、且指尖比 IP 離小指 MCP 更遠；OK＝拇指尖到食指尖 < 0.40 手掌且中／無名／小指至少 2 根伸直；只有拇指伸直時看拇指（MCP→指尖）的上下方向 > 0.45 手掌 → 讚／倒讚；拇指＋小指 → 打電話；其他依伸直的根數 → 1–5；沒有 → 0（例如拳頭）。門檻是依手部比例先訂好的，**沒有對這組圖調過**。

| 模型 | rot（追蹤） | aa13 | aa15 | aa20 |
|---|---|---|---|---|
| 參考點本身 | 30/30 | — | — | — |
| float | 30/30 | 26/30 | 25/30 | 18/30 |
| int8 | 15/30 | 14/30 | 10/30 | 7/30 |
| mixed16 | 18/30 | 14/30 | 13/30 | 9/30 |
| int8+dw16+heads16 | **30/30** | 20/30 | 20/30 | 14/30 |
| int8+auto8 | 30/30 | 22/30 | 21/30 | 17/30 |

完整流程（5.4 的 `adaptive+r`）：int8+dw16+heads16 29/30（×1.5）、30/30（×1.3）。分類器（現在的功能 8）在 M8a 實機的 batch1 測試畫面是 7/7（one×2、two、three、four、five、like）；規則版在同樣這幾隻手全對。OK／打電話／倒讚分類器沒有實機數字。

## 6. 門檻判定

| 門檻 | 判定 | 依據 |
|---|---|---|
| 1. 實機 ≤ 300 ms／手、追蹤 ≥ 3 fps | **未測（Phase B）** | 有風險，見第 7 節 |
| 2. 量化誤差 ≤ 3 % 裁切邊長 | 全 int8、研究的混精度：**不通過**（8–12 %）。**`int8+dw16+heads16`：通過**（轉正裁切平均 1.30 %、最大 2.38 %；holdout 校正 1.37 %） | 5.3 |
| 3. 規則手勢 ≥ 分類器 | **電腦端有條件通過**（30/30 追蹤裁切；完整流程 29–30/30；分類器實機 7/7 的那幾隻手全對）。樣本少且乾淨，**需實機＋真手複驗** | 5.6 |
| 4. 載入後 PSRAM 剩 ≥ 1 MB | **未測（Phase B）** | — |

## 7. Phase B 建議

1. **模型**：`hand_landmark_lite_int8+dw16+heads16.espdl`（含測試資料，先用 `model->test()` 確認實機輸出與 PC 模擬一致），確認後改用 `_notest` 版（1,348,288 B）放 `storage` 分割區。路徑：`C:/Users/yjz11/.claude/jobs/d99bd9d8/tmp/handlm-proto/quant/`。
2. **一起帶上機量速度的備案**：`int8+dw16_notest`（1,299,728 B，少 4 層 int16）、`int8+auto8`（只有 8 層 int16，15 % MACs）、全 `int16_notest`（2,161,520 B，誤差 ≈ 0，最慢）。順序：先量主版本；> 300 ms 就量 auto8；全 int16 只當精度上限／「慢速模式」備案。
3. **速度要特別量的**：int16 depthwise 的實際耗時、42 個 RequantizeLinear、32 個獨立 Clip（LUT）各佔多少（esp-dl 有逐層 profile）。若 Clip／Requantize 佔比大，再考慮讓 ESP-PPQ 把相鄰層對齊同精度以減少轉換。
4. **流程**：偵測框 → 軸對齊正方形（框長邊 ×1.3 比 ×1.5 好）→ 推論 → presence < 0.9 時試 ±90° → 用最好的結果算 MediaPipe 轉正 ROI → 再推論一次（輸出這一次的 21 點）→ 下一張直接用上一張的 21 點算 ROI（追蹤，每張 1 次），presence 低於門檻才回到偵測器。
5. **前處理**：輸入 RGB、0–1，`.espdl` 裡是 NHWC `1x224x224x3`（輸入 exponent：int8 版 −7；`int8+dw16+heads16` 第一層 int16，輸入是 INT16、−15；全 int16 −14）；轉正裁切要用仿射（esp-dl 的 image 前處理有仿射，人臉對齊在用）。
6. **驗收**：Phase B 用 OV3660 拍真手（9 種手勢、正面／側面、左右手），同時跑現在的分類器比較，補門檻 3 的實機數字。

## 8. 限制

- 評估只有 15 隻不同的手（＋鏡像），10 隻沒有真值（準真值來自同一個 float 模型，會讓「轉正」策略對準真值看起來偏好），背景乾淨、光線均勻，沒有 OV3660 實拍。
- `hand_detect` 的框是用「21 點外框＋10 %」模擬的，真實的框大小／位置偏移會不同（第一次推論的品質會受影響，但轉正後會收斂）。
- 量化數字是 ESP-PPQ 的 PC 模擬；esp-dl 文件說運算子與 PPQ 對齊，但 int16 部分 PPQ 用的模擬精度與實機可能有小差異，Phase B 用內嵌測試資料確認。
- 規則門檻沒有調，但也只在這組圖上驗過；側面、手指互相遮擋的情況沒測。

## 9. 檔案

- 腳本：`tools/handlm/`（`convert.py`、`build_sets.py`、`quantize.py`、`evaluate.py`、`crop_strategies.py`、`gesture_rules.py`、`hlm_common.py`、`viz.py`、`requirements.txt`）
- 本機輸出（不進 repo）：`C:/Users/yjz11/.claude/jobs/d99bd9d8/tmp/handlm-proto/`
  - `models/hand_landmark_lite.onnx`（3.96 MB float）
  - `quant/hand_landmark_lite_<variant>[ _notest].espdl`、`.info`、`.json`、`<variant>.layers.txt`、`<variant>.native`、`<variant>.preds.npz`、`final_eval.txt`、`results.json`
  - `data/eval.npz`、`calib_all.npy`、`calib_holdout.npy`、`viz_rot.jpg`、`viz_aa15.jpg`
