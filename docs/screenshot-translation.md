# DeskTodo 独立截图翻译

实现与验证日期：2026-10-28（Asia/Shanghai）。

## 使用

按 `Ctrl + Shift + A` 框选，点击工具栏末尾的“文A”。窗口会在本机识别截图，
再把识别出的文字发送到所选翻译引擎。默认 Google 翻译，可改为 MyMemory；
无需有道账号、应用 ID 或密钥，也不需要安装或启动有道词典。

支持 22 种源/目标翻译语言、自动检测源语言、截图预览、原译文对照、编辑或粘贴
原文、交换语言、重新识别，以及分别复制原文、译文和双语对照。
只切换目标语言或翻译引擎时复用当前原文。切换源语言会使自动 OCR 缓存失效，
但不会删除用户粘贴、修正或从译文交换来的文字。点击“重新识别”才明确用截图替换
这些文字；识别失败或取消时仍保留原文。

## 实现边界

- `ScreenshotTool` 交出自有选区快照，先关闭截图覆盖层，再打开 `TranslationWindow`。
- `OcrService::RecognizeBitmapForLanguage` 按源语言选择本地 OCR。PP-OCRv6 medium
  识别中简繁、英、日和拉丁文字；其他文字使用对应 Windows OCR 语言包。未安装
  对应语言包时明确提示，用户也可以粘贴原文后直接翻译。
- `TranslationService::TranslateText` 独立处理网络请求和结果解析。仅文字离开本机，
  不上传图片；按 UTF-8 字节预算分段并保持代理项、换行和段间空白。
- Google 使用其免密钥客户端接口，接口或访问限制可能变化，不承诺公共服务 SLA。
  MyMemory 使用[公开 API](https://mymemory.translated.net/doc/spec.php)，单段上限 500
  字节；匿名[当前额度](https://mymemory.translated.net/doc/usagelimits.php)为每日 5000
  字符。服务不可用或额度耗尽时可手动切换，不会悄悄把文字转发给另一引擎。
- 偏好只保存引擎和语言到 `translation-preferences.json`，不保存截图或翻译文本。
  旧有道云凭据文件不再读取。当前没有捆绑离线机器翻译模型。
- 每次后台任务独立持有输入和结果。取消/关窗立即返回 UI，并阻止旧结果写回；
  已经执行的本地 OCR 或同步网络调用会在后台结束，不会被强制终止。
- 当前结果为文本对照，未实现把译文绘制回原图。

有道仅作为交互参考，用户提供的 `E:\download\YoudaoDictSetup.exe` 已经 REA 核对，
样本身份、静态证据和反编译范围限制见[分析报告](youdao-screenshot-translation-analysis.md)。
没有复制有道代码、模型、素材、内置凭据或私有接口实现。

## 验证

### 2026-10-28 界面更新

截图翻译窗口采用统一青绿与暖橙配色，包含圆角语言选择器、并排阅读卡片、
截图预览与独立状态栏。原文和译文顶端对齐，字体、段落行距、焦点与悬停状态统一；
加载、空白、失败均有对应提示。原生 RichEdit 支持长文本滚动，忙碌时原文仍可阅读。
增加 `Ctrl + Enter` 翻译，Esc 在忙碌时取消、空闲时关闭，并遵循 Windows 动画偏好。

本次 Release 构建与 24 项窗口交互检查通过；长文本、最小窗口、加载和失败四种
视觉场景已使用合成内容渲染检查，检查环境 DPI 为 150%。
样式约定见[截图翻译页面规范](../design-system/pages/screenshot-translation.md)。

| 检查 | 结果 |
| --- | --- |
| 正式 Release 主程序 | 构建通过，已正常替换并重新启动 |
| 原生回归 `DeskTodoNativeSmoke` | 141 项，0 失败 |
| 本地 OCR `DeskTodoOcrSmoke` | 16 项，0 失败，包含英文、日文、法文及缺失语言包行为 |
| 翻译契约 `DeskTodoTranslationSmoke` | 42 项通过，含分段、两种响应格式、取消、偏好、网络失败保留原文 |
| 窗口交互 `DeskTodoTranslationWindowSmoke` | 24 项通过，含预览渲染、原文编辑、缓存复用、语言交换、源语言改变保留手改文字、取消和关闭 |
| 截图 OCR 与交接 `DeskTodoScreenshotOcrSmoke` | 通过，保留原有 OCR/LaTeX 路径并验证截图位图交接 |
| 真实联网 `DeskTodoTranslationLiveSmoke --live` | 新建后台线程内，合成英文截图经本地 OCR 后由 Google 译成中文；英文到日文、Windows 本地检测语言后由 MyMemory 译成中文，全部成功 |
| 视觉与差异检查 | 合成窗口预览检查通过；`git diff --check` 通过 |

联网测试只发送合成测试句。窗口测试使用模拟翻译后端，需要真实 Windows 桌面
剪贴板权限。多屏真实硬件、所有 22 种语言和长期服务稳定性没有穷举验证。
本地预览保存在构建目录 `translation-preview-v2.png`，不作为线上翻译准确率证据。
