# DeskTodo

DeskTodo 是一款原生 Windows 桌面整理工具。它用可移动、可缩放的“格子”收纳桌面文件，并集成截图标注、OCR、局域网快传、番茄时钟、Todo 和黑洞动态效果。

当前主工程是 `DeskTodo.Next`，使用 C++17、Win32、Direct2D、WIC、Windows OCR、Winsock、OpenGL/D3D 桌面捕获等原生 Windows 技术构建，不依赖 .NET 运行时。

## 功能概览

### 桌面格子

- 创建、重命名、移动、缩放、折叠、锁定和解散多个格子。
- 支持图标视图与列表视图，可按名称或类型排序，也可在格子内搜索。
- 文件、文件夹、快捷方式和程序可以在桌面与格子之间自由拖入、拖出。
- 采用逻辑归组方式管理桌面项目，不为普通项目额外生成 `.lnk`，并抑制 Explorer 中的重复图标。
- 自动处理同名文件和文件夹，避免格子项目与桌面新建项目发生名称冲突。
- 支持文件夹映射：直接显示指定目录内容，不移动原目录文件。
- 支持布局吸附、辅助线、网格排列、滚动和多显示器物理像素坐标。
- 支持不同 DPI、多屏移动和显示器工作区边界修正。
- 布局可备份为 `.bak` 文件并按需还原。

### 截图、贴图与 OCR

- 任意区域框选，支持取消、贴图、保存 PNG、复制到剪贴板。
- 画笔、箭头、文字、矩形框标注。
- 可分别设置标注粗细、字体、字号和颜色。
- 工具栏使用图标显示，鼠标悬停时显示中文功能提示。
- 支持 Windows OCR 文本识别和 LaTeX 识别结果窗口。
- 贴图窗口支持缩放、复制、另存为和关闭。

### 局域网快传

- 电脑端显示二维码和临时访问地址，手机扫描后即可在浏览器选择文件。
- 支持图片、视频、文档等任意文件以及多文件连续上传。
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
- 内置番茄时钟和 Todo List，状态会在重启后恢复。
- 软件为单实例运行，重复启动会把命令发送给已经运行的实例。

## 系统要求

- Windows 10 或 Windows 11 x64
- 支持桌面合成的显卡驱动
- 构建时需要 Visual Studio 2022 或 Visual Studio 2026：
  - “使用 C++ 的桌面开发”工作负载
  - MSVC x64 工具链
  - Windows 10/11 SDK
  - CMake 3.15 或更高版本
- OCR 需要系统安装可用的中文或英文 Windows OCR 语言包
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

## 运行

```powershell
.\DeskTodo.Next\build\Release\DeskTodo.Next.exe
```

首次正常启动会更新桌面上的 `DeskTodo 启动.lnk`，快捷方式指向当前运行的正式 EXE。

程序主要通过格子标题栏、设置中心和系统托盘菜单操作。托盘菜单提供刷新、新建格子、一键整理、显示/隐藏、锁定、截图、文件传送、布局备份/还原、黑洞开关和退出。

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl + Shift + N` | 新建格子 |
| `Ctrl + Shift + L` | 锁定或解锁全部格子 |
| `Ctrl + Shift + A` | 开始截图 |
| `F5` | 刷新格子 |
| `Ctrl + Shift + Q` | 退出 DeskTodo |
| `Ctrl + Alt + D` | 通过桌面快捷方式启动 DeskTodo |

## 命令行

DeskTodo 为单实例程序。下列命令既可用于首次启动，也可发送给正在运行的实例：

```powershell
DeskTodo.Next.exe --show
DeskTodo.Next.exe --new-fence
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
├─ Fences\                     格子存储目录
├─ todo-widget.dat             Todo 状态
├─ pomodoro-widget.dat         番茄时钟状态
├─ lan-transfer-folder.txt     快传接收目录
└─ Screenshots\                图片目录不可用时的截图回退目录
```

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

- 配置读写、布局、吸附和多显示器坐标
- 文件名冲突处理和格子逻辑归组
- 被占用程序、文件夹和快捷方式的格子拖放
- 黑洞参数校验和进程控制
- OCR 基础识别
- 截图工具栏、格子菜单、桌面拖放和悬浮提示的交互探针

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
- 程序采用 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`，内部布局保存物理像素坐标。
- 桌面项目通过 Windows Shell 和逻辑归组状态同步，避免格子内外同时出现相同图标。
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

发布或再分发前，请同时检查主仓库与各子模块中的许可文件和声明。
