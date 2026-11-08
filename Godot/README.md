# AI Debug：通用运行时调试模块

`Godot/modules/ai_debug` 为正在运行的 Godot 项目提供场景检查、代码求值、运行控制与方法补丁。基础能力使用 Godot Variant API，不要求项目安装插件、继承特定基类或实现业务接口。C# 求值与补丁需要 .NET 引擎及 `Godot.AIDebug.dll`。

## 安装方式

当前补丁适配 **Godot 4.8-beta2**，官方基线提交为 `cafc17a192acf67903910a68aeb3c076c0907a4b`。引擎补丁的范围和校验规则见 [patches/README.md](patches/README.md)。C# 能力需要 .NET 10 SDK；原生引擎构建需要 Git、Python 3.10+、uv，以及 Godot 对应平台的编译工具和 SDK。本机验证使用 Windows x64 / Visual Studio。

推荐将两个仓库并列放置：

```text
workspace/
├── engine/                      # Godot 源码
└── AIDebug/                     # 本仓库，可稀疏检出 Godot/
    └── Godot/
        ├── install.py
        ├── compatibility.json
        ├── modules/ai_debug/
        ├── patches/
        ├── tools/aidbg/
        └── tests/
```

以下命令均在 **AIDebug 仓库根目录**执行：

```powershell
# 只检查，不修改文件：
python Godot/install.py check --engine "../engine"
python Godot/install.py install --engine "../engine" --dry-run

# 安装及完整构建 .NET 编辑器、胶水与程序集：
python Godot/install.py install --engine "../engine"
python Godot/install.py build --engine "../engine" --jobs 28
```

安装工具识别已经应用的补丁，保留已有 `custom.py` 配置，并加入一个可重复更新的 `custom_modules` 配置块。模块使用仓库里的源码参与构建；无需复制到引擎的 `modules/` 下，也不建立目录链接。已有另一个 `engine/modules/ai_debug` 时会拒绝覆盖，需先自行迁出。

本机路径记录在引擎的 `.ai_debug-install.json` 和被引擎忽略的 `custom.py` 中；不应提交这些本地路径。仓库移动后重新运行 `install` 更新路径。脚本不自动获取新版本、不切换 Git 分支。引擎未知改动、补丁部分应用或版本不兼容时会拒绝安装，写入失败会恢复本次修改的文件。

首次安装完成后，按需要使用增量命令：

```powershell
# 只重新编译原生编辑器：
python Godot/install.py build --engine "../engine" --native-only
# 只重新构建并部署调试程序集及已有的 AIGuide 内置程序集：
python Godot/install.py build --engine "../engine" --managed-only
# 编译带调试模块的 .NET Debug 导出模板：
python Godot/install.py build --engine "../engine" --target template_debug
```

完整 `build` 会调用引擎原有的胶水和程序集构建工具。`template_debug` 构建需要先完成编辑器构建。此安装器不安装 AIGuide 插件；已有引擎内置 AIGuide 时，会向它传入模块路径来共享工具定义。

## 范围

模块提供 21 个工具：

| 类别 | 工具 |
| --- | --- |
| 观察 | `info`、`tree`、`get`、`logs`、`screenshot`、`dump` |
| 操作 | `set`、`call`、`eval_gd`、`run_gd`、`eval_cs` |
| 运行控制 | `pause`、`step`、`time_scale` |
| C# 补丁 | `patch_cs`、`unpatch`、`list_patches` |
| 原生补丁 | `hook_native`、`unhook_native`、`list_native_hooks` |
| 清理 | `reset` |

项目存档、业务状态序列化和输入录制回放已从核心移除。旧的 `snapshot_*`、`input_record_*`、`input_play`、`input_status` 工具不再提供；项目无需实现 `IAIDebugSnapshot` 或保存／恢复方法。已有快照文件不会自动删除。需要复现业务现场时，由独立的项目测试工具负责保存状态和驱动输入。

## 接入与激活

独立运行时，游戏进程直接提供 HTTP 入口：

```powershell
bin/godot.windows.editor.x86_64.mono.exe --path <project> --ai-debug
# --ai-debug-port=7651
# --ai-debug-token=<token>
# --ai-debug-bind=127.0.0.1
# --ai-debug-inline  保留 JIT 内联
```

也可以使用 `GODOT_AI_DEBUG=1`，以及 `GODOT_AI_DEBUG_PORT`、`GODOT_AI_DEBUG_TOKEN`、`GODOT_AI_DEBUG_BIND`。在 Debug/Editor 构建中可按 `Ctrl+Alt+Shift+F12` 激活。默认绑定回环地址；绑定其他地址必须显式提供 token。请求使用 `X-AIDebug-Token` 或 Bearer token。

从编辑器运行时，`AIDebugEditorDebuggerPlugin` 在编辑器进程监听 `127.0.0.1:7651`，通过 `EngineDebugger` 转发请求；游戏无需另外开放端口。内置 AIGuide 可在 `7650` 暴露对应的 `ai_debug_*` 工具。两者使用同一份 `tools.json`，分别由构建步骤生成 C++ 头文件及嵌入托管资源。

