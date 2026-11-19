# M5 網頁燒錄、PWA 離線、多語系 實作計畫

> **給執行的代理：** 必須搭配 superpowers:subagent-driven-development 逐項執行。步驟用勾選框（`- [ ]`）追蹤。
> 做畫面的任務（Task 4、5）實作者要先載入 `frontend-design:frontend-design` skill。
> 🧑‍🔧 步驟由主對話跟使用者一起做（需要實板與 USB 線）。直接在 `main` 上 commit；路徑 `C:\dev\esp32S3_cam`。
> M4（R4 連線）使用者決定暫緩（「先不用跟R4連」），M5 先做；`docs/superpowers/plans/2026-11-19-m4-r4-link.md` 是 M4 的備用計畫，**這個計畫不改它**。
> 設計問題已由使用者授權「直接採用建議」；下方「設計決定」列出每個決定與理由，執行時依各 Task 的步驟寫進 SPEC／ADR。
> ADR 編號：M4 計畫已預留 0012、0013，本計畫用 **0014**（執行時以 `docs/adr/` 的下一個空號為準）。

**目標：** 新電腦只要用 Chrome／Edge 打開 `https://lowis-mamilton.github.io/esp32S3_cam/`，就能把最新韌體燒進 MXCam（保留學過的資料）、自動連回並確認版本；Studio 可以安裝成 PWA、離線也能開啟和燒錄；兩種語言沒有漏字。

**架構：** Pages workflow 用 `espressif/idf:v5.5.6` 建置韌體（依韌體原始碼的 git tree 快取，只改網頁時不重建），把 bootloader／分割表／app 三個映像和 `firmware/manifest.json` 放進網站的 `firmware/`，Studio 同源讀取。燒錄由新的 `src/flash/` 模組負責：`FlashController` 狀態機透過注入的 `OpenLoader`（真實實作包 esptool-js）寫入，透過 `FlashHost`（包 `Device`）交出和收回序列埠，燒完用 GET_INFO 的 `fw_git` 對 manifest 驗證。PWA 用 `vite-plugin-pwa`（generateSW），韌體檔一起 precache，所以第一次造訪後離線也能燒。

**技術：** `esptool-js` 0.7.1（固定版本）、`spark-md5` 3.0.2（寫入後 MD5 比對）、`vite-plugin-pwa` 2.0.0（Workbox 7.4）、`@vite-pwa/assets-generator` 2.0.0（只用 npx 產生一次圖示）、Node 22.18+ 直接執行 `.ts` 腳本（type stripping）；既有 React 19／Vitest／GitHub Actions。

**規格：** `docs/SPEC.md` §6.2、§8（M5 完成條件）、§10；`docs/studio-ui.md` §1、§6；`docs/protocol.md` §4（GET_INFO 的 `fw_git`）；ADR-0004。

## 設計決定（執行時寫進 SPEC／ADR；理由附在後面）

