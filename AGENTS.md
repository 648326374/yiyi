# 仓库贡献指南

## 项目结构与模块划分

译意 Yiyi 是使用 C++17 和 Qt Quick / QML 开发的 Windows 翻译应用。

- `src/`：AI 接口、控制器、SQLite 存储、桌面集成、OCR 和 PDF 处理。`PdfController` 调度任务，`PdfEngine` 在独立工作进程中处理 PDF。
- `qml/`：页面、对话框、通用控件和主题。
- `assets/`、`resources.qrc`：图标、插画、Windows 资源及 QML 资源清单。
- `tests/`：Qt Test 测试、测试样例和模拟 HTTP 服务。
- `scripts/`：依赖准备、构建和打包；`tools/`：工作进程及可选验证工具；`docs/`：使用说明和验证记录。
- `build/`、`dist/`、`artifacts/`：已被 Git 忽略的构建产物、发行包和验证材料。

## 构建、测试与本地运行

需要 Qt 6.8 及以上、CMake 3.24 及以上、Ninja 和 MinGW。现有预设使用 Qt 6.12.0、MinGW 13.1.0。编译 OCR 桥接库还需要 MSVC，准备 PDF 字体需要 Python 3。

一键准备依赖、编译、测试并打包：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Package
```

安装路径不同可通过脚本参数指定。

依赖准备完成，并将 Qt、MinGW 的 `bin` 目录加入 `PATH` 后，也可分步执行：

```powershell
# 配置工程
cmake --preset windows-mingw
# 使用 6 个并行任务编译
cmake --build --preset windows-mingw -j 6
# 运行测试
ctest --preset windows-mingw
# 使用独立数据目录启动预览，关闭桌面集成功能
build/Yiyi.exe --data-dir artifacts/manual-test --no-desktop
```

本机预设可写入已被 Git 忽略的 `CMakeUserPresets.json`。

## 代码风格与命名约定

使用四个空格缩进，大括号和换行方式遵循所在文件的风格。C++ 类和 QML 组件使用大驼峰命名，如 `PdfController`；方法和属性使用小驼峰，如 `setPage`；私有成员使用 `m_` 前缀。C++ 文件名使用小写，如 `pdfcontroller.cpp`。界面复用 `Theme.qml` 和通用控件。仓库未统一配置格式化工具，提交前运行 `git diff --check` 检查空白问题。

## 测试要求

使用 Qt Test，测试文件命名为 `tests/tst_*.cpp`。测试槽函数名称应描述被验证的行为，参数化测试使用 `*_data()`。行为变更应补充有针对性的回归测试，重点关注取消、任务恢复和 PDF 导出完整性。

仅运行 PDF 测试：`ctest --preset windows-mingw -R yiyi_pdf`。桌面测试需要可交互的桌面；原生 OCR 验证需设置环境变量 `YIYI_TEST_NATIVE_OCR=1`。测试报告应明确记录跳过项。

## 提交与合并请求

历史提交通常使用简短中文标题，如 `完善pdf导出`，没有强制要求使用 `feat:`、`fix:` 等前缀。每次提交聚焦一个明确主题。合并请求应说明行为变化、关联问题和验证结果；界面变更附截图，并同步更新相关文档。

## 安全与配置

自动化测试使用临时配置和本机模拟服务。API Key、个人设置、任务数据库及用户文档不得提交到仓库或放入发行包。真实模型验证仅在明确启用时执行。导出 PDF 必须保留源文件，另存译稿。
