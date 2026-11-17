# DX11 NGX 超分前接口 V1

[BeforeSrV1.h](BeforeSrV1.h) 定义 Windows x64 进程内 ABI。通过**实际加载的桥模块**的 `HMODULE`
解析导出，不依赖 DLL 文件名。旧版桥和不带 NGX 的构建没有这些导出；找不到即沿用原路径。
接口不加载 ReShade/F6，也不改 INI、NGX 创建选项或输出回执。

## HUDless 边界

桥通过 [HudlessV1.h](HudlessV1.h) 提供显式 HUDless 来源接口，尚无已认证的游戏最终无 HUD 图像来源。
下述 `BeforeSrFrameV1.color`
是 render-size 的超分输入，不证明没有 HUD，也不代表最终曝光、后处理或 NR 后的颜色。
普通超分输出同样不能仅凭调用时机或尺寸被声明为 HUDless。

外部 NGX 消费者若支持显式 `Opti.Hudless`，提供方须先证明该资源与本次成功求值属于同帧、
同相机，具有最终输出尺寸，并与实际 Present 颜色的数值域、曝光、后处理及行向相匹配。
无候选时清空参数；消费者必须持有资源至实际复制结束，并使用生产队列的真实同步。
NR 已修改最终颜色或修改结果不确定时，不得复用 NR 前的候选。

使用实际桥模块解析 `Dx11FsrBridge_RegisterHudlessV1` / `Dx11FsrBridge_UnregisterHudlessV1`，
注册一个同步 `RecordHudlessV1` 提供方。仅 NGX 开启的构建有这两个导出；接口只在 F6 V1 求值路径调用。
回调取得本次 request/frame/instance、immediate context、只读输入、待写输出身份和最终尺寸/格式。
返回 `S_OK` 时，回填三个身份，并转交一份独立纹理的 COM 引用，设置 `k_hudless_final_present_color`；
此标志是提供方对同相机、无 HUD、最终颜色/曝光/后处理匹配的声明，桥的结构检查不证明画面内容。
需要翻转行向时加 `k_hudless_flip`。输入、桥输出及游戏目标的别名、跨设备、格式或完整布局失配均拒绝。

桥只在当前 Evaluate 范围以 typed `ID3D11Resource *` 设置 `Opti.Hudless`，并设置 unsigned `Opti.Hudless.Flip`，
返回时先清空指针和 Flip，再释放自己的引用，失败/无输出回执同样清理。没有有效来源时传空且正常 SR 继续。
消费者须在 Evaluate 内取得自己的 COM 引用，负责实际 Present 复制和读退休同步；提供方复用输出时须遵守该同步。
回调在当前 context 排入工作并恢复状态，不允许等待、阻塞 I/O 或重入桥调度。注销返回 `ERROR_BUSY` 时
在回调之外重试；成功注销前不能卸载实现模块。接口不自动注册游戏捕获器，也不把普通 SR 或 backbuffer 候选认证为来源。

## 直连 NGX：在 Evaluate 前记录效果

1. `Dx11FsrBridge_RegisterBeforeSrV1` 注册一个记录者。桥从本次超分 Draw 选中输入后，先复制场景颜色、提取深度、解包运动矢量，再在**同一个 D3D11 immediate context** 调用记录者，最后才执行 NGX Evaluate。
2. 回调输入只读，可借用准备好的 Color、Depth、Motion，不需另做捕获。要让本帧超分使用处理后的颜色，返回 `S_OK` 和独立的、同设备同尺寸同存储 DXGI 格式的单采样纹理（单 mip / 单 slice，带 SRV 绑定权限）；任何非空输出都转交一份 COM 引用。不提供兼容结果时 NGX 照用原输入。
3. 回调只在当前 context 上排入工作，并恢复它修改的状态；不做阻塞 I/O、GPU 等待、异步外部提交或桥调度重入。跨帧复用消费者自己的工作纹理，不缓存桥的借用指针。卸载消费者模块前先在回调外注销；`ERROR_BUSY` 时留待回调结束后重试。

未启用原生 AO、也未注册记录者时，直连路径不准备额外的滤镜资源。直连已注册的记录者优先，不与原生 AO 串叠。
F6 V1 求值路径独占下面的查询入口，不调用这个记录者；若桥启用了原生 AO，F6 收到的是处理后的同帧颜色。

## F6：在现有 V1 Evaluate 内查询