1. **韌體從哪裡來（Task 1，ADR-0014）**：`pages.yml` 自己建置韌體，**不去拿 CI 的 artifact**。快取鍵 `mxcam-fw-v1-idf5.5.6-<firmware tree>-<protocol tree>`（`git rev-parse HEAD:firmware HEAD:arduino-lib/src/protocol`，韌體的全部輸入就是這兩個目錄）；命中就跳過 IDF 容器，只改 Studio 時部署仍約 1 分鐘（目前 45 秒；韌體建置約 4.5 分鐘只在韌體變更時發生）。網站結構：`/esp32S3_cam/firmware/{manifest.json, bootloader.bin, partition-table.bin, mxcam.bin}`。理由：同源讀取不需要 CORS（GitHub Release 的下載連結沒有 CORS header，瀏覽器拿不到）；跨 workflow 拿 artifact 要用 `workflow_run` 等 CI 跑完，CI 又有 `cancel-in-progress`，容易拿不到。**GitHub Release 掛韌體：延後**（目前沒有打 tag 的習慣；M4 的函式庫 release workflow 做好時再一起加），記到 SPEC §10。
2. **`fw_git` 改成「最後一個改到韌體來源的 commit」（Task 1，ADR-0014，protocol.md §4）**：`gen_git_header.cmake` 改用 `git log -1 -- firmware arduino-lib/src/protocol`，dirty 判斷也只看這兩個目錄。理由：原本用 HEAD，每次只改 Studio 的 commit 都會讓網站上的韌體 `fw_git` 變掉，板子明明是同一份韌體卻一直顯示「有新韌體」；快取過期重建時也會變。CI 的 checkout 要 `fetch-depth: 0`（淺 clone 時 `git log -- 路徑` 會回 HEAD）。
3. **manifest 格式（Task 1）**：`{"schema":1,"chip":"esp32s3","version":"0.1.0","fwGit":"4352356","idf":"v5.5.6","builtAt":"<ISO>","parts":[{"name":"bootloader","file":"bootloader.bin","offset":0,"size":…,"sha256":"…"},{"name":"partition-table","file":"partition-table.bin","offset":32768,…},{"name":"app","file":"mxcam.bin","offset":65536,…}]}`。`version`、`idf` 從 app 映像裡的 `esp_app_desc_t` 讀，並檢查 `version` 等於 `firmware/CMakeLists.txt` 的 `MXCAM_FW_VERSION_*`（GET_INFO 回報的就是這組，兩邊不一致就讓建置失敗）；`fwGit` 從 `build/esp-idf/main/generated/mxcam_git.h` 讀，結尾是 `+`（dirty）或 `unknown` 時拒絕發佈（本機 `--allow-dirty` 才允許 `+`）。
4. **燒什麼（Task 3）**：分開寫三個映像（0x0 bootloader、0x8000 分割表、0x10000 app），`flashMode/flashFreq/flashSize` 一律 `'keep'`（映像標頭已是 16MB／80m／DIO，bootloader 再切 QIO，不讓 esptool-js 改標頭）。**預設保留 NVS**（0x9000–0xF000，學到的顏色、名字、設定）。理由：merge 成單一映像會把中間的 NVS 填成 0xFF 而清掉資料。另有「完整清除」選項（`eraseAll: true`，清掉整顆 16 MB 再寫），在頁面內勾選後要再按一次「確定清除」才開始（不用瀏覽器的 confirm 對話框）。分割表從 M0 起沒變過；之後若改分割表，ADR-0014 要求 manifest 加欄位強制完整清除（現在不做，YAGNI）。
5. **「從檔案燒錄」（Task 2、4）**：保留在「進階」摺疊區，只接受 **app 映像**（寫到 0x10000），檢查標頭 magic `0xE9`、chip id `0x0009`（ESP32-S3）、`esp_app_desc_t` 的 project name 是 `mxcam`、大小 ≤ 4 MB。理由：方便開發者或老師拿到別人給的 `.bin`，也讓本機 build 不用部署就能測；只收 app 是因為 bootloader／分割表寫錯會讓板子開不了機。
6. **序列埠交接（Task 3）**：`MxcamConnection` 新增 `suspend()`／`resume()`；`Device` 新增 `releasePort()`／`reclaimPort()`。燒錄時：Studio 停串流、關埠、**停止自動連線**（包括 `serial` 的 `connect` 事件）→ esptool-js 用同一個 `SerialPort` 物件（Studio 沒連線時：唯一一個已授權的 MXCam 埠，否則 `requestPort` 過濾 303A:1001）→ `main('default_reset')`（PID 0x1001 時 esptool-js 自動用 USB-JTAG 的重設序列進下載模式）→ 寫入並 MD5 比對 → `after('custom_reset', undefined, 'D0|R1|W100|R0|W50')` 重開機 → 關埠 → `resume()`（15 秒內每 2 秒試一次自動連線）→ 等 GET_INFO（20 秒）比對 `fwGit`。ESPLoader 用 `baudrate: 115200, romBaudrate: 115200`：USB-Serial-JTAG 不管鮑率，兩者相同 esptool-js 就**不會重開序列埠**（重開會讓 USB-JTAG 板子重設、離開 stub）。不用內建的 `after('hard_reset')`：0.7.1 的 `HardReset` 對 USB-JTAG 只送 `RTS=0`，前面沒有 `RTS=1`，不一定會重開機。
7. **板子沒有可用的韌體（Task 3、4）**：空白晶片或當機的板子，USB-Serial-JTAG 仍由 ROM 提供，所以照樣可以燒（Studio 沒連線也能按「開始燒錄」）。連不上 bootloader 時頁面顯示步驟：「按住 BOOT → 按一下 RST → 放開 BOOT → 再按一次開始燒錄」；燒完 20 秒沒回應時提示「按一下 RST 或重新插拔 USB」。燒錄中所有導覽、連線、展示模式開關停用，並掛 `beforeunload`（離開頁面時由瀏覽器詢問；燒到一半關掉會讓板子沒有 app）。展示模式下燒錄按鈕停用並提示「關閉展示模式後才能燒錄」（避免兩個後端搶序列埠）。
8. **版本顯示（Task 4）**：韌體頁顯示「板子上：0.1.0（4352356）」與「網站提供：0.1.0（xxxxxxx）」。板子 `fw_git` ≠ manifest `fwGit` 時上方列出現「有新韌體」小標籤（點了到韌體頁）。順手處理 SPEC §10 M3 審查項目「分不出舊韌體與人臉初始化失敗」：板子 `fw_git` 等於網站版本卻沒有功能 3 → 側欄提示「人臉偵測在這塊板子上無法啟動」，否則照舊「需要更新韌體」（不需要改韌體版本號；M4 計畫的同一項到時標註已完成）。
9. **PWA（Task 5，ADR-0014 一節）**：`vite-plugin-pwa` 2.0.0 固定版本、`registerType: 'prompt'`；有新版時畫面底部橫幅「有新版本 [重新載入]」（燒錄中按鈕停用）；第一次可離線時顯示一次「已可離線使用」。**韌體檔用 precache**（不是 runtime cache）：`globPatterns` 包含 `bin`、`json`，`maximumFileSizeToCacheInBytes: 8 MiB`（app 約 2.13 MiB，預設 2 MiB 會讓 build 直接失敗）。理由：第一次造訪後離線就能燒（不必先燒過一次），而且 manifest 與 `.bin` 跟 app 同一個版本一起更新，不會出現新 manifest 配舊 `.bin`；代價是第一次造訪背景多下載約 2.2 MB。圖示：用現有 `public/favicon.svg` 以 `npx @vite-pwa/assets-generator@2.0.0 --preset minimal-2023 public/favicon.svg` 產生一次 PNG 並 commit（不把 sharp 放進建置）。開發模式不啟用 service worker；離線驗收用 `npm run build && npm run preview`。
10. **多語系（Task 6）**：key 集合相同的測試、語言記憶（`localStorage` `mxcam.lang`）已經有；新增：兩種語言的 `{變數}` 佔位符必須相同、`.tsx` 裡不能有寫死的中文（掃描測試）、人工檢查寫死的英文；esptool-js 的原始 log 只放在「詳細記錄」摺疊區，不翻譯。
11. **測試（各 Task）**：esptool-js 與序列埠全部用假物件（`OpenLoader`、`FlashHost` 注入）測狀態機；SHA-256 測試用 `// @vitest-environment node`（WebCrypto）；不加 Playwright。esptool-js 的轉接層（`esptoolLoader.ts`）很薄，只靠 🧑‍🔧 實機驗證。
12. **新電腦不用裝驅動**：Windows 10／11 與 macOS 對 USB-Serial-JTAG（CDC）都用內建驅動；寫進 SPEC §6.2。macOS 實機檢查是選做，沒測就在 `docs/measurements/m5.md` 記「未測」。

## 全域限制

- 板子 ESP32-S3 N16R8；OTG 孔 VID `0x303A` PID `0x1001`；映像位址 bootloader `0x0`、分割表 `0x8000`、app `0x10000`（factory 4 MB，到 `0x410000`）；NVS `0x9000`（24 KB）不可被預設燒錄碰到
- Vite `base` 維持 `/esp32S3_cam/`；韌體網址一律用 `${import.meta.env.BASE_URL}firmware/`
- 不跳 `alert`／`confirm`／`prompt`；確認一律在頁面內（例外：燒錄中的 `beforeunload`）
- 所有新文字都經過 i18n（`zh-TW`、`en`），既有 key 集合測試要維持綠燈
- `esptool-js` 只能用動態 `import()` 載入（不進首頁 bundle）；依賴版本寫死（不加 `^`）：`esptool-js` 0.7.1、`spark-md5` 3.0.2、`@types/spark-md5` 3.0.5、`vite-plugin-pwa` 2.0.0
- 既有 `MxcamConnection` 行為不變：新功能只能是新增的 `suspend()`／`resume()`，既有 `connection.test.ts` 全部維持綠燈
- `npm run lint && npm test && npm run build`（在 `pc-app/`）每個 Task 結束都要通過；動到韌體的 Task 還要 `idf.py build`（PowerShell 先啟用 `C:\Espressif` 的 ESP-IDF v5.5.6 環境；沒跑過不准說「已建置成功」）
- commit 只加明確路徑（不用 `git add -A`）；commit 訊息結尾加 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
- Task 7 之前不 push

## 審查重點