连接信息保存在项目的 `user://ai_debug/session.json`，正常关闭时只删除属于当前服务的文件。`aidbg` 可以发现默认用户目录下的连接信息；自定义用户目录需要显式传入端口和 token。

未激活时不监听端口、不加载调试程序集，但仍有 idle callback 检查激活条件，不宣称零开销。`template_release` 构建强制禁用该模块，Release 导出插件也不会主动复制调试程序集。

## 执行与取消

`eval_gd`、`run_gd`、属性访问和方法调用在主线程执行。`eval_cs` 支持 `thread:"main"` 和 `thread:"worker"`。后台模式适合处理独立的线程安全数据；场景树辅助函数会拒绝后台访问，直接调用其他 Godot API 时仍需遵守引擎线程规则。

C# 求值通过托管任务执行，由主循环轮询完成结果，不创建需要主线程等待退出的原生工作线程。每个进程同时最多运行一个 C# 求值；正在执行时的新求值返回 `busy`。会话变量跨调用保留，`reset_session:true` 或 `reset` 可清除。

`timeout_ms`（1–300000，默认 10000）是响应期限和协作取消期限，不是强制终止保证。全局变量 `Cancellation` 提供 `CancellationToken`，长循环应主动检查：

```csharp
while (hasMoreWork)
{
    Cancellation.ThrowIfCancellationRequested();
    // 有限、可返回的一段工作
}
```

超时或客户端断开会请求取消，返回 `execution_may_continue` 表示代码是否可能仍在执行。忽略取消的后台代码仍占用唯一执行槽，避免反复提交失控任务。主线程代码必须尽快返回；同步阻塞、死循环及不能协作取消的外部调用仍可能卡住主线程。异步主线程脚本可以等待后续帧，不再同步等待自身完成。

`reset` 请求取消、丢弃会话状态、撤销本模块的 C# 和原生补丁，并恢复激活时的暂停状态和时间倍率。它不会撤销 `set`、`call`、脚本执行造成的任意业务副作用。清理失败不会假报成功，未成功撤销的补丁会保留在活动列表中。HTTP DELETE 和空闲自动清理遵循相同规则。

## 原生 Hook

`hook_native` 支持 `log`、`return_const`、`dll`，当前限 Windows x64。`log` 采样前四个整数参数寄存器、整数返回寄存器及调用次数；保留浮点参数和浮点返回寄存器，但不将它们解释成带类型的参数。`return_const` 返回 64 位整数，不适用于浮点或复杂结构返回值。

符号可以是 PDB 名、`module.dll!Export` 或地址。DLL 替换必须匹配目标 ABI。已经内联的调用点不会经过符号入口。C# 小方法补丁建议启动时传入 `--ai-debug`，在 CLR 初始化前禁用 JIT 内联；运行中激活不能追溯取消已发生的内联。

## HTTP 行为

运行时与编辑器桥共用请求解析、token 和 Origin 校验。最大请求头 16 KiB、请求体 16 MiB、同时连接 32 个；未完成请求 30 秒后关闭。仅接收 Content-Length 请求体，不支持 chunked；每个 HTTP 请求只接受一条 JSON-RPC 消息，不支持批处理。提供 `initialize`、`tools/list`、`tools/call` 和 `ping`，不提供 SSE 流。

## 回归测试

从 AIDebug 仓库根目录执行：

```powershell
# 干净源码安装、重复执行、保留配置、冲突拒绝与失败回滚：
$env:GODOT_AI_DEBUG_TEST_SOURCE = (Resolve-Path "../engine").Path
python Godot/tests/test_install.py

# 当前已构建编辑器的直连路径：
python Godot/install.py test --engine "../engine"
# 编辑器桥以及已有 AIGuide 的 7650 转发：
python Godot/install.py test --engine "../engine" --bridge
```

桥接测试需要空闲的 7650、7651 端口，以及已安装内置 AIGuide 的 .NET 编辑器。运行时测试创建独立的临时项目，覆盖工具范围、GDScript/属性访问、C# 会话与异步执行、并发连接关联、执行槽忙、超时后重置、后台任务运行时退出、补丁撤销与 HTTP 非法请求。桥接模式额外检查 AIGuide 的工具 schema 和实际转发。

Windows x64 开发者命令行可将 `Godot/tests/native_fixture.c` 编译成 DLL，再添加 `--native-dll <DLL绝对路径>`，验证四个 double 参数穿过原生日志跳板后的值。测试只检查自己创建的临时项目，并在结束后清理。

## 命令行客户端

```powershell
python Godot/tools/aidbg/aidbg.py --port 7651 --token <token> info
python Godot/tools/aidbg/aidbg.py --port 7651 --token <token> tree --depth 4
```
