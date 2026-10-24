# DeskTodo

DeskTodo 是一款原生 Windows 桌面整理工具。它用可移动、可缩放的“格子”收纳桌面文件，并集成游戏容器、截图标注、OCR、局域网快传、番茄时钟、Todo 和黑洞动态效果。

当前主工程是 `DeskTodo.Next`，使用 C++17、Win32、Direct2D、WIC、Windows OCR、Winsock、OpenGL/D3D 桌面捕获等原生 Windows 技术构建，不依赖 .NET 运行时。

## 功能概览

### 桌面格子

- 创建、重命名、移动、缩放、折叠、锁定和解散多个格子。
- 支持图标视图与列表视图，可按名称或类型排序，也可在格子内搜索。
- 文件、文件夹、快捷方式和程序可以在桌面与格子之间自由拖入、拖出。
- 采用仅作用于桌面视图的 Shell 过滤管理逻辑归组，不为普通项目额外生成 `.lnk`；右键刷新后格子项目不会闪现，普通“桌面”文件夹窗口仍能看到真实路径。
- 旧版曾搬入私有格子目录的项目会在无同名冲突时安全迁回真实桌面路径；冲突或移动失败时保留原文件，绝不覆盖。
- 自动处理同名文件和文件夹，避免格子项目与桌面新建项目发生名称冲突。
- 支持文件夹映射：直接显示指定目录内容，不移动原目录文件。
- 支持布局吸附、辅助线、网格排列、滚动和多显示器相对坐标。
- 按显示器稳定标识保存 96-DPI 相对锚点；显示器临时关闭时保留离线格子位置，重连后恢复到原屏幕，兼容混合 DPI。
- 多显示器热插拔采用稳定拓扑确认和分批恢复机制：Windows 临时投射到主屏的图标不会覆盖离线显示器的原布局，显示器重新连接后会按屏幕特征恢复位置。
- 热插拔恢复期间会冻结当前拖动使用的显示器拓扑，避免坐标系中途变化导致吸附线消失、位置跳动或拖动卡顿。
- 布局可备份为 `.bak` 文件并按需还原。

### 截图、贴图与 OCR

- 任意区域框选，支持取消、贴图、保存 PNG、复制到剪贴板。
- 画笔、箭头、文字、矩形框标注。
- 可分别设置标注粗细、字体、字号和颜色。
- 工具栏使用图标显示，鼠标悬停时显示中文功能提示。
- 优先使用本地 PP-OCRv6 medium 模型识别文字，支持中文、英文及混排；模型不可用时自动使用 Windows OCR。
- 对小尺寸文字自动放大并保留笔画细节，处理深色背景和低对比度，保留多行文字的换行。
- 识别在后台运行，截图界面保持响应，按 Esc 可取消；识别成功后复制文本并打开结果窗口。
- 支持 LaTeX 识别结果窗口（由 OCR 文本转换，不是专用公式识别模型）。
- 贴图窗口支持缩放、复制、另存为和关闭。

### 局域网快传

- 电脑端显示二维码和临时访问地址，手机扫描后即可在浏览器选择文件。
- 支持图片、视频、文档等任意文件以及多文件连续上传。
- 电脑点击“发送到手机”多选文件，或将文件拖入快传窗口；手机网页自动显示文件列表，点击即可下载。
- 手机下载保留原文件名，支持中文名称和大文件；下载进度可在浏览器下载列表查看，电脑端同时显示发送进度。
- 发送列表可随时清空，关闭快传窗口后自动清空；仅所选文件可供手机下载。文件夹请先压缩后发送。
- 手机端和电脑端均显示传输进度。
- 可修改接收目录，也可直接打开接收文件夹。
- 默认接收目录为 `%USERPROFILE%\Downloads\DeskTodo 快传`。
- 服务仅在快传窗口使用期间运行，并使用随机令牌限制访问。

> 局域网快传使用本机 HTTP 服务，请只在可信任的家庭或办公局域网中使用。首次启动时 Windows 防火墙可能请求网络访问权限。