1. **燒錄中自動連線搶埠**：板子進下載模式或重開機時會重新出現在 USB 上，Studio 的 `connect` 事件或自動連線若在這時打開序列埠，esptool-js 會失敗或寫到一半斷掉；燒完又必須確實重新連上。→ Task 3 測試 `suspend blocks auto-connect until resume`、`resume retries auto-connect`
2. **網站上的韌體檔彼此不一致**：瀏覽器 HTTP 快取的舊 manifest 配新 `.bin`、或部署到一半。必須在寫入任何東西之前發現並顯示「網站上的韌體正在更新，請重新整理」。→ Task 2 測試 `hash mismatch rejects`、Task 3 測試 `load failure never opens the loader`
3. **一直顯示「有新韌體」**：只改 Studio 的 commit 讓 `fw_git` 變動、或 CI 建出 dirty 的韌體（`+`）。→ Task 1 測試 `rejects dirty and unknown git`，Task 1 的 `fw_git` 範圍檢查步驟，Task 7 部署後檢查
4. **燒到一半失敗或插錯板子**：USB 被拔、MD5 不符、或接的是同樣 VID/PID 的 ESP32-C3。必須不寫入（錯晶片）或明確顯示失敗與救援步驟、只收回序列埠一次、不會無限重連。→ Task 3 測試 `wrong chip never writes`、`write failure reclaims once`
5. **PWA 更新或離線時壞掉**：燒錄中按了「重新載入」、離線又沒有快取時韌體頁空白、韌體檔超過 precache 上限讓 build 失敗。→ Task 5 測試 `update banner disabled while flashing`、`pwa config precaches firmware`，Task 4 測試 `no firmware on site`

---

### Task 1：韌體身分與發佈到網站

**檔案：**
- 修改：`firmware/main/gen_git_header.cmake`、`docs/protocol.md`（§4 GET_INFO 表格 `fw_git` 那一列）
- 新增：`pc-app/src/flash/image.ts`、`pc-app/src/flash/image.test.ts`、`pc-app/src/flash/manifest.ts`、`pc-app/src/flash/manifest.test.ts`
- 新增：`pc-app/scripts/fw-manifest.ts`、`pc-app/scripts/fw-manifest.test.ts`
- 修改：`pc-app/package.json`（script `"fw:local": "node scripts/fw-manifest.ts ../firmware/build public/firmware --allow-dirty"`）、`pc-app/tsconfig.node.json`（`include` 加 `scripts/**/*.ts`、`src/flash/image.ts`、`src/flash/manifest.ts`）、`.gitignore`（加 `pc-app/public/firmware/`）
- 修改：`.github/workflows/pages.yml`、`.github/workflows/ci.yml`（firmware job 的 checkout 加 `fetch-depth: 0`）
- 新增：`docs/adr/0014-web-flashing-and-firmware-distribution.md`；修改：`docs/SPEC.md` §8

**介面（產出，Task 2–5 使用）：**
```ts
// image.ts — 不碰 DOM，腳本和瀏覽器共用
export const ESP32S3_CHIP_ID = 0x0009
export const APP_MAX_BYTES = 0x400000
export interface AppImageInfo { project: string; version: string; idf: string }
export type AppImageProblem = 'not-esp-image' | 'wrong-chip' | 'not-mxcam' | 'too-large'
export function inspectAppImage(data: Uint8Array):
  { ok: true; info: AppImageInfo } | { ok: false; problem: AppImageProblem }

// manifest.ts — 不碰 DOM
export type PartName = 'bootloader' | 'partition-table' | 'app'
export const PART_LAYOUT: Record<PartName, { offset: number; max: number }> // 0x0/0x8000, 0x8000/0x1000, 0x10000/0x400000
export interface FirmwarePart { name: PartName; file: string; offset: number; size: number; sha256: string }
export interface FirmwareManifest { schema: 1; chip: 'esp32s3'; version: string; fwGit: string; idf: string; builtAt: string; parts: FirmwarePart[] }
export class ManifestError extends Error { readonly field: string }
export function parseManifest(json: unknown): FirmwareManifest   // 依 PART_LAYOUT 排序後回傳

// scripts/fw-manifest.ts
export interface ManifestInputs { flasherArgs: unknown; gitHeader: string; cmakeLists: string;
  files: Record<PartName, Uint8Array>; builtAt: string }
export function buildManifest(inp: ManifestInputs, opts?: { allowDirty?: boolean }): FirmwareManifest
// CLI：node scripts/fw-manifest.ts <IDF build 目錄> <輸出目錄> [--allow-dirty]
```
- `inspectAppImage` 的位移（已用目前的 `mxcam.bin` 核對）：byte 0 = `0xE9`；byte 12–13（LE）= chip id；byte 32 起是 `esp_app_desc_t`（magic `0xABCD5432` LE）；version 在 48（32 bytes C 字串）、project name 在 80（32 bytes）、idf 版本在 144（32 bytes）。檢查順序：not-esp-image → wrong-chip → not-mxcam（magic 或 project 不符）→ too-large
- `parseManifest` 規則：`schema === 1`、`chip === 'esp32s3'`、`version` 符合 `^\d+\.\d+\.\d+$`、`fwGit` 符合 `^[0-9a-f]{6}([0-9a-f]|\+)$`、三個 part 各一個且 `offset` 等於 `PART_LAYOUT`、`0 < size <= max`、`sha256` 是 64 位小寫 hex、`file` 符合 `^[\w.-]+\.bin$`（不能有路徑）；錯誤丟 `ManifestError`（`field` 指出哪個欄位）
- `buildManifest`：檔案位置與位址取自 `flasher_args.json` 的 `bootloader`／`partition-table`／`app`（位址要等於 `PART_LAYOUT`，否則丟錯）；`version`／`idf` 取自 `inspectAppImage(files.app)`；`version` 必須等於 CMakeLists 的 `MXCAM_FW_VERSION_MAJOR/MINOR/PATCH`；`fwGit` 取 `#define MXCAM_FW_GIT "…"`，`unknown` 一律拒絕、`+` 結尾除非 `allowDirty` 否則拒絕；sha256 用 `node:crypto`；結果再經過 `parseManifest` 才回傳。CLI 把三個檔複製成 `bootloader.bin`、`partition-table.bin`、`mxcam.bin` 並寫 `manifest.json`（2 格縮排）

