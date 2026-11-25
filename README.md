# 译意 Yiyi

使用 **C++/WinRT + WinUI 3** 开发的 Windows 桌面 AI 翻译词典。当前版本为 **1.0.0**。

![译意翻译工作台](docs/screenshots/translate-light.png)

## 下载与运行

- **精简包** `dist/Yiyi-1.0.0-winui-windows-x64.zip` 约 68 MiB。安装 Windows App Runtime 2.4 x64 后，解压并运行 `Yiyi-WinUI/Yiyi.WinUI.exe`。 [Windows App Runtime 下载](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads/)
- **自带运行时包** `dist/Yiyi-1.0.0-winui-windows-x64-self-contained.zip` 约 89 MiB，随附 Windows App Runtime 2.4 x64。遇到 `0x80670016` 时，完整解压后运行 `Yiyi-WinUI-SelfContained/Yiyi.WinUI.exe`。

两个包均包含本地 OCR 模型、PDFium、qpdf 及两套 PDF 字体。

首次启动点击「偏好设置 → 添加 AI 服务」。支持 Ollama、LM Studio、OpenAI Chat Completions、OpenAI Responses、Anthropic 和 Gemini。本地服务需事先启动，云端服务使用自己的 API Key。

默认用户数据位于 `%LOCALAPPDATA%\Yiyi\Yiyi`。设置写入 `settings.json`，历史和收藏写入 `history.sqlite`，文档任务存入 `office-tasks/` 和 `pdf-tasks/`。首次启动可读取原有 `settings.ini`，API Key 由当前 Windows 用户的 DPAPI 保护。更新前可从托盘退出正在运行的实例。

## 功能

- 上下布局的翻译工作台，粘贴原文即开始智能翻译。支持简明词条和流式完整词条、多服务配置、可选思考等级，以及可搜索的历史和生词本。
- Windows 托盘、可配置的全局快捷键与划词浮标。截图支持本地 RapidOCR 和 AI 识图，校对后可复制、填入工作台或翻译。
- TXT 分段翻译可停止、续译及导出已完成部分。DOCX / PPTX 支持原格式导出、段落校对、单段重译和进度恢复。DOCX 可用本机 Word 或 LibreOffice 在应用内整页对照。旧版 DOC / PPT 可先转换为 DOCX / PPTX。
- PDF 支持多栏、表格、扫描区域识别、原文与译文同步预览、暂停续接和结构校正。导出保留图片、链接和批注，嵌入可搜索的译文。无法安全替换的区域保留原文并列入检查清单。

文档和界面预览见 [使用说明](docs/使用说明.md)、[Word / PPT 翻译](docs/Office翻译-0.11.0.md)、[PDF 版式与导出](docs/PDF版式与导出.md)、[深色工作台](docs/screenshots/translate-dark.png)和[验证记录](docs/验证记录.md)。

## 构建与测试

开发环境需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载、Windows SDK 10.0.26100.0、CMake 3.24 或更新版本，以及 Python 3。Visual Studio 2026 对应的生成器要求 CMake 4.2 或更新版本。构建脚本会准备工作进程所需的 OCR 和 PDF 依赖。可用 `-PythonPath` 指定用于准备 PDF 字体的解释器。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -SmokeTest -Package
```

Release 程序位于 `build/winui/Release/Yiyi.WinUI.exe`，可用以下参数在独立配置中启动：

```powershell
build/winui/Release/Yiyi.WinUI.exe --data-dir artifacts/manual-test --no-desktop
build/winui/Release/Yiyi.WinUI.exe --data-dir artifacts/pdf-test --no-desktop --pdf "C:\path\sample.pdf"
```

`--no-desktop` 关闭托盘、快捷键和划词监测。TXT、DOCX 和 PPTX 可用 `--document` 打开。使用 `-SmokeTest` 会运行启动检查。

构建自带运行时包时，添加 `-SelfContained`。该版本的 Release 输出位于 `build/winui-self-contained/Release/`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -SelfContained -SmokeTest -Package
```

准备依赖后运行回归测试。使用 Visual Studio 2022 时，将生成器改为 `Visual Studio 17 2022`。

```powershell
cmake -S winui/tests -B build/winui-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/winui-tests --config Release
ctest --test-dir build/winui-tests -C Release --output-on-failure
cmake -S winui/pdf -B build/winui-pdf -G "Visual Studio 18 2026" -A x64
cmake --build build/winui-pdf --config Release
ctest --test-dir build/winui-pdf -C Release --output-on-failure
```

测试使用临时数据目录和本机模拟服务。Windows 托盘、快捷键及划词的交互验证需要可操作的桌面。

## 工程结构

| 路径 | 内容 |
| --- | --- |
| `winui/` | WinUI 界面、AI 请求、文档任务、SQLite 存储及 Windows 桌面集成 |
| `winui/pdf/`、`winui/ocr/` | PDF 和 OCR 独立工作进程 |
| `winui/tests/`、`tests/fixtures/` | 原生回归测试、文档和词典样例 |
| `assets/` | 应用图标及 Windows 版本资源 |
| `scripts/`、`tools/` | 构建、打包、依赖准备、字体处理及样例生成 |

PDFium 在独立工作进程中处理原页。DirectWrite 负责译文排版，qpdf 分批合成导出文件。PDFium、qpdf、Noto 字体和构建时的 FontTools 版本与哈希由 `scripts/pdf-dependencies.json` 固定。OCR 桥接库使用 RapidOCR 和 ONNX Runtime。 [第三方组件及许可](THIRD_PARTY_NOTICES.md)

## 版本记录

**1.0.0 原生 Windows 重构。** 从 Qt 迁移到 C++/WinRT 和 WinUI 3。翻译工作台改为上下布局，自动识别查词与翻译，支持粘贴即翻译。DOCX 支持本机 Word 双栏整页预览，可按段落和表格行联动或独立翻页，并支持逐段校对和另存可编辑文档。