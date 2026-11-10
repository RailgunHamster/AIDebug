# 引擎集成补丁

供引擎维护者审阅和维护源码接入。当前适配 Godot **4.8-beta2**，官方基线提交为 `cafc17a192acf67903910a68aeb3c076c0907a4b`。项目成员使用已集成的引擎，无需处理补丁。

`godot-4.8-beta2-ai-debug.patch` 只包含模块之外必需的 .NET 接入：

- `modules/mono/mono_gd/gd_mono.h/.cpp`：保留运行时加载委托、按需调用 Godot.AIDebug、启动时的 JIT 内联选项及 Debug 导出接入。
- `modules/mono/build_scripts/build_assemblies.py`：通过 `GODOT_AI_DEBUG_MODULE_DIR` 获取外部模块路径，或使用引擎内置 `modules/ai_debug`，构建并复制调试程序集。

已有引擎内置 AIGuide 时，其托管构建配置通过 `AIDebugModuleDir` 引用同一份 `tools.json`。AIGuide 的内置入口由引擎单独维护。

补丁使用零上下文格式。维护新引擎分支时，先对照基线审阅目标文件，在源码根目录用 `git apply --check --unidiff-zero <补丁路径>` 检查适用性，再将对应改动纳入引擎版本控制。已经包含这些改动的集成引擎无需重复应用。

升级引擎适配时，更新补丁和支持版本，并验证补丁可应用、引擎构建、免项目安装启动和实际运行时调试。