- [ ] **Step 1：寫失敗的測試**
  - `image.test.ts`：用程式組出 64+144 bytes 的假映像（helper `fakeApp({chip, project, version, idf, size})`）：正常 → `{ok:true, info:{project:'mxcam', version:'0.1.0', idf:'v5.5.6'}}`；byte 0 不是 `0xE9` → `not-esp-image`；chip id 5（C3）→ `wrong-chip`；project `blink` → `not-mxcam`；長度 `0x400001` → `too-large`
  - `manifest.test.ts`：合法 manifest 原樣通過且 parts 依 offset 排序；`fwGit: 'unknown'`、`app.offset: 0x20000`、`size: 0x400001`、`file: '../x.bin'`、少一個 part、`sha256` 大寫 → 各丟 `ManifestError`
  - `fw-manifest.test.ts`：`buildManifest` 用假輸入產生的 manifest 能通過 `parseManifest`、sha256 與 size 正確；`rejects dirty and unknown git`（`4352356` 可、`435235+` 預設拒絕而 `allowDirty` 可、`unknown` 兩種都拒絕）；CMakeLists 是 0.2.0 而映像是 0.1.0 → 丟錯；`flasher_args` 的 app 位址是 `0x20000` → 丟錯
- [ ] **Step 2：跑測試確認失敗**：`cd pc-app && npx vitest run src/flash scripts` → FAIL（模組不存在）
- [ ] **Step 3：實作** `image.ts`、`manifest.ts`、`scripts/fw-manifest.ts`。Node 22.18+ 直接跑 `.ts`（type stripping），所以這三個檔案：彼此 import 要寫 `.ts` 副檔名、只能用可抹除的語法（不用 enum、不用建構子參數屬性）、不碰 DOM；CLI 入口用 `process.argv[1] === fileURLToPath(import.meta.url)` 判斷
- [ ] **Step 4：跑測試確認通過**：同 Step 2 → PASS；再跑 `npm run lint && npm test && npm run build`
- [ ] **Step 5：`fw_git` 改成看韌體來源**：`gen_git_header.cmake` 的 hash 改用 `${GIT} log -1 --format=%h --abbrev=7 -- firmware arduino-lib/src/protocol`、dirty 判斷改成 `${GIT} status --porcelain --untracked-files=no -- firmware arduino-lib/src/protocol`（`REPO` 已是 repo 根目錄）；檔頭註解同步改。protocol.md §4 的 `fw_git` 說明改成「最後一個修改韌體來源（`firmware/`、`arduino-lib/src/protocol/`）的 commit 前 7 碼；這兩個目錄有未提交變更時是前 6 碼加 `+`」。驗證：`idf.py build` 通過，`build/esp-idf/main/generated/mxcam_git.h` 的 hash 等於 `git log -1 --format=%h -- firmware arduino-lib/src/protocol`（不是 HEAD）；再 `npm run fw:local` 產生 `pc-app/public/firmware/`，`manifest.json` 的 `version` 是 `0.1.0`、app `size` 等於 `mxcam.bin` 大小
- [ ] **Step 6：pages.yml 改成四個 job**（push 觸發的 `paths` 加 `firmware/**`、`arduino-lib/src/protocol/**`；所有快取動作都在 `ubuntu-latest` 上做，避免容器與 runner 壓縮格式不同造成 cache miss）：
  - `fw-key`（ubuntu）：checkout；算 key；`actions/cache/restore@v4` `lookup-only: true` 路徑 `fw-out`；輸出 `key`、`hit`
  - `fw-build`（`needs: fw-key`，`if: needs.fw-key.outputs.hit != 'true'`，container `espressif/idf:v5.5.6`）：checkout `fetch-depth: 0`；`idf.py build`；把 `flasher_args.json`、`bootloader/bootloader.bin`、`partition_table/partition-table.bin`、`mxcam.bin`、`esp-idf/main/generated/mxcam_git.h` 依原相對路徑複製到 `fw-out/`；`actions/upload-artifact@v4` 名稱 `fw-out`
  - `site`（`needs: [fw-key, fw-build]`，`if: ${{ !cancelled() && needs.fw-key.result == 'success' && needs.fw-build.result != 'failure' }}`）：checkout；fw-build 有跑 → `download-artifact` 到 `fw-out` 再 `actions/cache/save@v4`；沒跑 → `actions/cache/restore@v4`（`fail-on-cache-miss: true`）；`node scripts/fw-manifest.ts ../fw-out public/firmware`（**不加** `--allow-dirty`）；然後原本的 npm ci／lint／test／build、`upload-pages-artifact`
  - `deploy`：同原本（`needs: site`）
  - `ci.yml` 的 firmware job checkout 加 `fetch-depth: 0`
- [ ] **Step 7：記錄**：寫 `docs/adr/0014-web-flashing-and-firmware-distribution.md`（設計決定 1、2、3、4、6，以及 9 的 precache 理由；Considered Options：CI artifact／GitHub Release／merge 單一映像／runtime cache）；SPEC §8 表格上方加一行「M4 暫緩（2026-11-19 使用者決定），M5 先做」
- [ ] **Step 8：Commit**：`feat: publish firmware with a manifest on the Pages site`

### Task 2：下載與檢查韌體

**檔案：** 新增 `pc-app/src/flash/firmware.ts`、`pc-app/src/flash/firmware.test.ts`

**介面：**
- 使用：Task 1 的 `parseManifest`、`inspectAppImage`、`FirmwareManifest`、`PartName`
- 產出：
```ts
export interface FlashImage { name: PartName; offset: number; data: Uint8Array }
export type FirmwareBundle =
  | { source: 'site'; manifest: FirmwareManifest; images: FlashImage[] }
  | { source: 'file'; fileName: string; info: AppImageInfo; images: FlashImage[] } // 只有 app，offset 0x10000
export type FirmwareLoadCode = 'unavailable' | 'network' | 'size' | 'hash' | 'image'
export class FirmwareLoadError extends Error { readonly code: FirmwareLoadCode; readonly detail?: string }
export const firmwareBaseUrl = (): string                        // `${import.meta.env.BASE_URL}firmware/`
export function sha256Hex(data: Uint8Array): Promise<string>       // crypto.subtle
export function fetchManifest(base?: string, fetchFn?: typeof fetch): Promise<FirmwareManifest>
export function loadSiteFirmware(m: FirmwareManifest, base?: string, fetchFn?: typeof fetch): Promise<FirmwareBundle>
export function bundleFromFile(fileName: string, data: Uint8Array): FirmwareBundle
```
- 所有 fetch 用 `{ cache: 'no-cache' }`（有 service worker 時照樣由 precache 回應；沒有時避免 HTTP 快取的舊 manifest）
- `fetchManifest`：404 或離線 → `unavailable`；JSON／格式錯 → `unavailable`（`detail` 帶 `ManifestError.field`）
- `loadSiteFirmware`：逐檔下載；長度不符 → `size`；sha256 不符 → `hash`；app 再用 `inspectAppImage` 檢查且 `version` 要等於 manifest → 否則 `image`
- `bundleFromFile`：`inspectAppImage` 不通過 → `FirmwareLoadError('image', problem)`

