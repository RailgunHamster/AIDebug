# AIDebug

让外部 AI Agent 通过 MCP 检查和调试正在运行的游戏：观察场景、执行代码、控制运行、应用和撤销方法补丁。

每个引擎的实现、安装工具和补丁放在对应目录中，独立接入引擎构建。

| 目录 | 实现 | 当前适配 |
| --- | --- | --- |
| [Godot/](Godot/README.md) | Godot 源码模块，可选 .NET 求值和补丁 | Godot 4.8-beta2，Windows x64 已验证 |

## 只取 Godot 实现

已有本地仓库时直接使用 `Godot/`。有远程仓库后，可只展开这个目录：

```powershell
git clone --filter=blob:none --sparse <AIDebug仓库地址> AIDebug
git -C AIDebug sparse-checkout set Godot
git -C AIDebug checkout <固定的tag或commit>
```

稀疏检出保留 `Godot/` 和必要的仓库根文件。版本仍由整个仓库的提交标识；安装脚本不自动拉取代码或切换版本。

## 安装到 Godot 源码

从 AIDebug 仓库根目录执行，路径可以包含空格：

```powershell
python Godot/install.py check --engine "../engine"
python Godot/install.py install --engine "../engine"
python Godot/install.py build --engine "../engine" --jobs 28
```

`check` 只检查；`install` 应用已适配的补丁并配置外部模块路径；`build` 编译 .NET 编辑器、生成胶水并构建程序集。模块源码留在本仓库，通过 Godot 的 `custom_modules` 加入构建。

适配版本、依赖、增量构建、调试命令和回归测试见 [Godot 使用说明](Godot/README.md)。