### 黑洞动态效果

- 集成 `ghostty-blackhole` 渲染代码，通过独立渲染进程叠加在桌面和其他软件上。
- 黑洞会产生引力透镜、吸积盘、光束、多普勒和 Bloom 效果。
- 支持多屏范围与随机移动轨迹。
- 支持“始终显示”和“空闲检测”两种模式。
- 内置多种预设，并可调整色温、倾角、旋转、半径、不透明度、亮度、曝光、速度等参数。
- 设置保存后可再次启动使用，也可以从托盘菜单立即关闭效果。

### 设置中心与桌面组件

- 设置中心按多屏工作区和 DPI 自动缩放，包含以下页面：
  - 基础设置
  - 桌面整理
  - 桌面搜索
  - 组件
  - 截图与贴图
  - 局域网快传
  - 关于
- 可设置开机启动、备份/还原布局。
- 桌面搜索优先查找 `E:\Everything\Everything.exe`，同时兼容 Everything 的标准安装路径；未找到时使用 Windows 搜索。
- 内置番茄时钟、游戏容器和增强 Todo List；Todo 支持优先级、到期日、逾期提示和智能排序，状态会在重启后恢复。
- 番茄时钟支持自定义大小；拉伸窗口时标题、计时数字、进度条、按钮和交互区域会同步缩放，尺寸变化后会完整重绘，避免控件残影和重复显示。
- 游戏容器可收纳一个 `.lnk`、`.url` 或 `.exe` 启动项，并以大卡片方式双击启动。
- 软件为单实例运行，重复启动会把命令发送给已经运行的实例。

## 系统要求

- Windows 10 或 Windows 11 x64
- 支持桌面合成的显卡驱动
- 构建时需要 Visual Studio 2022 或 Visual Studio 2026：
  - “使用 C++ 的桌面开发”工作负载
  - MSVC x64 工具链
  - Windows 10/11 SDK
  - CMake 3.15 或更高版本
- PP-OCRv6 离线识别不依赖系统语言包；Windows OCR 后备引擎需要系统安装相应语言包
- Everything 为可选依赖

## 获取源码

仓库包含黑洞渲染子模块，克隆时需要同时初始化子模块：

```powershell
git clone --recurse-submodules https://gitee.com/dada368/desk-todo.git
cd desk-todo
```

已有工作区可执行：

```powershell
git submodule update --init --recursive
```

## 构建

启用高精度离线 OCR，先执行一次：

```powershell
.\DeskTodo.Next\setup-ocr.ps1
```

脚本会下载固定版本的 ONNX Runtime、OpenCV 和 PP-OCRv6 medium 检测/识别
模型并校验 SHA256，存放在被 Git 忽略的 `DeskTodo.Next\deps\ocr` 下。
首次准备需要联网；实际识别完全在本机进行，不上传截图。

Visual Studio 2026：

```powershell
cmake -S .\DeskTodo.Next -B .\DeskTodo.Next\build `
  -G "Visual Studio 18 2026" -A x64
cmake --build .\DeskTodo.Next\build --config Release --target ALL_BUILD -- /m
```

Visual Studio 2022 用户将生成器改为：

```text
Visual Studio 17 2022
```

主程序生成在：

```text
DeskTodo.Next\build\Release\DeskTodo.Next.exe
```

构建结束后，黑洞着色器、预设和运行资源会自动复制到 `Release` 目录。请保留这些资源与 EXE 的相对位置。
启用 PP-OCRv6 后，构建也会生成 `Release\ocr`，分发程序时需同时保留这个目录
（模型、原生 DLL 及许可证，约 210 MB）。未准备离线模型的源码构建会使用增强后的 Windows OCR。

## 运行

```powershell
.\DeskTodo.Next\build\Release\DeskTodo.Next.exe
```

首次正常启动会更新桌面上的 `DeskTodo 启动.lnk`，快捷方式指向当前运行的正式 EXE。

程序主要通过格子标题栏、设置中心和系统托盘菜单操作。托盘菜单提供刷新、新建格子、新建游戏容器、一键整理、显示/隐藏、锁定、截图、文件传送、布局备份/还原、黑洞开关和退出。

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl + Shift + N` | 新建格子 |
| `Ctrl + Shift + L` | 锁定或解锁全部格子 |
| `Ctrl + Shift + A` | 开始截图 |
| `Ctrl + Shift + F5` | 刷新格子 |
| `Ctrl + Shift + Q` | 退出 DeskTodo |
| `Ctrl + Alt + D` | 通过桌面快捷方式启动 DeskTodo |