- [ ] **Step 1：寫失敗的測試**（檔頭 `// @vitest-environment node`；`fetchFn` 用假的 `Map<url, Uint8Array | 404>`）：`sha256Hex(new TextEncoder().encode('abc'))` 等於 `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`；正常情況三個 image 依 offset 排序、內容正確；`hash mismatch rejects`（改一個 byte → `code === 'hash'`）；長度不符 → `size`；manifest 404 → `unavailable`；`fetch` 丟 `TypeError`（離線）→ manifest 是 `unavailable`、`.bin` 是 `network`；`bundleFromFile` 的 C3 映像 → `image`，正常映像 → 一個 image 在 `0x10000`
- [ ] **Step 2：跑測試確認失敗**：`npx vitest run src/flash/firmware.test.ts` → FAIL
- [ ] **Step 3：實作** `firmware.ts`
- [ ] **Step 4：跑測試確認通過**，再跑 `npm run lint && npm test && npm run build`
- [ ] **Step 5：Commit**：`feat(pc-app): download and verify firmware images`

### Task 3：燒錄流程與序列埠交接

**檔案：**
- 修改：`pc-app/src/serial/connection.ts`、`pc-app/src/serial/connection.test.ts`
- 修改：`pc-app/src/device/types.ts`、`RealDevice.ts`、`DemoDevice.ts`、`device.test.ts`／`realdevice.test.ts`（以及任何實作 `Device` 的測試假物件）
- 新增：`pc-app/src/flash/flasher.ts`、`pc-app/src/flash/flasher.test.ts`、`pc-app/src/flash/host.ts`、`pc-app/src/flash/esptoolLoader.ts`
- 修改：`pc-app/package.json`、`package-lock.json`（`esptool-js` 0.7.1、`spark-md5` 3.0.2、`@types/spark-md5` 3.0.5，版本不加 `^`）

**介面：**
- 使用：Task 2 的 `FlashImage`、`FirmwareBundle`、`FirmwareLoadError`
- 產出：
```ts
// connection.ts（新增）
suspend(): Promise<SerialPort | null>   // 停串流、關埠、停止自動連線（含 connect 事件與 connect()）；回傳原本使用的埠
resume(opts?: { retryForMs?: number }): void   // 恢復；立即 autoConnect，失敗則每 2 秒再試，直到連上、retryForMs（預設 15000）到期或再次 suspend

// device/types.ts 的 Device（新增）
releasePort(): Promise<SerialPort | null>   // RealDevice：視為使用者中斷（不顯示「連線中斷」），回傳 conn.suspend()；DemoDevice：null
reclaimPort(): void                         // RealDevice：conn.resume()；DemoDevice：什麼都不做

// flash/flasher.ts
export type FlashPhase = 'idle' | 'waiting-port' | 'loading' | 'connecting' | 'erasing' | 'writing'
  | 'restarting' | 'verifying' | 'done' | 'failed'
export type FlashFailure = 'load' | 'port-busy' | 'no-bootloader' | 'wrong-chip' | 'write'
  | 'verify-timeout' | 'version-mismatch'
export interface FlashState { phase: FlashPhase; progress: number /* 0–1，全部映像合計 */; failure?: FlashFailure;
  loadCode?: FirmwareLoadCode; detail?: string; log: string[]; elapsedMs?: number; deviceGit?: string; deviceVersion?: string }
export interface LoaderLike { readonly chipName: string
  write(images: FlashImage[], eraseAll: boolean, onProgress: (index: number, written: number, total: number) => void): Promise<void>
  resetToApp(): Promise<void>; close(): Promise<void> }
export type OpenLoader = (port: SerialPort, log: (line: string) => void) => Promise<LoaderLike>
export interface FlashHost { releasePort(): Promise<SerialPort | null>; reclaimPort(): void
  pickPort(): Promise<SerialPort | null>; waitForInfo(timeoutMs: number): Promise<DeviceInfo | null> }
export class FlashController {
  constructor(deps: { openLoader: OpenLoader; host: FlashHost; now?: () => number })
  state(): FlashState; subscribe(cb: () => void): () => void; get busy(): boolean
  start(load: () => Promise<FirmwareBundle>, opts: { eraseAll: boolean }): Promise<void>
  reset(): void   // 只在 done／failed 時回到 idle
}
export const VERIFY_TIMEOUT_MS = 20000

// flash/host.ts
export function deviceFlashHost(device: Device, serial: Serial): FlashHost
// pickPort：getPorts() 中 303A:1001 的埠剛好一個就用它，否則 requestPort({filters:[{usbVendorId:0x303a, usbProductId:0x1001}]})；使用者取消 → null
// waitForInfo：訂閱 device.snapshot()，status 'connected' 且有 info 就回傳，逾時回 null

// flash/esptoolLoader.ts
export const openEsptoolLoader: OpenLoader
```
- `start()` 的順序（`load` 放在取得序列埠之後，是因為 `requestPort` 必須在按鈕點擊後幾秒內呼叫）：
  1. `waiting-port`：`port = await host.releasePort() ?? await host.pickPort()`；`null`（取消）→ 回到 `idle`，並 `reclaimPort()`
  2. `loading`：`bundle = await load()`；`FirmwareLoadError` → `failed`/`load`（帶 `loadCode`）
  3. `connecting`：`openLoader(port, log)`；`DOMException` `NetworkError` 或訊息含 `Failed to open` → `port-busy`（別的程式佔用，例如 Arduino IDE 的序列埠監控）；其他錯誤 → `no-bootloader`；`chipName !== 'ESP32-S3'` → 先 `close()` 再 `failed`/`wrong-chip`，**不寫入**
  4. `eraseAll` 時先 `erasing`，第一次 `onProgress` 起變 `writing`；`progress` 依各映像位元組數加權（`written/total` 是壓縮後的比例）；寫入或 MD5 失敗 → `write`
  5. `restarting`：`resetToApp()`、`close()`
  6. `verifying`：`reclaimPort()`；`waitForInfo(VERIFY_TIMEOUT_MS)`；`null` → `verify-timeout`；`source === 'site'` 且 `info.firmwareGit !== manifest.fwGit` → `version-mismatch`；否則 `done`，記錄 `elapsedMs`（從 `connecting` 開始算）與裝置版本
  - 任何失敗都要 `close()` loader（若已開）並 `reclaimPort()`，**整個 `start()` 只呼叫一次 `reclaimPort()`**；`busy` 時再呼叫 `start()` 直接 reject