在 `F6_NGX_D3D11_EvaluateFeatureV1` 的**同一调用线程**，用原始 D3D11 context 与原始参数袋的 Color、Depth
调用 `Dx11FsrBridge_QueryBeforeSrFrameV1`。在把参数翻译为 D3D12 指针之前取值。调用方初始化 `size/version`；
`S_OK` 才得到描述，`S_FALSE` 是无当次匹配且不改输出。查询不会改变资源，也不是 GPU 完成凭证；消费者仍拥有自己的提交、同步、共享资源和 ReShade 生命周期。

`color_view_format` 是游戏累积 Draw 的实际 typed SRV 格式；`color_storage_format` 是 NGX 输入副本的存储格式，两者可不同。
只有实际被复制的源纹理、mip 0 的单层 `Texture2D` SRV 才发布视图格式；空视图或同尺寸错资源留为 `DXGI_FORMAT_UNKNOWN`。
原生 AO 成功时发布 AO 实际使用的 UNORM SRV，并只继承本帧已认证输入的编码；未知输入仍为未知。
目前 Color/Depth/Motion 对应同一次派发、完整 render-size 区域，mip 0、slice 0，原点 `(0,0)`。
`color_transfer == unknown` 不能授权按 G22、sRGB、线性或 HDR 处理：视图解码方式不能证明内容的编码，
也不能以内置 NGX IsHDR 或最终交换链代替。F6 若要在自己的共享纹理上处理，须另行验证复制及工作格式，不可仅凭查询放行颜色门。

## 当前 producer 编码认证

2026-11-16 的认证来自已审实际 PS/VS 指令程序和同次 Draw 的状态，不依赖场景名、资源地址或最终交换链。
已审 PS 的 SDR 分支使用 `max(1.055 * pow(c, 1/2.4) - 0.055, 0)`，对应常用快速 sRGB 近似，
保留游戏自己的量化抖动。只有当前 CB0 的模式选择 SDR、附加 gamma 等于 1、叠色关闭时，才可声明 `srgb`。
PQ 坐标后接未知 LUT、额外 gamma 和未知叠色路径保留 `unknown`；本声明不代表精确分段 sRGB 曲线或 GPU 完成。

CPU 常量镜像来自实际 CreateBuffer 初值、UpdateSubresource 或 Map/Unmap；GPU 复制、丢弃和不透明命令会使旧声明失效。
认证还要求单个完整视口、有效全屏三角形、完整裁剪、无不确定深度遮挡、无混合或逻辑操作，且写入 RGB。
全屏几何只从不超过 4096 字节的实际 VB/IB CPU 数据判断；带 UAV 或 STREAM_OUTPUT 的 GPU 可写几何不参与认证。
IMMUTABLE 几何可凭 D3D 创建契约保留初值，其余几何在不透明命令后需要新的 CPU 来源。
暂不支持 Context1 非零 constant-buffer first 或不足 105 个寄存器的范围。

证书绑定实际资源、context、写入序号及观察代次，仅在原 Draw 返回且常量和几何版本未变时发布，供本桥消费一次。
任何已观察覆盖都会作废旧证书；没有完整证据时继续正常 SR 并返回 `unknown`。这些检查不新增同步 GPU 读回、计时探针或配置键。

## 源颜色取证（仅诊断构建）

在独立测试副本中，使用诊断开启、`DX11FSRBRIDGE_RELEASE_RUNTIME=OFF` 的桥。在 `[Dx11FsrBridge]`
启用现有 `Fsr2TraceColorProducers=1`，在要查的场景按 F6 一次；桥模块目录输出 `Dx11FsrBridge.color_chain.json`
和 `Dx11FsrBridge.motion_chain.json`。如需对照 writer shader，可同时启用现有 `DumpPixelShaders=1`。
这不是性能测试配置，须保持其他诊断开关关闭。

文件根记录 PID、SDK 调用输入 context、实例、游戏 Render 帧序号、实际复制源纹理和读端 SRV 格式。
`writes` 列出观察到的资源写入候选及写端视图/着色器；`related_last_writes` 是各依赖资源的最后观察，**不是**完整 GPU 执行轨迹。
上下文不匹配、未见写入、部分复制、录制/重放、常量选择或着色器被改写时，仍需同帧 GPU 捕获等独立证据。
不从历史地址读回 GPU 常量或颜色；不以此文件、某个视图枚举或 SDK 调用尝试当作 transfer 或求值成功凭证。
