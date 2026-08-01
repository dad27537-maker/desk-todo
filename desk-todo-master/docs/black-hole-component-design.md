# DeskTodo 安全黑洞组件设计

## 目标与边界

组件是 DeskTodo.Next 进程内、离线、只渲染的桌面视觉组件。它不读取任意外部 Shader
或图片，不联网、不启动进程、不加载非系统 DLL、不写文件，不接触桌面文件管理权限。
设置由 DeskTodo 自己的可信配置层传入；渲染核心只接收数值参数和宿主 GPU 表面。

本地骨架位于 `DeskTodo.Next/src/Components/BlackHole`：

- `BlackHoleTypes.h`：稳定的数据结构、质量档位和状态。
- `IBlackHoleRenderBackend.h`：宿主无关后端接口。
- `BlackHoleComponent.*`：生命周期、线程安全设置、暂停、设备丢失和动态分辨率。
- `shaders/BlackHoleCleanRoom.hlsl`：独立编写的 D3D11 Shader 原型，无外部纹理。

## 模块边界与 API

`Component` 只负责策略，不直接调用 WinHTTP、Shell、文件系统或注册表。后端负责 GPU
资源，但只能使用宿主传入的 device/context/target。

```cpp
bool Initialize(const SurfaceDescriptor& surface);
bool Resize(uint32_t width, uint32_t height, float dpiScale);
bool RenderFrame(const FrameContext& frame, FrameStats& stats);
void UpdateSettings(const Settings& settings);
void Suspend();
void Resume();
void NotifyDeviceLost();
void Shutdown();
```

所有参数在进入后端前经过 `Settings::Sanitize`。接口故意不包含 URL、文件路径、脚本文本、
DLL 名称和命令行，避免后续实现无意扩大权限。

## 推荐线程模型

- UI/Overlay 线程：处理窗口、布局和输入，只提交最新不可变设置快照。
- 单一渲染线程：独占 D3D immediate context，按宿主帧节奏渲染，不创建额外线程池。
- 设置更新：短时 mutex 复制小结构；渲染期间不持锁。
- 销毁：宿主先停止定时器，再等待最多一帧，随后在渲染线程释放 GPU 对象。
- 设备丢失：后端返回失败，组件进入 `DeviceLost`，由宿主重建设备后重新 Initialize。

DeskTodo 当前 D2D DC RenderTarget 为单线程对象，不能跨线程直接使用。D3D11 后端应把结果
输出到独立 BGRA 纹理；最终合成必须回到 Overlay 所属线程。

## DeskTodo.Next 集成方案

### 推荐：D3D11 离屏后端

1. 新建 `D3D11BlackHoleBackend`，创建 BGRA 支持设备、常量缓冲和两个像素 Shader。
2. 主通道输出半精度 HDR 纹理；低/均衡档使用 0.5/0.75 渲染比例。
3. Bloom 仅两级或四级，避免原项目八级链路。
4. 输出目标为 `DXGI_FORMAT_B8G8R8A8_UNORM` 预乘 Alpha。
5. 短期接入现有 D2D DC：每 2-3 帧 staging 回读，再创建/更新 `ID2D1Bitmap`。
6. 长期重构：把 Overlay 升级为 D3D11 + DXGI Surface 的 D2D device context，实现零拷贝。

短期 CPU 回读成本较高，但模块隔离清晰；应限制组件区域和最大 30 FPS。完成 D2D/D3D
互操作后再开放 60 FPS。

### OpenGL 宿主

只作为独立 `WS_EX_TOOLWINDOW` 子组件使用，不与 DeskTodo 全屏 layered Overlay 混用。
OpenGL context 固定在专用线程，Shader 编译进资源，使用系统 `opengl32.dll`，禁止插件 DLL
和运行时 Shader 文件。缺点是跨 API 合成和透明窗口兼容性差，不作为默认实现。

### WPF

推荐 `D3DImage` + D3D9Ex 共享表面桥接，或者 Windows Composition Hosting。不要使用
WinFormsHost/GLFW 子窗口覆盖桌面层，因为 Airspace 限制会破坏透明和输入穿透。

### WinUI 3

使用 `SwapChainPanel` 或 Composition `ICompositionSurface`。后端暴露共享 D3D11 Texture，
UI 线程仅提交 visual；窗口最小化或不可见时暂停渲染。

### DirectX 独立宿主

这是最自然的路径。由宿主创建 D3D11 device，组件只创建 device-child 资源。禁止后端自行
枚举适配器或切换全局设备；这样可以统一处理 TDR、DPI、多显示器和电源状态。

## 资源管理

- Shader 使用 `.rc` 资源或构建时转成只读字节数组；生产环境不从磁盘编译任意文本。
- 星空、噪声和温度色带均程序化生成，不使用上游 PNG。
- 所有 COM 对象用 `ComPtr`，所有创建函数有对应释放路径。
- Resize 只重建尺寸相关纹理；Program、sampler 和 constant buffer 复用。
- 设备丢失后先释放 view，再释放 texture，最后释放 pipeline state。
- 不缓存跨设备对象，不把原始 COM 指针保存到配置。

## 性能目标与降级

| 场景 | 目标 |
|---|---|
| 1080p 桌面、均衡档 | 组件区域 30 FPS，GPU 时间 <= 6 ms，CPU <= 1 ms |
| 1440p/4K | 默认 0.5-0.75 renderScale，不超过 8 ms GPU |
| 显存 | 基础 <= 64 MiB，Bloom 开启 <= 96 MiB |
| 闲置功耗 | 不可见/锁屏为 0 FPS；电池节能最多 15 FPS、关闭 Bloom |
| 稳定性 | 连续运行 8 小时，显存增长 < 2 MiB |

连续 8 帧超预算时 `renderScale` 降 0.1，最低 0.35；连续 180 帧低于预算 65% 时
升 0.05。降级顺序：关闭 Bloom、减少积分步数、降低 renderScale、降低 FPS、最后显示
静态程序化渐变。任何 Shader 编译或设备错误都不得让 DeskTodo 主进程退出。

## 安全约束

- 构建和运行均默认断网；组件代码不得链接 winhttp/wininet/urlmon/ws2_32。
- 不允许 `CreateProcess`、`ShellExecute`、PowerShell、脚本引擎或自更新。
- 不允许 `LoadLibrary` 加载非系统绝对路径；D3D11 方案不需要显式加载 DLL。
- 不读取用户指定文件；不支持 Shader 热加载或在线材质。
- 不写日志、缓存、截图或设置；持久化必须由 DeskTodo 的受控配置服务完成。
- 参数有硬上限，循环上限编译期固定，纹理尺寸不超过宿主区域和 4096x4096。
- Release 构建启用 CFG、ASLR、DEP、栈保护和 `/guard:cf`，生成 SBOM 与哈希清单。