## 命令行

DeskTodo 为单实例程序。下列命令既可用于首次启动，也可发送给正在运行的实例：

```powershell
DeskTodo.Next.exe --show
DeskTodo.Next.exe --new-fence
DeskTodo.Next.exe --new-game-fence
DeskTodo.Next.exe --map-folder
DeskTodo.Next.exe --refresh
DeskTodo.Next.exe --auto-organize
DeskTodo.Next.exe --toggle-visibility
DeskTodo.Next.exe --toggle-lock
DeskTodo.Next.exe --settings
DeskTodo.Next.exe --screenshot
DeskTodo.Next.exe --lan-transfer
DeskTodo.Next.exe --backup-layout
DeskTodo.Next.exe --restore-layout
DeskTodo.Next.exe --toggle-blackhole-bg
DeskTodo.Next.exe --exit
```

## 数据与配置

主要用户数据保存在：

```text
%APPDATA%\DeskTodo.Next\
├─ config.json                 格子布局和项目归组
├─ desktop-icon-layout.dat     普通桌面图标的多显示器相对位置
├─ Fences\                     格子存储目录
├─ todo-widget.dat             Todo 状态
├─ pomodoro-widget.dat         番茄时钟状态
├─ lan-transfer-folder.txt     快传接收目录
└─ Screenshots\                图片目录不可用时的截图回退目录
```

`desktop-icon-layout.dat` 是自动维护的二进制快照，用于显示器关闭、重连或 DPI
变化后恢复未归入格子的普通桌面图标；格子的逻辑成员不会写入该快照。

其他位置：

- 截图默认保存到 `%USERPROFILE%\Pictures\DeskTodo Screenshots`。
- 黑洞设置保存在 `HKEY_CURRENT_USER\Software\DeskTodo\BlackHole`。
- 测试时可通过 `DESKTODO_DATA_ROOT` 环境变量把配置写入隔离目录。

修改或清理配置前请先退出 DeskTodo。重要布局建议先在设置中心导出 `.bak` 备份。

## 测试

完成 Release 构建后可运行：

```powershell
$release = ".\DeskTodo.Next\build\Release"

& "$release\DeskTodoNativeSmoke.exe"
& "$release\DeskTodoLogicalFenceSmoke.exe"
& "$release\DeskTodoBlackHoleSmoke.exe"
& "$release\DeskTodoOcrSmoke.exe"
```

测试范围包括：

- 配置原子写入、布局、吸附和多显示器相对坐标
- 文件名冲突处理、格子逻辑归组、桌面刷新持久化和游戏启动项约束
- Todo 优先级、到期日、排序及旧版状态迁移
- 被占用程序、文件夹和快捷方式的格子拖放
- 黑洞参数校验和进程控制
- OCR 基础识别
- 截图工具栏、格子菜单、桌面拖放和悬浮提示的交互探针

当前多显示器修复的 Release 回归结果：

- `DeskTodoNativeSmoke.exe`：119 项检查，0 失败。
- `DeskTodoLogicalFenceSmoke.exe`：通过，包含显示器断开、重连和显示器标识变化恢复检查。
- `git diff --check`：通过。

`DeskTodoDesktopDragProbe.exe` 用于交互式桌面测试，需要在 DeskTodo 正在运行且使用真实 Windows 桌面的会话中执行。

## 项目结构

