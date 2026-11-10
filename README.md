# AIDebug

让外部 AI Agent 通过 MCP 检查和调试正在运行的游戏：观察场景、执行代码、控制运行、应用和撤销方法补丁。

每个引擎的实现、维护工具和补丁放在对应目录中。日常使用由引擎或项目依赖自动接入，无需额外运行插件安装器。

| 目录 | 实现 | 当前适配 |
| --- | --- | --- |
| [Godot/](Godot/README.md) | Godot 源码模块，可选 .NET 求值和补丁 | Godot 4.8-beta2，Windows x64 已验证 |
| [Sbox/](Sbox/README.md) | s&box 项目编辑器入口和可显式加载的运行时调试程序集 | Windows x64，编辑器 Play 与独立游戏 |


## 直接使用

Godot：使用已集成 AIDebug 的编辑器或 Debug 模板打开项目，按 `Ctrl+Alt+Shift+F12` 或传入 `--ai-debug` 激活。GDScript、C# 项目均无需安装项目插件，也无需创建 C# 宿主；调试程序集随引擎提供。普通使用者无需 Python、.NET SDK 或本机编译工具。引擎维护者负责提前构建集成版本，普通官方发行版不包含本模块。

s&box：Verso 更新 `Sbox/` Library 依赖后，编辑器自动发现并编译入口；通过 **AI Debug → 启动本地调试** 激活。详见 [s&box 使用说明](Sbox/README.md)。

## 源码维护：只取 Godot 实现

维护者可只展开 `Godot/`，版本由 Git 提交或项目依赖锁文件固定：

```powershell
git clone --filter=blob:none --sparse https://github.com/RailgunHamster/AIDebug.git AIDebug
git -C AIDebug sparse-checkout set Godot
git -C AIDebug checkout <固定的tag或commit>
```

稀疏检出保留 `Godot/` 和必要的仓库根文件。版本仍由整个仓库的提交标识；安装脚本不自动拉取代码或切换版本。

## 引擎维护者构建

仅源码引擎维护者需要此步骤；已有集成编辑器的项目成员直接使用。构建命令会检查引擎适配、更新外部模块路径，再执行原生与托管构建，无需先执行独立的 `install` 命令：

```powershell
python Godot/install.py check --engine "../engine"
python Godot/install.py build --engine "../engine" --jobs 28
```

`check` 只检查；`build` 自动完成源码接入并编译 .NET 编辑器、胶水和程序集。模块源码留在本仓库，通过 Godot 的 `custom_modules` 加入构建。

适配版本、依赖、增量构建、调试命令和回归测试见 [Godot 使用说明](Godot/README.md)。
