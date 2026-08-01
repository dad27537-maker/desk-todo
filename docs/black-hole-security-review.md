# rossning92/Blackhole 源码与安全审查

## 审查范围

- 仓库：<https://github.com/rossning92/Blackhole>
- 固定提交：[`6beeeda1acade00d304d5615de6f444878fc384d`](https://github.com/rossning92/Blackhole/commit/6beeeda1acade00d304d5615de6f444878fc384d)
- 提交时间：2024-12-01（提交者时间）
- 审查方式：浅克隆后静态逐文件检查；没有执行上游构建、脚本或二进制。
- 本地只读快照：`third_party_audit/Blackhole`

本报告只能说明上述提交中未发现特定恶意行为，不能证明未来提交、Conan 下载物、
显卡驱动或构建环境绝对安全。

## 项目架构

入口为 `src/main.cpp`，负责创建 1920x1080 无边框 GLFW 窗口、初始化 GLEW 和
Dear ImGui、加载本地纹理，然后逐帧执行渲染管线。`src/render.cpp` 管理全屏四边形、
浮点纹理、FBO 和按文件名缓存的 Shader Program；`src/shader.cpp` 从磁盘读取并编译
GLSL；`src/texture.cpp` 用 stb_image 读取 PNG 与六面 cubemap。

渲染管线如下：

1. `blackhole_main.frag` 在每像素最多 300 步内积分近似光线轨迹，检测事件视界，累积
   吸积盘体密度与发光，并在光线逃逸后采样星云 cubemap。
2. 亮度提取。
3. 最多 8 层降采样 Bloom。
4. 逐层升采样并累加。
5. 原图与 Bloom 合成。
6. ACES 近似色调映射和 Gamma 校正。
7. ImGui 在最终画面之上绘制参数面板。

主要依赖来自 `CMakeLists.txt:18-19`：imgui 1.86、glfw 3.3.6、glew 2.2.0、
glm 0.9.9.8、stb cci.20210713，均由 Conan 1.x 在配置阶段获取。

## 安全结论

### 未发现的行为

在当前提交的运行时代码中没有发现：

- HTTP/WebSocket/原始 Socket 网络请求；
- `CreateProcess`、`ShellExecute`、`system`、`popen` 或脚本解释器调用；
- 注册表修改、服务安装、计划任务、自启动、自更新或下载器；
- 业务文件写入、删除、重命名或用户目录扫描；
- 加密、Base64 载荷、明显混淆、反调试或凭据访问；
- 从用户输入路径动态加载 DLL。

运行时文件访问限于固定相对路径的 GLSL 与 PNG 读取。`src/shader.cpp` 使用
`std::ifstream`，`src/texture.cpp` 使用 `stbi_load`。OpenGL 后端在 Windows 上固定加载
系统名 `opengl32.dll`（`src/imgui_impl_opengl3_loader.h:580-593`），随后解析 OpenGL
函数地址；没有拼接外部 DLL 路径。

### 明确风险与证据

| 等级 | 风险 | 证据与影响 | 处理建议 |
|---|---|---|---|
| 高 | 仓库无许可证 | 根目录没有 `LICENSE`；源码头只有 Copyright 声明，PNG/Shader 也没有授权说明。默认版权保留，不能直接复制、分发或嵌入。 | 仅参考公开物理公式和架构思想，clean-room 重写；不复制源码、Shader、截图、星云与色带资源。 |
| 中 | 构建期联网与供应链 | `CMakeLists.txt:8-16` 下载并执行 `conan.cmake`。下载脚本有 TLS 和 SHA-256 校验，这是有效缓解；但 `CMakeLists.txt:18-25` 从 Conan Center 下载并构建缺失包。 | DeskTodo 版本禁止配置期联网；依赖使用 Windows SDK 或锁定源码哈希、离线缓存和 SBOM。 |
| 中 | 依赖未锁修订 | 只锁包版本，没有 `conan.lock`、recipe revision 或 package revision；同名版本的 recipe/binary 可随远端变化。 | 固定精确提交/哈希和编译器配置；CI 验证 SHA-256。 |
| 中 | 资源解析攻击面 | stb 版本为 2021 包；加载 PNG 时没有尺寸、像素数、通道数上限。`texture.cpp:18-25` 对异常通道数可能留下未初始化格式，cubemap 又固定按 RGB 上传。 | 安全复刻版不读取外部图片；程序化生成星空和色带。若以后允许资源，先解码到受限缓冲并限制尺寸。 |
| 中 | GPU 资源耗尽 | 1080p 每像素最多 300 步，噪声 LOD 可到 12，Bloom 最多 8 层；窗口尺寸虽可变化，但主要纹理固定 1920x1080。 | 限制步数和分辨率，动态质量控制；不可见、锁屏、节能时暂停。 |
| 低 | 清理不完整 | `main.cpp:335-337` 的 ImGui Shutdown 被注释；FBO、纹理、Program、VAO 的缓存没有统一释放。 | RAII 管理 GPU 对象；设备丢失和窗口销毁必须幂等释放。 |
| 低 | Shader/资源相对路径 | 从当前工作目录读取固定相对路径，若安装目录可被低权限用户修改，可替换 Shader/图片造成 GPU 崩溃或视觉欺骗。 | Shader 编译进资源或二进制；安装目录 ACL 只读；生产环境禁用任意 Shader 热加载。 |

### 未发现木马不等于可直接信任

当前源码规模较小、行为集中在 OpenGL 渲染，没有明显持久化或外联逻辑，静态证据不支持
“存在木马”的判断。但没有 CI、签名、发行物可复现性或完整依赖锁；如果直接执行原仓库
构建，最大的风险来自配置期联网和远端包，而不是已检查的主程序逻辑。

## 版权隔离决定

DeskTodo 安全复刻版不得复制：

- `blackhole_main.frag` 和任何后处理 Shader 的表达代码；
- `assets/`、`docs/` 下的 PNG/GIF；
- 随仓库提交的 ImGui OpenGL/GLFW backend 文件；
- 原项目的类名、参数拼写、常量组合或 UI 排列。

可以独立使用的知识包括：Schwarzschild 非旋转黑洞、事件视界、光子轨迹数值积分、
吸积盘体密度、HDR/Bloom/色调映射等公共物理和通用图形学概念。实现应从论文、教材、
Microsoft/Khronos 官方文档重新推导，并保留设计来源记录。
