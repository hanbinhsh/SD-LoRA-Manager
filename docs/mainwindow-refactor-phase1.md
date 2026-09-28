# MainWindow 第一阶段解耦

本阶段先抽出无主窗口依赖的规则、文件操作和数据结构，不拆分页面，不改变网络状态机。
固定布局仍由原有 UI 文件管理。

## 模块边界

| 模块 | 职责 | 调用约束 |
| --- | --- | --- |
| core/modeltypes.h | 模型、图片、用户信息及同步任务的数据结构 | 不依赖 QWidget 或 item roles |
| core/metadatascantypes.h | 元信息扫描与健康检查结果 | 不依赖下载页 |
| core/pathmodels.h | 路径及启用状态 | 设置页与路径弹窗共用 |
| core/modelupdateinfo.h | 版本更新任务的纯数据 | 缓存工具不再依赖页面控件结构 |
| utils/downloadcache | 下载缓存编解码、来源文件检查及原子保存 | 后台读取，无 QWidget 依赖，保留完整 latestVersionJson |
| utils/downloadstatus | 分类、按钮动作、失败重试和持久化状态判断 | MainWindow、DownloadsPage、DownloadManager 共用 |
| utils/modelmetadatacodec | 读取模型 JSON、解码显示数据、合并完整模型对象、预览路径 | 不修改输入 JSON，不更新 currentMeta |
| utils/modelscanner | 目录扫描、模型列表摘要及缓存复用 | 由主窗口在后台调用，返回纯数据 |
| utils/modelimagematcher | 模型候选名/摘要、图片匹配、使用统计 | 候选构造有文件 I/O，匹配判定无 I/O |
| utils/gallerymetadata | 图片解析到图库数据、解析版本 | 复用已有图片元信息解析器 |
| utils/metadatainspection | 元信息分类、健康检查 | 文件操作，不访问控件 |
| utils/civarchiveparser | CivArchive 页面/JSON 解码及查询 URL | 不发送请求，不处理认证 |
| utils/previewimagestore | 预览图参数文本、PNG 元信息写入、原始字节回退 | 后台文件操作，不持有窗口 |
| utils/modelfilter | 搜索、底模和类型判定 | 主窗口将 item roles 转为 Record |
| utils/pathutils、fileutils | 路径列表、启用状态、文件重名规避 | 主窗口和 DownloadManager 共用 |
| utils/imagepresentation | 图标、圆角、模糊背景和占位绘制 | QPixmap/QGraphicsScene 操作仍在 GUI 线程 |
| utils/tagutils | 提示词拆分、清洗、图库键规范化 | 保留图库与工具页原有规范化差异 |

## 已合并的分歧

- 图库筛选和使用统计共同调用 ModelImageMatcher::matches，不再各维护一份摘要/名称回退规则。
- LoRA 候选同时包含磁盘文件名与 safetensors 内部名称，覆盖 ComfyUI 与 A1111 的不同来源。
- 名称去扩展名只识别模型扩展名，不再将模型名中的 .v1 等内容当成扩展名去掉。
- 主页与侧边栏使用同一个基础搜索判定，均包含 Civitai 名称、作者、模型标签、用户标签、备注和自定义触发词。
- 本地读取、详情同步、批量同步共用图片 metadata 解码，兼容字符串和数值形式的参数。
- readLocalJson 不再隐式改写 currentMeta；需要切换当前模型的调用点明确赋值。
- MainWindow 与 DownloadManager 使用同一个唯一下载路径生成函数。
- 已有预览图改写与新下载预览图共用 PNG 写入实现。

以上解码只生成展示/匹配数据。Civitai 原始版本对象仍用于保存，不通过 ModelMeta 重建 API JSON。
CivArchive 适配规则沿用原实现，本轮只迁移解析层。

## 验证

无界面测试开关：

