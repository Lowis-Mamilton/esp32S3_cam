# 電腦軟體做成 Web Serial PWA

MXCam Studio 要同時支援 Windows 和 macOS，也要能在網頁上直接燒錄韌體（esptool-js）。所以做成 TypeScript + Vite + React 的 Web Serial PWA，發佈到 GitHub Pages，安裝之後離線也能用。

## Considered Options

- **Python + PySide6 打包成 exe**：跟舊版一樣用 Python，但要分別打包 Windows 和 macOS 版，macOS 還有簽章問題，燒錄韌體也要另外包 esptool。
- **Electron**：可以跨平台，但安裝檔很大，比 PWA 多了打包和發佈的負擔。

## Consequences

- 只能用 Chrome 或 Edge；macOS 的 Safari 不支援 Web Serial。
- 用 GitHub Pages 免費發佈的前提是 repo 公開，所以 repo 採 MIT 授權公開。
