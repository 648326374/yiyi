# 仓库贡献指南

## 项目结构与模块划分

译意 Yiyi 是使用 C++20、C++/WinRT 和 WinUI 3 开发的 Windows 翻译应用。

- `winui/`：XAML 界面、AI 接口、文档任务、SQLite 存储与桌面集成。`PdfTask` 调度任务，`winui/pdf/` 和 `winui/ocr/` 分别构建 PDF 和 OCR 工作进程。
- `assets/`：应用图标和 Windows 版本资源。
- `winui/tests/`：原生回归测试与模拟 HTTP 服务。`tests/fixtures/` 保存共享测试样例。
- `scripts/`：依赖准备、构建和打包。`tools/` 保存 OCR 桥接库、字体处理和样例生成工具。`docs/` 保存使用说明和验证记录。
- `build/`、`dist/`、`artifacts/`：已被 Git 忽略的构建产物、发行包和验证材料。

## 构建、测试与本地运行

需要 Visual Studio 的「使用 C++ 的桌面开发」工作负载、Windows SDK 10.0.26100.0、CMake 3.24 及以上，以及 Python 3。使用 Visual Studio 2026 生成器时需要 CMake 4.2 及以上。运行程序和启动检查需要 Windows App Runtime 2.4 x64。

准备依赖、编译、检查启动并打包：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -SmokeTest -Package
```

可用 `-PythonPath` 指定用于准备 PDF 字体的解释器。Release 构建位于 `build/winui/Release/`，便携包位于 `dist/`。

准备依赖后，可单独构建并运行回归测试。使用 Visual Studio 2022 时，将生成器换成 `Visual Studio 17 2022`。

```powershell
cmake -S winui/tests -B build/winui-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/winui-tests --config Release
ctest --test-dir build/winui-tests -C Release --output-on-failure
cmake -S winui/pdf -B build/winui-pdf -G "Visual Studio 18 2026" -A x64
cmake --build build/winui-pdf --config Release
ctest --test-dir build/winui-pdf -C Release --output-on-failure
build/winui/Release/Yiyi.WinUI.exe --data-dir artifacts/manual-test --no-desktop
```

## 代码风格与命名约定

使用四个空格缩进，大括号、换行和命名遵循所在模块的风格。私有成员使用 `m_` 前缀。界面复用 `App.xaml` 中的主题资源和通用控件。仓库未统一配置格式化工具，提交前运行 `git diff --check` 检查空白问题。

## 测试要求

回归测试由 CTest 运行。测试文件沿用 `*_smoke.cpp` 与配套 Python 用例的命名。行为变更应补充有针对性的回归测试，重点关注取消、任务恢复和 PDF 导出完整性。

仅运行 PDF 测试可使用 `ctest --test-dir build/winui-pdf -C Release --output-on-failure`。Windows 桌面交互验证需要可操作的桌面，测试报告应明确记录跳过项。

## 提交与合并请求

历史提交通常使用简短中文标题，如 `完善pdf导出`，没有强制要求使用 `feat:`、`fix:` 等前缀。每次提交聚焦一个明确主题。合并请求应说明行为变化、关联问题和验证结果；界面变更附截图，并同步更新相关文档。

## 安全与配置

自动化测试使用临时配置和本机模拟服务。API Key、个人设置、任务数据库及用户文档不得提交到仓库或放入发行包。真实模型验证仅在明确启用时执行。导出 PDF 必须保留源文件，另存译稿。