```powershell
cmake -S . -B build/codex-model-startup-verified -DBUILD_MODEL_LIST_CACHE_TESTS=ON -DBUILD_CORE_UTIL_TESTS=ON
cmake --build build/codex-model-startup-verified --parallel 4
ctest --test-dir build/codex-model-startup-verified --output-on-failure
```

需先进入已配置 Qt 6.11.1、MSVC、OpenSSL 的构建环境。
测试只读写 QTemporaryDir，不读取用户模型库、不启动应用、不访问网络。

覆盖：完整 JSON 字段保留、重复读取、图片参数类型、缓存冷/热扫描、无效缓存、
LoRA/Checkpoint 匹配、ComfyUI 严格模式回退开关、统计与匹配一致性、
预览图参数写入读取、CivArchive 嵌入 JSON、路径与搜索、元信息检查。

本轮不验证实际窗口交互和视觉效果；UI 布局未修改。

## 下载页进入速度与重复逻辑收尾

- 原先首次进入下载页会在 GUI 线程读取整个 downloads.json，并一次创建全部卡片；每条记录还会重复排序、重新插入整组控件、全表筛选及更新选中状态。
- 现在由后台读取缓存和检查源文件；页面先切换并显示加载提示，GUI 线程按时间预算分批创建卡片（每批最多 8 条、创建预算 8ms，批次间让出事件循环）。排序和按钮状态每批统一更新，已正确排序的卡片不再从布局移除重插。
- 状态更新只重算当前卡片的搜索可见性；全量筛选仅在搜索词改变时执行。批量忽略、清空完成合并布局刷新与保存，不再逐条写整个缓存。
- 卡片预览使用 QImageReader 请求缩略图尺寸，后台缩放裁剪，页面只处理小图并做一次圆角绘制。
- 删除 DownloadManager 纯页面查询/排序转发、主窗口重复的选中状态扫描和废弃控件指针接口。页面自行连接分类切换、全选和取消选择，不再依赖侧边栏模型选择。
- 普通 metadata 更新与手动 CivArchive 补充共用批次初始化、确认弹窗和任务构造，不改变各来源请求/响应和完整 JSON 保存逻辑。
- 缓存恢复具有幂等入口；更新检测等待恢复完成并按路径重新查找模型，避免持有旧 item 指针。恢复期间的新状态优先于缓存，已移除任务不被恢复覆盖。
- 中途退出或读取失败不会将残缺/空列表写回原缓存。损坏缓存保持原文件以便恢复，当前会话暂停覆盖该文件。Hash 计算失败不再被当作计算中而从缓存漏掉。

额外离屏回归测试（不打开主程序窗口、不截屏、不发网络请求）：

```powershell
cmake -S . -B build/codex-model-startup-verified -DBUILD_DOWNLOAD_RESTORE_TESTS=ON
cmake --build build/codex-model-startup-verified --parallel 4
ctest --test-dir build/codex-model-startup-verified --output-on-failure
```

download_restore_test 使用临时下载缓存，测试可执行文件单独放在 build/tests 子目录，隔离主程序的 config。
覆盖分批恢复/事件循环响应、重复进入、恢复中更新/移除任务、分类排序/搜索选择、恢复中退出、损坏与缺失缓存。
core_utils_downloads 覆盖完整 API JSON 往返、重复记录、来源与占位状态、检查/下载失败分流和 Hash 失败缓存。
当前共 10 项测试；真实数据量下的页面切换速度与视觉表现仍由实际使用验证。

## 留待下一阶段

- 从 MainWindow 迁移元信息同步、更新检测和预览图下载的网络状态机。
- 提取模型库状态所有者，再逐步降低页面对 QListWidget/item roles 的依赖。
- 迁移用户图库缓存、扫描调度及筛选状态的生命周期管理。
- 独立配置/收藏/用户备注存储服务。

不将 MainWindow 私有成员直接暴露给上述模块；后续继续通过明确的数据输入和结果/信号输出通信。
