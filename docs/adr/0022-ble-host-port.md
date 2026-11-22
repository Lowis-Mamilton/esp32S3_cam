# Studio 的無線連線用 BLE，做成選用的藍牙版韌體

MXCam 裝在車上時，接著 USB 線很難一邊跑一邊看畫面、調參數。使用者要 Studio 能無線連到 MXCam（2026-11-22）。決定：用 BLE（Web Bluetooth），韌體多一個主機埠，封包和 USB 完全相同；但藍牙不進預設韌體，做成另外建置的藍牙版。

（編號 0012／0013 留給 M4、0020／0021 留給 M7，所以這份是 0022。）

## 決定

1. **BLE，不用 Wi-Fi。** Studio 掛在 GitHub Pages（HTTPS），瀏覽器不准 HTTPS 頁面連區網的 `ws://`，用 Wi-Fi 就得讓 MXCam 自己提供 Studio 網頁，而且電腦連上 MXCam 的熱點時會斷網。Web Bluetooth 在 HTTPS 可以直接用，瀏覽器也還是 Chrome／Edge。Wi-Fi 需要的內部 RAM 通常更多，而內部 RAM 正是這塊板子最缺的。
2. **選用的藍牙版韌體。** `firmware/sdkconfig.ble` 疊在 `sdkconfig.defaults` 上建置才有藍牙，預設韌體和 CI 不含。原因是餘裕：藍牙版映像 3,772,720 B，離 90% 自訂上限（3,774,873 B）只剩 2.2 KB；內部 RAM 最低剩 63 KB，底線是 60 KB。等 M8 把模型搬到模型分割區、再找出 10 KB 以上的內部 RAM，才考慮讓預設韌體也有藍牙。
3. **封包不變，BLE 只是另一條位元組流。** 一個 GATT 服務、兩個特徵（RX 寫入、TX 通知），規則在 `docs/protocol.md` §12。Studio 的協定層和 `mxcam_service` 都沒有改。
4. **`mxcam_link` 多一個主機埠，一次只服務一個主機。** 每個埠有自己的解碼器、RX task 和 tx mutex。請求的回應送回收到請求的那個埠；推送和延後回應送給「最後發出請求」的埠；BLE 斷線後回到 USB。`mxcam_service` 仍然不知道有幾種傳輸。
5. **控制器程式碼留在 IRAM。** 搬到 flash 執行（`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`）可以多省 16 KB 內部 RAM，但手勢辨識在傳輸時慢約 20%（和辨識搶 flash／PSRAM 匯流排）。
6. **連上後什麼都不要求，只用 1M PHY。** spike 一連上就要求 2M PHY、資料長度、連線間隔，結果 Windows 讀服務清單要 10–60 秒；2M PHY 在測試的電腦上會讓連線中斷。什麼都不要求時約 1 秒連上，Windows 自己協商出的設定有約 57 KB/s。
7. **藍牙的即時畫面最多 5 fps。** 5 fps 約 25 KB/s，人臉速度不變、手勢慢約 10%。10 fps 時 core 0 忙不過來，JPEG 壓縮被擠到 300 ms 以上。Studio 在藍牙上自動用 5 fps。
8. **第二個埠的 RX task 堆疊放 PSRAM**（省 4 KB 內部 RAM，讓最低剩餘回到 60 KB 以上）。這個 task 只解碼請求、送回應，不可以自己呼叫 flash 函式；SAVE 和 FACTORY_RESET 本來就在 store worker 做。
9. **不配對、不加密、沒有開關，名稱 `MXCam-XXXX`。** 第一版先求能用。韌體燒錄永遠走 USB。藍牙每次連線都要使用者按一下（瀏覽器的規定），不會自動重連。

## Considered Options

- **Wi-Fi（熱點＋MXCam 自己提供 Studio 網頁）**：速度綽綽有餘，任何瀏覽器（含 iPad）都能用。沒選的理由見決定 1；另外還要把 Studio 存進 flash、跟韌體一起更新。如果之後需要 10 fps 以上的無線畫面或 iOS，再回頭評估。
- **先做 M4 計畫的 service 改寫（`mxs_transport_t`、各傳輸自己的 outbox）再加 BLE**：是比較乾淨的結構，兩個主機同時連也不會互搶。沒選是因為 M7 正在改同一個 `mxcam_service`，而且藍牙和 USB 都是 Studio 在用，同一時間通常只有一個。M4 做那次改寫時把 BLE 當成一種傳輸別一起收進去。
- **藍牙直接進預設韌體**：使用者不用另外燒一版。沒選的理由見決定 2。

## Consequences

- USB 和藍牙同時各連一個 Studio 時會互搶：推送跟著最後發出請求的那一邊跑，延後回應（LEARN、SAVE）可能送到另一邊。不支援這種用法。
- 藍牙版韌體沒有餘裕再加東西；在它上面加功能前要先量映像大小和內部 RAM。
- 附近任何人都能用 Chrome 連上藍牙版的 MXCam 並改設定。教室使用前要加「關閉無線」設定或配對。
- 多台 MXCam 在同一間教室時，使用者只能靠名稱末四碼分辨；TFT 還沒有顯示名稱（SPEC §10）。
- 內部 RAM 的統計在這次工作中修正過（`docs/measurements/internal-ram.md`）；藍牙的量測過程在 `spike/ble` 分支的 `docs/measurements/ble-spike.md`。
- 沒量過：運作電流、其他電腦與手機的速度。

## 驗證

- 韌體：`python tools/ble/smoke.py --fn 3 --ttl COM7`（藍牙版韌體）。2026-11-22 實測：1 秒內連上，GET_INFO／SET_FUNCTION／GET_CONFIG／STREAM_START 都有回應（約 31 ms），5.0 fps、25 KB/s、JPEG 有效、PING 全部有回應、沒有亂碼；人臉 10.3 fps／47.6 ms，手勢 5.0 fps／159 ms；內部 RAM 最低 63,096 B。同一版韌體用 USB 串流的速度與預設韌體相同。
- 預設韌體（不含藍牙）：映像 3,548,240 B，沒有任何藍牙程式碼。
- Studio：`pc-app` 的測試用假的 Web Bluetooth 涵蓋連線、分段寫入、跨通知的封包、5 fps、斷線。實機用瀏覽器驗收還沒做。