- `openEsptoolLoader`：動態 `import('esptool-js')` 與 `import('spark-md5')`；`new Transport(port, false)`；`new ESPLoader({ transport, baudrate: 115200, romBaudrate: 115200, terminal: { clean() {}, write: log, writeLine: log } })`；`await loader.main('default_reset')`（失敗時先 `transport.disconnect()` 再丟出）；`chipName = loader.chip.CHIP_NAME`；`write` 呼叫 `writeFlash({ fileArray: images.map(i => ({ data: i.data, address: i.offset })), flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', eraseAll, compress: true, reportProgress: onProgress, calculateMD5Hash: (img) => SparkMD5.ArrayBuffer.hash(img.slice().buffer) })`；`resetToApp` 呼叫 `loader.after('custom_reset', undefined, 'D0|R1|W100|R0|W50')`；`close` 呼叫 `transport.disconnect()`

- [ ] **Step 1：寫失敗的 connection／device 測試**（沿用既有的假 serial／port）：`realdevice.test.ts`：`releasePort()` 後 snapshot `lost === false`、`status 'disconnected'`；`DemoDevice.releasePort()` 回 `null`。`connection.test.ts`：`suspend blocks auto-connect until resume`（連線中 `suspend()` 回傳那個 port、狀態 `disconnected`、port 已關；之後發 `connect` 事件與呼叫 `autoConnect()`／`connect()` 都不開埠；`resume()` 後自動連上）；`resume retries auto-connect`（假 timer：第一次 open 失敗，2 秒後第二次成功；`retryForMs` 到期後不再試）；沒有連線時 `suspend()` 回 `null`
- [ ] **Step 2：寫失敗的 flasher 測試**（假 `OpenLoader`／`FlashHost`，假 timer）：
  - 正常：phase 依序 `waiting-port → loading → connecting → writing → restarting → verifying → done`；`write` 收到三個 image、`eraseAll === false`；`reclaimPort` 剛好一次、在 `resetToApp` 與 `close` 之後
  - `eraseAll: true`：出現 `erasing`，`write` 收到 `eraseAll === true`
  - `releasePort` 回 `null` → 呼叫 `pickPort`；`pickPort` 回 `null` → 回到 `idle`、`openLoader` 沒被呼叫
  - `load failure never opens the loader`：`load` 丟 `FirmwareLoadError('hash')` → `failed`、`failure 'load'`、`loadCode 'hash'`、`reclaimPort` 一次
  - `wrong chip never writes`：`chipName 'ESP32-C3'` → `failed`/`wrong-chip`、`write` 沒被呼叫、`close` 有被呼叫
  - `write failure reclaims once`：`write` 丟錯 → `failed`/`write`、`close` 一次、`reclaimPort` 一次
  - `openLoader` 丟 `NetworkError` → `port-busy`；丟一般錯 → `no-bootloader`
  - `waitForInfo` 回 `null` → `verify-timeout`；回不同 `firmwareGit` → `version-mismatch`；`source: 'file'` 時不比對 git
  - `progress`：兩個 image 大小 1000／3000，第二個寫到一半時 `progress === 0.625`
  - `busy` 期間再 `start()` → reject，且不影響進行中的流程
- [ ] **Step 3：跑測試確認失敗**：`npx vitest run src/serial src/flash/flasher.test.ts` → FAIL
- [ ] **Step 4：實作** `suspend`／`resume`、`Device.releasePort`／`reclaimPort`（所有 `Device` 實作都要補）、`flasher.ts`、`host.ts`、`esptoolLoader.ts`；安裝依賴
- [ ] **Step 5：跑測試確認通過**，再跑 `npm run lint && npm test && npm run build`；確認 `dist/assets` 裡 esptool-js 是獨立 chunk（首頁的 entry chunk 不含 `ESPLoader` 字串）
- [ ] **Step 6：Commit**：`feat(pc-app): flash controller with esptool-js and port handoff`

### Task 4：韌體頁、版本提示與燒錄中鎖定

**檔案：**
- 修改：`pc-app/src/ui/pages/FirmwarePage.tsx`、`pc-app/src/ui/pages/pages.test.tsx`、`pc-app/src/App.tsx`、`pc-app/src/app.test.tsx`、`pc-app/src/ui/layout/TopBar.tsx`、`Sidebar.tsx`、`layout.test.tsx`、`pc-app/src/theme.css`、`pc-app/src/i18n/zh-TW.ts`、`en.ts`
- 新增：`pc-app/src/flash/useFirmware.ts`

**介面：**
- 使用：Task 2 `fetchManifest`／`loadSiteFirmware`／`bundleFromFile`；Task 3 `FlashController`、`deviceFlashHost`、`openEsptoolLoader`
- 產出：
```ts
// useFirmware.ts
export function useSiteManifest(fetchFn?: typeof fetch): { manifest: FirmwareManifest | null; state: 'loading' | 'ready' | 'unavailable' }
export function useFlashState(c: FlashController): FlashState      // useSyncExternalStore
export const updateAvailable = (info: DeviceInfo | undefined, m: FirmwareManifest | null): boolean
  // info 與 m 都有，且 info.firmwareGit !== m.fwGit
// FirmwarePage props
{ snapshot: DeviceSnapshot; manifest: FirmwareManifest | null; manifestState: 'loading' | 'ready' | 'unavailable';
  controller: FlashController; demo: boolean }
// Sidebar 新 prop
faceInitFailed?: boolean   // 真板子、fw_git 等於網站版本、supported 沒有 3 → 功能 3 的提示改成 fnFaceInitFailed
```
- App：`FlashController` 用 `useMemo` 建一次（`openEsptoolLoader` + `deviceFlashHost(device, serial)`；device 換掉時重建）；`controller.busy` 時 Sidebar／TopBar 的導覽、連線、展示模式全部停用，並掛 `beforeunload`（`preventDefault()`），結束就移除；測試可注入 `openLoader`（`App` 新增選填 prop `openLoader?: OpenLoader`）
- 韌體頁版面（文字 key 名稱可自訂，下面是中文原文）：
  - 「板子上」：`0.1.0（4352356）` 或「未連線」；「網站提供」：`0.1.0（xxxxxxx）`、「讀取中…」或「這個網站沒有附韌體（離線且尚未快取）」
  - 主按鈕「開始燒錄」（網站版本不可用、展示模式、`busy` 時停用；展示模式提示「關閉展示模式後才能燒錄」）；說明「會保留學過的顏色和設定」
  - 勾選「完整清除（清掉學過的顏色、名字和設定）」後主按鈕變「清除並燒錄」，按下後同一區塊出現「確定清除」與「取消」，按「確定清除」才開始
  - 進度：階段文字＋進度條（`role="progressbar"`、`aria-valuenow` 0–100）；完成顯示「燒錄完成：0.1.0（xxxxxxx），花了 nn 秒」
  - 失敗訊息：`load`（`hash`／`size` →「網站上的韌體正在更新，請重新整理頁面再試」，其他 →「下載韌體失敗」）、`port-busy`、`no-bootloader`（附 BOOT／RST 四步驟）、`wrong-chip`（「這不是 ESP32-S3」）、`write`（「寫入失敗，板子可能沒有可用的韌體：請再燒一次」＋BOOT／RST 步驟）、`verify-timeout`（「按一下 RST 或重新插拔 USB」）、`version-mismatch`（顯示兩邊版本）；都有「再試一次」（`controller.reset()`）
  - 「詳細記錄」`<details>`：`state.log` 原文（等寬字）
  - 「進階：從檔案燒錄」`<details>`：選 `.bin` → `bundleFromFile`；問題顯示對應訊息（`not-esp-image`、`wrong-chip`、`not-mxcam`、`too-large`）；通過後顯示檔名與檔內版本，按「燒錄這個檔案」
