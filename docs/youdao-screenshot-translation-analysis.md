# 有道截图翻译静态分析与 DeskTodo 实现依据

分析日期：2026-10-28（Asia/Shanghai）。用户指定样本为
`E:\download\YoudaoDictSetup.exe`，版本 `11.3.24.0`。已确认其字节与先前下载的
官方 Windows 发行包完全相同，因此复用同一字节身份下的解包和 REA 静态证据。
仅读取、下载和解包，未安装或启动有道程序，未发送截图、登录请求或私有翻译请求。

## 样本与复现

- 用户指定本地样本：`E:\download\YoudaoDictSetup.exe`；121,513,152 字节，
  版本 `11.3.24.0`，SHA256
  `89ac34c178f1dea3b4471b365bb73c496f2439cf8b71a4bd157db9bfd89f3457`。
  Windows 签名检查返回 `Valid`。REA `open_binary` 直接打开该本地路径，
  返回相同 SHA256 和 PE/x86 身份，之后已 `close_binary`。
  [本地样本身份记录](../artifacts/youdao-rea/user-sample-identity.json)。
- 官网页面：[有道官方下载](https://cidian.youdao.com/download-app/)。
- 页面引用的 `app.03acb833.js` 给出默认 Windows 下载地址：
  `https://cidian.youdao.com/download/YoudaoDict.exe`。
- 2026-10-28 下载样本：121,513,152 字节，版本 `11.3.24.0`，SHA256
  `89ac34c178f1dea3b4471b365bb73c496f2439cf8b71a4bd157db9bfd89f3457`。
- Windows `Get-AuthenticodeSignature` 返回 `Valid`，签名主体为
  `NetEase Youdao Information Technology (Beijing) Co.,Ltd.`。
- 使用本机 Bandizip `bz.exe` 静态识别为 NSIS，提取其 `$PLUGINSDIR/dict.7z`，
  再提取相关 EXE、XML 和结果页 JavaScript。没有执行安装包内的程序。
- 可复现的 XML/字节字符串检查脚本：
  [inspect_reference.py](../artifacts/youdao-rea/inspect_reference.py)。
  输入样本、提取文件和完整 REA 图保存在本地忽略目录中，不随源码分发。

## 已观察到的结构和交互

| 结论 | 静态证据 | 结论边界 |
| --- | --- | --- |
| 截图 UI 有独立原生进程 | 包内 `11.3.24.0/YdScreenCapture.exe`；PE32/x86；其 SHA256 为 `f84667a61e37af32dc9af6de19ee60a7a4a65aae33b1523b5aa61ab94fa12b6b` | 文件存在和架构已确认；没有观察启动时机 |
| 支持原文/译文对照、交换语言、再次截图、提取文字和保存译图 | `skins/screencap_setwnd_layout.xml` 中的 `ContrastMode`、`OriTransSwitch`、`ExchangeLang`、`RestartScreenCap`、`EditText`、`SaveScreenTransImage` 等控件声明；截图 EXE 中也有对应字节字符串 | 证实发行资源提供这些入口；未观察每个按钮的实际事件调用 |
| 截图语言选择包含自动检测和十种语言选项 | `screencap_langwnd_layout.xml` 声明 `auto`、`en`、`zh-CHS`、`zh-CHT`、`ja`、`ko`、`fr`、`de`、`ru`、`es`、`pt` | 这是该版本截图 UI 的声明列表，不等于服务的完整语言列表 |
| 截图进程具备位图捕获、剪贴板和 HTTPS 请求所需接口 | REA 导入表包括 `BitBlt`、`OpenClipboard`、`SetClipboardData`、`WinHttpConnect`、`WinHttpOpenRequest`、`WinHttpSendRequest`、`WinHttpReceiveResponse` 等 | 导入表不证明接口在某次执行中被调用，也不证明所有截图都上传 |
| 截图和主程序之间存在进程桥接设计线索 | 截图 EXE 字符串包含 `ScreenCaptureIpc`、`IScreenCaptureHostBridge`、`CChildProcessScreenCaptureHostBridge`、`CScreenCaptureChildIpcWnd` | 只能推断存在相关 IPC 设计，消息协议及运行时调用链未恢复 |
| OCR 有独立程序与多类识别资源 | 包清单包含 `YoudaoOcr.exe`、`youdao_ocr_lib.dll`、`recognition/model`、`model_jp`、`model_latin` 及对应 label | 文件名提示中文/日文/拉丁识别资源，未解析模型格式或测试准确率 |
| 截图翻译结果可以进入通用翻译界面 | 结果页 `1038.js` 出现 `screenshottrans`、`screenshot_trans_detail` 场景标记，以及源/目标语言、关键词等参数处理；REA 对该文件完成 inert AST 解析，恢复 22 个模块、130 个 findings、0 个解析失败 | 117 个其他 JS 源文件未进入 REA 语义分析；动态调用和具体渲染行为仍未知 |

上述 XML 控件及文件 SHA256 的机器可读记录见
[reference-observations.json](../artifacts/youdao-rea/reference-observations.json)。
原生字节字符串记录保留文件偏移和编码，见
[static-feature-strings.json](../artifacts/youdao-rea/static-feature-strings.json)。
这些字符串是独立字节扫描结果，不冒充反编译或 REA 交叉引用结果。

## 私有接口线索及分析边界

截图 EXE 的 UTF-16LE 字节中可见以下 URL 模板：

- `https://ocrtran.youdao.com/ocr/imgtranocr`，模板参数含源/目标语言、
  `clientele=deskdict`、`salt`、`sign` 和 `isRender=true`。
- `https://dict.youdao.com/dicttranslate`，模板参数含源/目标语言、签名与时间信息。
- 另有 LaTeX、截图用量和 AI 总结相关路径。

这些模板只能证明样本中存在相关字符串。未验证接口当前可用性、请求体、签名算法、
账户权限、免费次数或返回结构。`isRender=true` 与保存译图/切换原译文控件共同提示
有道可能支持译文回填图片，但本次没有实际运行确认。

用户要求是在 DeskTodo 内新建自己的截图翻译模块，并参考有道的功能。
用户没有选择接入有道云。上述内部接口只是样本事实，不作为集成接口，
也不解密、提取或复用客户端密钥。实现边界不包含复制有道代码、模型或界面素材。

## 独立实现可参考的交互和模块拆分

建议沿用已观察到的操作结构：**框选截图 → 源/目标语言选择 → 后台 OCR →
翻译 → 原文/译文对照 → 复制、切换语言或重截**。桌面捕获、OCR 和翻译引擎
应各自独立，使截图翻译在 DeskTodo 内完成，不依赖安装或启动有道词典。

| 模块 | 输入和输出 | 独立实现要求 |
| --- | --- | --- |
| 截图入口 | 用户选区 → 自有位图快照 | 复用 DeskTodo 框选工具；关闭覆盖层后打开翻译窗口，快照由当前任务独立持有 |
| 多语言 OCR | 快照与源语言 → 文字、行或区域、位置 | 使用项目自身的本地 OCR 能力；按语言选择适配模型或 Windows OCR；报告缺失语言包；不能只把中英文 OCR 的结果标成多语言 |
| 翻译引擎 | OCR 文本、源语言、目标语言 → 译文 | 通过独立接口接入所选引擎，明确实际可用语言和配置状态；不把有道云设为已获用户选择的默认依赖 |
| 结果窗口 | 快照、OCR 结果、译文 → 可读对照 | 提供原文/译文、复制、交换语言、重试、重截；翻译失败时保留已经识别的原文 |
| 语言切换 | 源/目标语言变化 → 新任务 | 改源语言时重新识别；只改目标语言时复用 OCR；任务固定持有启动时的语言，防止结果与下拉框不一致 |
| 后台任务 | 识别/翻译请求 → 完成、失败或取消 | UI 保持响应；取消和关窗后旧结果不能写回新窗口；资源与任务生命周期清晰 |

这些是依据分析提出的自有模块设计，不能当成已经完成的应用行为或验收结果。
翻译引擎是否使用本地模型、可配置服务或其他后端，需要依据最终代码和实际可用环境
记录；不能仅凭这次静态分析宣称已获得免费、离线或不限次数的翻译能力。

图片内译文回填应作为有单独验收标准的显示功能：需要 OCR 区域坐标、背景处理、
译文排版与原图/译图切换。文本对照窗口不能描述成已实现原图回填。

后续验收应覆盖：至少两种非中文来源的识别与翻译、源/目标语言切换、网络或引擎
不可用时保留原文、取消/关窗无迟到结果、第二屏与不同 DPI 的截图入口，以及原有
截图保存、OCR、LaTeX 和贴图回归。本报告只记录分析，不替代最终代码的实测结果。

最终独立实现及真实联网验收见[截图翻译说明](screenshot-translation.md)。

## REA 证据与限制

| REA 操作 | Evidence ID | 保存记录 |
| --- | --- | --- |
| `inspect_desktop_application` | `ev_a25b66fe3894d35f6fbe300daeaad9f471785f785fc443467446da664eda08f8` | 完整本地 evidence bundle；154 条目录记录、3 个文件头 probe |
| `inspect_pe_imports` | `ev_513521928340bb69940e3a08b6e5a852bc2bb5aba2c404fc10cef11469cb2ac5` | [inspect_pe_imports.json](../artifacts/youdao-rea/inspect_pe_imports.json)：25 个 DLL、816 个导入 |
| `inspect_pe_resources` | `ev_a2cf4b45704a9699dc7689f9c20cad96e3578ef8698521b496b0f39fd64feb5a` | [inspect_pe_resources.json](../artifacts/youdao-rea/inspect_pe_resources.json)：87 个资源 |
| `analyze_javascript_application` | `ev_6fc94b00b83534ca4360fd2d226fb6827a78ef1708fd5fd141941d86b9ebefef` | 完整本地 evidence bundle；仅选 `1038.js` |
| 上述 JS 分析的 summary view | `ev_29214978af14b211d295ab89864a8f35491567d606ab36f3bee0ef6ad0e3fc79` | [javascript-summary.json](../artifacts/youdao-rea/javascript-summary.json) |

完整 REA 记录已导出至本地 `artifacts/youdao-rea/rea-evidence.json`，并压缩为
`rea-evidence.json.gz`；已调用 `close_binary` 结束会话。原生字符串搜索返回
`provider_unavailable`：当前 REA 未配置 Ghidra/IDA，Hopper 不支持当前 Windows
主机。为避免把工具缺失说成完成了反编译，本次只使用可用的 PE/目录/JS 静态能力，
没有安装额外反编译器。

仍未解决：截图进程和主进程的完整调用链、实际网络请求、私有签名、OCR/云端识别的
运行时选择，以及有道原图回填的具体算法。现有证据足以说明可参考的交互结构和独立
实现边界，不足以声明复刻了有道完整截图翻译引擎。
