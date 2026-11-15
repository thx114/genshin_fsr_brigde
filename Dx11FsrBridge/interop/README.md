# DX11 NGX 超分前接口 V1

[BeforeSrV1.h](BeforeSrV1.h) 定义 Windows x64 进程内 ABI。通过**实际加载的桥模块**的 `HMODULE`
解析导出，不依赖 DLL 文件名。旧版桥和不带 NGX 的构建没有这些导出；找不到即沿用原路径。
接口不加载 ReShade/F6，也不改 INI、NGX 创建选项或输出回执。

## 直连 NGX：在 Evaluate 前记录效果

1. `Dx11FsrBridge_RegisterBeforeSrV1` 注册一个记录者。桥从本次超分 Draw 选中输入后，先复制场景颜色、提取深度、解包运动矢量，再在**同一个 D3D11 immediate context** 调用记录者，最后才执行 NGX Evaluate。
2. 回调输入只读，可借用准备好的 Color、Depth、Motion，不需另做捕获。要让本帧超分使用处理后的颜色，返回 `S_OK` 和独立的、同设备同尺寸同存储 DXGI 格式的单采样纹理（单 mip / 单 slice，带 SRV 绑定权限）；任何非空输出都转交一份 COM 引用。不提供兼容结果时 NGX 照用原输入。
3. 回调只在当前 context 上排入工作，并恢复它修改的状态；不做阻塞 I/O、GPU 等待、异步外部提交或桥调度重入。跨帧复用消费者自己的工作纹理，不缓存桥的借用指针。卸载消费者模块前先在回调外注销；`ERROR_BUSY` 时留待回调结束后重试。

没有注册者则不读取源 SRV 的描述、不创建额外纹理，也不增加颜色或深度复制。F6 V1 求值路径独占下面的查询入口，不同时调用这个记录者。

## F6：在现有 V1 Evaluate 内查询

在 `F6_NGX_D3D11_EvaluateFeatureV1` 的**同一调用线程**，用原始 D3D11 context 与原始参数袋的 Color、Depth
调用 `Dx11FsrBridge_QueryBeforeSrFrameV1`。在把参数翻译为 D3D12 指针之前取值。调用方初始化 `size/version`；
`S_OK` 才得到描述，`S_FALSE` 是无当次匹配且不改输出。查询不会改变资源，也不是 GPU 完成凭证；消费者仍拥有自己的提交、同步、共享资源和 ReShade 生命周期。

`color_view_format` 是游戏累积 Draw 的实际 typed SRV 格式；`color_storage_format` 是 NGX 输入副本的存储格式，两者可不同。
目前 Color/Depth/Motion 对应同一次派发、完整 render-size 区域，mip 0、slice 0，原点 `(0,0)`。
`color_transfer == unknown` 不能授权按 G22、sRGB、线性或 HDR 处理：视图解码方式不能证明内容的编码，
也不能以内置 NGX IsHDR 或最终交换链代替。F6 若要在自己的共享纹理上处理，须另行验证复制及工作格式，不可仅凭查询放行颜色门。