- TopBar：真板子連線中且 `updateAvailable` 時顯示「有新韌體」按鈕（點了切到韌體頁）

- [ ] **Step 1：寫失敗的測試**（`pages.test.tsx`、`layout.test.tsx`、`app.test.tsx`；用假 `FlashController` 依賴與假 fetch）：
  - 韌體頁顯示板子與網站兩個版本；`no firmware on site`（manifest `unavailable`）→ 顯示「沒有附韌體」且「開始燒錄」停用、頁面其他部分正常
  - 勾「完整清除」後要再按「確定清除」才呼叫 `start(..., {eraseAll: true})`；按「取消」不呼叫
  - 展示模式：「開始燒錄」停用並顯示提示
  - 假 loader 跑完：顯示完成文字與秒數；`version-mismatch` 顯示兩個版本；`no-bootloader` 顯示 BOOT／RST 步驟
  - 燒錄中：側欄、連線按鈕、展示模式開關停用；`window` 上有 `beforeunload` listener，完成後移除
  - 進階檔案：C3 映像顯示「不是 ESP32-S3 的韌體」，正常映像顯示版本
  - `updateAvailable`：git 不同 → TopBar 有「有新韌體」，點了顯示韌體頁；git 相同 → 沒有
  - Sidebar：`faceInitFailed` 時功能 3 的提示是「人臉偵測在這塊板子上無法啟動」，否則「需要更新韌體」
  - 刪除舊的「即將推出」測試
- [ ] **Step 2：跑測試確認失敗**：`npx vitest run src/ui src/app.test.tsx` → FAIL
- [ ] **Step 3：實作**（先載入 `frontend-design:frontend-design`；樣式沿用 `theme.css` 變數，進度條用強調色）；移除不再使用的 `comingSoon`、`fwNoFile` 等 key（兩份字典一起）
- [ ] **Step 4：跑測試確認通過**，再跑 `npm run lint && npm test && npm run build`
- [ ] **Step 5：Commit**：`feat(pc-app): firmware page with web flashing and update hint`

### Task 5：PWA（安裝、離線、更新提示）

**檔案：**
- 修改：`pc-app/package.json`、`package-lock.json`（`vite-plugin-pwa` 2.0.0）、`pc-app/vite.config.ts`、`pc-app/vite.config.test.ts`、`pc-app/index.html`、`pc-app/tsconfig.app.json`（`types` 加 `vite-plugin-pwa/react`）
- 新增：`pc-app/public/pwa-64x64.png`、`pwa-192x192.png`、`pwa-512x512.png`、`maskable-icon-512x512.png`、`apple-touch-icon-180x180.png`、`favicon.ico`（由 assets-generator 產生）
- 新增：`pc-app/src/pwa/useSwUpdate.ts`、`pc-app/src/pwa/registerSW.stub.ts`、`pc-app/src/pwa/UpdateBanner.tsx`、`pc-app/src/pwa/pwa.test.tsx`
- 修改：`pc-app/src/App.tsx`、`i18n/zh-TW.ts`、`en.ts`、`theme.css`

**介面：**
```ts
// useSwUpdate.ts — 唯一 import 'virtual:pwa-register/react' 的檔案
export function useSwUpdate(): { needRefresh: boolean; offlineReady: boolean; update(): void; dismissOffline(): void }
// UpdateBanner.tsx
export default function UpdateBanner(props: { busy: boolean }): JSX.Element | null
```
- `vite.config.ts`：`VitePWA({ registerType: 'prompt', injectRegister: false, includeAssets: ['favicon.svg', 'favicon.ico', 'apple-touch-icon-180x180.png'], manifest: { name: 'MXCam Studio', short_name: 'MXCam', description: 'MXCam 視覺感測器的設定與燒錄工具', lang: 'zh-TW', display: 'standalone', start_url: '/esp32S3_cam/', scope: '/esp32S3_cam/', theme_color: '#131211', background_color: '#131211', icons: [64、192、512 的 any，512 的 maskable] }, workbox: { globPatterns: ['**/*.{js,css,html,svg,png,ico,webmanifest,json,bin}'], maximumFileSizeToCacheInBytes: 8 * 1024 * 1024, cleanupOutdatedCaches: true } })`；`test.alias` 把 `virtual:pwa-register/react` 指到 `src/pwa/registerSW.stub.ts`（回傳兩個 `false` 狀態與空的 `updateServiceWorker`），單元測試不碰 service worker
- `index.html`：加 `<link rel="apple-touch-icon" href="%BASE_URL%apple-touch-icon-180x180.png">`、`<meta name="theme-color" content="#131211">`
- 橫幅：`needRefresh` →「有新版本 [重新載入]」（`busy` 時按鈕停用並顯示「燒錄完成後再重新載入」）；`offlineReady` →「已可離線使用」，4 秒後自動消失；都不是對話框

- [ ] **Step 1：產生圖示**：`cd pc-app && npx @vite-pwa/assets-generator@2.0.0 --preset minimal-2023 public/favicon.svg`；確認產生的 6 個檔案，打開 512 與 maskable 看一眼（maskable 的圖案要在安全區內）
- [ ] **Step 2：寫失敗的測試**：
  - `vite.config.test.ts` `pwa config precaches firmware`：PWA 選項抽成 `vite.config.ts` 的 `export const pwaOptions`（`VitePWA(pwaOptions)`），測試直接斷言 `registerType === 'prompt'`、`manifest.start_url === '/esp32S3_cam/'`、`manifest.scope === '/esp32S3_cam/'`、`globPatterns[0]` 含 `bin` 與 `json`、`maximumFileSizeToCacheInBytes >= 4 * 1024 * 1024`
  - `pwa.test.tsx`（`vi.mock('./useSwUpdate')`）：`needRefresh` 時顯示「有新版本」，按下呼叫 `update()`；`update banner disabled while flashing`（`busy` 時按鈕 disabled）；`offlineReady` 的提示 4 秒後消失；英文介面顯示英文