```text
DeskTodo/
├─ DeskTodo.Next/                 当前主工程
│  ├─ src/App/                    启动、单实例和命令分发
│  ├─ src/Managers/               格子与布局管理
│  ├─ src/Rendering/              Direct2D 格子渲染
│  ├─ src/Services/               文件、Shell、配置、OCR、布局服务
│  ├─ src/Window/                 设置、截图、快传和组件窗口
│  ├─ src/Components/BlackHole/   黑洞控制与兼容层
│  └─ tests/                      原生与交互测试
├─ reference_repos/
│  └─ ghostty-blackhole-main-old/ 黑洞渲染子模块
├─ docs/                          设计、审计和黑洞文档
├─ design-system/                 DeskTodo 视觉设计规范
├─ widgets/                       早期组件脚本与参考资料
└─ artifacts/                     验证记录
```

## 实现说明

- 桌面格子使用全虚拟屏幕透明分层窗口渲染，格子之外保持鼠标穿透。
- 程序采用 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`，布局按显示器稳定标识保存 96-DPI 相对锚点，并在运行时换算为物理像素。
- 桌面项目通过 Windows Shell 桌面视图过滤和逻辑归组状态同步；过滤不会伪造文件删除事件，因此不会隐藏普通文件夹视图中的真实路径。
- 删除、打开、快捷方式和桌面位置等操作尽量使用 Windows Shell 原生接口。
- 黑洞渲染器来自仓库中的 `ghostty-blackhole` 子模块，并作为 DeskTodo 的独立模式启动。

## 安全提示

- 局域网快传没有公网穿透能力，也不应通过路由器端口转发暴露到互联网。
- 恢复布局会覆盖当前格子布局，执行前请确认备份文件来源可靠。
- 强制结束进程可能来不及释放格子项目状态；正常情况下请从托盘菜单退出。

## 第三方代码

项目包含或参考了第三方开源代码，主要包括：

- `reference_repos/ghostty-blackhole-main-old`
- `third_party_audit/Blackhole`

多显示器页面隔离、热插拔后保留各显示器布局等设计参考了
[FreeFallingSnow/SnowDesktop](https://github.com/FreeFallingSnow/SnowDesktop)。DeskTodo
未直接复制其源代码；由于 SnowDesktop 使用 GPL-3.0，发布或再分发时还应阅读其
[许可证文件](https://raw.githubusercontent.com/FreeFallingSnow/SnowDesktop/main/LICENSE)。

发布或再分发前，请同时检查主仓库与各子模块中的许可文件和声明。

## 多显示器热插拔修复说明

### 问题表现

第二个显示器断电时，Windows Explorer 可能会把该显示器上的桌面图标临时投射到主屏。旧逻辑在这个过渡阶段采集布局，可能把临时坐标保存成真实坐标。显示器重新连接后，原有显示器标识也可能发生变化，导致格子和桌面图标无法匹配回原屏幕。

另外，图标恢复曾在界面线程中使用重复等待，显示拓扑变化后 Direct2D 吸附线画刷还可能继续引用旧的渲染目标，这会造成蓝色吸附线消失、位置修正失效以及拖动卡顿。

### 当前实现

- 对显示器拓扑进行多次采样，只有拓扑稳定后才开始恢复。
- 按显示器稳定 ID、屏幕几何、工作区、DPI 和主副屏属性匹配布局。
- 离线显示器的普通桌面图标单独停放，不与格子逻辑归组混淆，也不会被临时主屏投射覆盖。
- 使用无等待的分批恢复队列，避免每个图标阻塞界面线程。
- 拖动期间保持交互拓扑不变，恢复任务在拖动结束后继续执行。
- 显示拓扑变化时释放并重建对应 Direct2D 渲染资源，保证吸附辅助线始终绑定当前渲染目标。
- 番茄时钟和 Todo 组件在拉伸后统一进行布局、字体和控件区域缩放，并在窗口尺寸变化后完整重绘。

这些改动解决了显示器重连后吸附线消失、位置恢复异常和拖动卡顿问题；真实硬件断电/上电场景仍建议在目标设备上进行一次验收。
