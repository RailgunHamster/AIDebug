# 引擎集成补丁

当前适配 Godot **4.8-beta2**，官方基线提交为 `cafc17a192acf67903910a68aeb3c076c0907a4b`。文件列表及校验和在 [compatibility.json](../compatibility.json)。

`godot-4.8-beta2-ai-debug.patch` 只包含模块之外必需的 .NET 接入：

- `modules/mono/mono_gd/gd_mono.h/.cpp`：保留运行时加载委托、按需调用 Godot.AIDebug、启动时的 JIT 内联选项及 Debug 导出接入。
- `modules/mono/build_scripts/build_assemblies.py`：根据本地安装记录寻找外部模块，构建并复制调试程序集。

补丁不安装 AIGuide 编辑器插件。已有引擎内置 AIGuide 时，安装工具会在构建其程序集时传入 AIDebug 模块路径，以共享工具定义；AIGuide 独立仓库不受修改。

使用 `python Godot/install.py install --engine <Godot源码>` 应用。已应用的补丁会被识别，重复执行不重复插入。首次应用零上下文补丁前，工具逐个校验目标文件与已审阅官方基线的 SHA-256；未知改动、部分应用或不兼容版本会拒绝安装。安装不会覆盖冲突文件。

维护新的引擎适配时，应增加新的补丁配置，验证干净源码安装、重复安装、冲突拒绝和实际引擎构建，再更新支持版本。