- [ ] **Step 3：跑測試確認失敗**：`npx vitest run vite.config.test.ts src/pwa` → FAIL
- [ ] **Step 4：實作**；App 在頁面底部放 `<UpdateBanner busy={controller.busy} />`
- [ ] **Step 5：跑測試確認通過**，再跑 `npm run lint && npm test && npm run build`；檢查 `dist/sw.js` 存在、`dist/manifest.webmanifest` 的 `start_url` 正確；有 `public/firmware/`（`npm run fw:local`）時，`dist/sw.js` 的 precache 清單包含 `firmware/mxcam.bin` 與 `firmware/manifest.json`，build 沒有 `maximumFileSizeToCacheInBytes` 錯誤
- [ ] **Step 6：本機離線檢查**：`npm run build && npm run preview`，Chrome 開 `http://localhost:4173/esp32S3_cam/`；DevTools → Application 看到 service worker 已啟用、Cache Storage 有 `mxcam.bin`；Network 切 Offline 後重新整理，頁面正常、韌體頁顯示網站版本；網址列出現安裝圖示
- [ ] **Step 7：Commit**：`feat(pc-app): installable PWA with offline firmware`

### Task 6：多語系收尾

**檔案：** 修改 `pc-app/src/i18n/i18n.test.ts`、`zh-TW.ts`、`en.ts`；新增 `pc-app/src/i18n/hardcoded.test.ts`；依稽核結果修改 `pc-app/src/**/*.tsx`

- [ ] **Step 1：寫失敗的測試**：
  - `i18n.test.ts` `placeholders match in both languages`：每個 key 的 `{name}` 集合在兩份字典相同
  - `hardcoded.test.ts`（`// @vitest-environment node`，用 `node:fs` 掃 `src/**/*.tsx`，排除 `*.test.tsx` 與 `src/i18n/`）：`no CJK text outside i18n`——去掉註解後不能有 `/[\u3400-\u9fff\uff00-\uffef]/`；失敗訊息列出檔名與行號
- [ ] **Step 2：跑測試**：`npx vitest run src/i18n` → 若已經通過，在暫存檔裡放一行中文確認測試真的會失敗，再刪掉
- [ ] **Step 3：人工稽核寫死的英文**：`rg -n ">[A-Za-z][^<{]*<|(title|aria-label|placeholder|alt)=\"[A-Za-z]" pc-app/src --glob '*.tsx' --glob '!*.test.tsx'`；`MXCam Studio`、單位（fps、KB/s）、程式碼範例可以保留，其他改成 `t()`；切到 English 走一遍每一頁（含韌體頁各種失敗訊息、PWA 橫幅），切回繁中再走一遍
- [ ] **Step 4：修正**；`npm run lint && npm test && npm run build`
- [ ] **Step 5：Commit**：`test(pc-app): guard i18n placeholders and hard-coded text`

### Task 7：🧑‍🔧 部署、實機驗收與量測

**檔案：** 新增 `docs/measurements/m5.md`；修改 `docs/SPEC.md`（§6.2、§8、§10）、`docs/studio-ui.md`（§1、§6，加一節「韌體更新頁」）、`CLAUDE.md`（pc-app 的 `npm run fw:local`、Pages 會建韌體）、`README.md`（「燒錄：打開網址 → 韌體 → 開始燒錄」）

- [ ] **Step 1：push**（使用者同意後）；確認 CI 全綠；`pages` workflow 第一次會跑 `fw-build`；打開 `https://lowis-mamilton.github.io/esp32S3_cam/firmware/manifest.json`，`fwGit` 等於 `git log -1 --format=%h -- firmware arduino-lib/src/protocol` 且沒有 `+`
- [ ] **Step 2：快取檢查**：`gh workflow run pages.yml` 重跑一次 → `fw-key` 命中、`fw-build` 被跳過、整體約 1 分鐘、manifest 的 `fwGit` 不變
- [ ] **Step 3：🧑‍🔧 從部署的網站燒錄**（Windows，Chrome）：先學一個顏色並存到板子 → 韌體頁按「開始燒錄」→ 不跳選埠視窗（沿用連線中的埠）→ 進度到 100% → 自動連回、顯示「燒錄完成」與版本 → 學過的顏色還在。記錄燒錄秒數、從按下到連回的總秒數
- [ ] **Step 4：🧑‍🔧 其他情境**：
  - 「完整清除」：要按兩次才開始；燒完後學習資料清空、設定回預設；記錄時間
  - 板子沒有可用韌體：用「進階：從檔案燒錄」選一個故意截斷的檔（應被拒絕）；再用 TTL 孔 `idf.py -p COMy erase-flash` 或「完整清除」中途拔線製造空白板，照 BOOT／RST 步驟救回
  - Arduino IDE 序列埠監控佔住 OTG 埠時按燒錄 → 顯示「被其他程式佔用」
  - 本機：`npm run fw:local && npm run dev`，從 `http://localhost:5173/esp32S3_cam/` 燒錄本機 build
  - 離線：網站裝成 App（或一般分頁）造訪過一次後，DevTools Offline（或關 Wi-Fi）重新整理 → 介面正常、韌體頁有網站版本 → 燒錄成功
  - 更新提示：部署新版後開著舊頁面 → 出現「有新版本」，按下後載入新版
  - macOS（選做）：有 Mac 就重複 Step 3，沒有就記「未測」
- [ ] **Step 5：如果 USB-JTAG 重開機序列不可靠**（Step 3 卡在 `verify-timeout` 而按 RST 就好）：記錄現象，改 `resetToApp` 的序列（例如加 `D0|R1|W200|R0|W200`）或改用 `after('hard_reset', true)` 再試，把結果寫進 m5.md
- [ ] **Step 6：寫 `docs/measurements/m5.md`**（格式比照 m3.md）：日期、韌體 git、Studio git、esptool-js 版本；表格：下載韌體時間、燒錄時間（保留 NVS／完整清除）、總時間、app 大小；驗收清單逐條結果；已知限制
- [ ] **Step 7：更新文件**：SPEC §6.2「內建網頁燒錄」與「離線可用」改成已完成並指向 ADR-0014、加「Windows／macOS 不需要另外裝驅動」；§8 M5 標記完成；§10 新增／勾選：GitHub Release 掛韌體（延後）、「Studio 分不出舊韌體與人臉初始化失敗」標註 M5 已處理、macOS 驗證結果；studio-ui.md §1 刪掉「即將推出」那句、新增韌體更新頁一節（版本對照、保留資料、完整清除、從檔案燒錄、救援步驟）；CLAUDE.md、README.md
- [ ] **Step 8：Commit 並 push**：`docs: M5 measurements and acceptance`；確認 CI 與 Pages 都綠
