# 可直接交给 AI 编程助手的提示词

```text
你正在维护 Windows 原生桌面整理软件 DeskTodo.Next。请实现一个 clean-room 的“安全黑洞”
桌面组件。不得复制 rossning92/Blackhole 或其他无明确许可证项目的源码、Shader、图片、
参数命名和 UI；只能依据公开物理公式、图形学教材和 Microsoft 官方 Direct3D 文档独立实现。

现有骨架：
- src/Components/BlackHole/BlackHoleTypes.h
- src/Components/BlackHole/IBlackHoleRenderBackend.h
- src/Components/BlackHole/BlackHoleComponent.h/.cpp
- src/Components/BlackHole/shaders/BlackHoleCleanRoom.hlsl

技术目标：
1. 首选 C++17 + D3D11，宿主传入 ID3D11Device、ID3D11DeviceContext 和目标纹理；不得创建
   GLFW 窗口，不得引入 Conan、vcpkg 在线下载或第三方运行库。
2. 使用全屏三角形像素 Shader，独立实现有限步数的 Schwarzschild 近似光线弯曲、事件视界、
   程序化吸积盘和程序化星空。所有循环上限固定，输入参数必须钳制。
3. 实现均衡档 96 步、低档 48 步、高档 144 步；Bloom 只允许 2 或 4 层。最终输出 BGRA8
   预乘 Alpha，并支持 renderScale 0.35-1.0。
4. 接入 DeskTodo DesktopOverlayWindow：短期允许 staging 回读为 ID2D1Bitmap，最大 30 FPS；
   将接口设计成可迁移到 D2D device context + DXGI surface 的零拷贝方案。
5. 实现暂停、恢复、Resize、设备丢失重建、锁屏/不可见暂停、电池节能降到 15 FPS 并关闭 Bloom。
6. GPU 对象全部 RAII；失败只禁用组件并显示静态占位，不得终止 DeskTodo。

强制安全要求：
- 无网络请求；不得链接 winhttp、wininet、urlmon、ws2_32。
- 无进程/命令执行；不得调用 CreateProcess、ShellExecute、system 或脚本引擎。
- 无文件写入；不得读取外部 Shader、纹理、DLL 或用户路径。
- Shader 必须编译进资源或字节数组；程序化生成所有视觉资源。
- 不使用 LoadLibrary 加载第三方 DLL，不实现下载器、自更新、遥测或插件脚本。
- 禁止复制 third_party_audit/Blackhole 下的任何代码和资源，该目录只可用于人工审计证据。

测试要求：
- 单元测试 Settings::Sanitize 的所有上下界和非法组合。
- fake backend 测试 Initialize/Render/Suspend/Resume/DeviceLost/Shutdown。
- WARP 设备运行 Shader，检查输出非全黑、无 NaN/Inf、事件视界中心接近黑色。
- 320x180、1080p、4K 和 DPI 100/150/200% 测试。
- 连续运行 8 小时，显存增长 < 2 MiB；设备重建 100 次无泄漏。
- 断网运行且 ProcMon 观察不到网络、进程创建、外部文件读写。
- Release 二进制导入表不含网络 DLL；扫描无高危结果。

验收后输出：设计说明、代码变更、依赖清单、Shader 来源说明、性能数据、安全测试证据和
未解决风险。不要声称“绝对无漏洞”，只报告已验证范围。
```

## 关键词

clean-room、Schwarzschild、gravitational lensing、event horizon、photon trajectory、
accretion disk、volumetric emission、procedural starfield、full-screen triangle、HLSL、
D3D11、offscreen render target、BGRA premultiplied alpha、D2D/D3D interop、DXGI surface、
device lost、WARP、adaptive resolution、frame budget、battery saver、RAII、SBOM、offline build、
no network、no file write、shader embedding、GPU watchdog、TDR、Bloom、tone mapping。

## 验收标准

- 视觉上存在稳定事件视界、弯曲星空和旋转吸积盘，不依赖任何上游图片。
- 组件可添加、移动、缩放、隐藏和删除，不影响桌面文件交互。
- 均衡档 1080p 组件区域 GPU <= 6 ms；超预算自动降级。
- 隐藏、锁屏和节能状态符合设计中的 FPS/功耗限制。
- Shader 错误、设备丢失、Resize 风暴都不导致 DeskTodo 崩溃。
- 安全扫描确认没有网络、命令执行、外部 Shader/纹理加载和文件写入路径。
- 源码与资源均有明确许可证或为项目原创，并保留来源记录。

## 测试清单

- 参数边界、NaN、Inf、负值和半径倒置。
- Fake backend 生命周期与错误注入。
- WARP 与真实 Intel/NVIDIA/AMD GPU。
- 设备移除、休眠唤醒、锁屏解锁、显卡驱动重启。
- 单屏/多屏、热插拔、DPI 切换、4K、HDR/SDR。
- 频繁 Resize、显示隐藏、添加删除 100 次。
- 帧时间、GPU/CPU、显存、功耗与 8 小时稳定性。
- ProcMon、TCPView/防火墙日志、导入表、静态杀毒与依赖哈希。
- 无网络环境全新安装、首次启动和卸载后残留检查。
- 版权检查：与审计仓库 Shader/资源做相似度人工复核。
