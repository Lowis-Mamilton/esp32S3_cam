# 協定成長方式：各功能自己的參數、功能清單上限 32、LEARN 可指定 ID

功能會從 3 個長到 12 個（SPEC §3 擴充功能表），每個新功能都有自己的參數，有些之後要「學到指定的 ID」。協定的原則是「只新增、不改既有欄位」（protocol.md 開頭），所以在 M6 一次把成長的方式定下來，細節在 `docs/protocol.md` §9。

1. **GET_INFO 的功能清單上限從 8 調到 32**：公開常數 `MXP_INFO_MAX_FUNCS 32`，`mxp_info_t.function_ids[32]`，pack／unpack 超過 32 就失敗。功能 ID 會到 12，舊的上限 8 會讓整包 GET_INFO 解析失敗。Arduino 函式庫還沒發佈（`MXCam.h` 還是空殼），現在改沒有相容問題；R4 只多 24 bytes RAM。
2. **各功能自己的參數用新指令 `GET_FUNC_CONFIG (0x44)`／`SET_FUNC_CONFIG (0x45)`**：payload 是 `function_id u8, layout_version u8, body`，每個功能一個固定版面、自己的版本號。SET 整包取代，回應是夾限後的整包；不支援的功能 → `BAD_PARAM`，版本不符 → `BAD_PARAM`，長度不符 → `BAD_LENGTH`。改到值設 `dirty`、SAVE 才寫 flash、FACTORY_RESET 回預設，和 Config v1 一樣。功能 1–3 的參數**留在 Config v1**，不搬。
3. **不新增能力位元**：GET_INFO 的功能清單就是能力。列出 4／5 的韌體就支援它們的 func config；之後版面變動靠 `layout_version`。少一個欄位，主機邏輯不變（Studio 本來就靠功能清單決定哪些功能可選）。
4. **LEARN 可以多帶 `id (u16)`**：9 bytes 是原本的語意，11 bytes 是「學到指定 ID」。格式現在定很便宜，而且向後相容；M6 的韌體對 11 bytes 一律回 `BAD_PARAM`，M9（人臉辨識、自學習分類）才真的用。

## Considered Options

- **Config v2（把所有功能的參數放進一個更大的全域結構）**：每加一個功能就要改一次全域版面，主機要整包重寫，而且違反 Config v1「不改既有欄位」的規則。
- **TLV（tag-length-value）參數清單**：最有彈性，但 R4 上的 C 程式碼要多一層解析和測試面；固定版面＋各自版本號一樣好夾限、好測，彈性目前用不到。
- **GET_INFO 加能力位元（bitmask）**：功能清單已經能表達「支援哪些功能」，多一個欄位只會多一個要保持一致的地方。

## Consequences

- C 正本（`arduino-lib/src/protocol/`）和 TS（`pc-app/src/protocol/`）都提供線條 v1、物體追蹤 v1 的編解碼；夾限只在韌體端（C 的 unpack）做，TS 不夾限。
- 新功能要參數時：在 protocol.md 加一個版面表（`function_id`、`layout_version 1`），C 加 pack／unpack＋夾限，TS 加型別；不用動 Config v1。
- 版面要改時升 `layout_version`；舊主機送舊版本會收到 `BAD_PARAM`，要先 GET 看韌體用哪一版。
- 線條的 ResultRecord 帶 14 bytes 的 `extra`；主機遇到不認得的 `extra` 長度（0–18）必須略過而不是整包失敗，這樣新韌體配上舊主機也不會壞。
- 舊韌體不認得 `0x44`／`0x45`（回 `UNKNOWN_CMD`）；主機先看 GET_INFO 的功能清單，沒有 4／5 就不送。
