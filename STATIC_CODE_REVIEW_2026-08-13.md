# SD LoRA Manager 静态代码审查报告

审查日期：2026-08-13  
审查方式：初始报告仅做静态检查；2026-08-13 随后的第一轮修复未启动应用或截图验证。  
审查范围：项目中的 C++、头文件、Qt UI、QSS、Python 脚本和 CMake 配置，共盘点 75 个相关文件；重点追踪图库、模型详情、元数据、下载、工具页、持久化和同步链路。

## 第一轮修复状态

以下状态用于后续审核追踪，原始问题描述仍保留，不删除历史依据。

| 状态 | 问题 ID | 本轮处理 |
|---|---|---|
| 已修复 | G-01、G-02、G-03、G-04、G-06、G-07、G-09、G-10、G-13 | 固定图库网格；筛选后按首个可见项整行定位；扫描与缩略图增加 generation；失败缩略图有限重试；显式区分全局图库；加入 WebP；统一 Tag 规范化；解析失败可重试；结果分批插入。 |
| 部分修复 | G-05、G-12 | 图库缩略图已使用专用任务前缀，避免遍历其它模型视图；缓存写入已改为原子提交。O(1) item 索引和启动时后台加载仍待处理。 |
| 已修复 | M-02、M-03、M-04、M-05、M-06、M-07、M-09、M-10 | API 回包按绝对路径定位；预览回写同时核对目录与文件名；先构造完整 metadata 再写 roles；严格校验 JSON；多文件按 hash/文件名/primary 选择；metadata 原子写入；Hash 改为串行活动任务并只保留最新待处理模型；预览图删除和封面交换改为可回滚事务。 |
| 已修复 | D-01、D-02、D-03、D-04、D-05、D-06、D-07 | Hash 读错误返回失败；检查下载写入；空 Hash 和空文件禁止安装；SHA256 后台校验；覆盖前备份且失败回滚；依赖检查提前；下载队列按模型路径去重。 |
| 已修复 | C-01、C-03、C-04、C-05、C-06 | 设置默认值修正；设置、收藏、颜色、对话、模板、图库缓存和翻译 CSV 使用原子写入并检查提交。 |
| 已修复 | P-01、P-07 | PNG 文本块增加长度和剩余数据边界；ComfyUI prompt/workflow/parameters 改为字段合并，不再因某一来源提前成功而丢失 Checkpoint/LoRA。缓存解析版本提升至 7。 |
| 部分修复 | P-02 | 压缩 iTXt 不再误当 UTF-8 乱码解析，但 zTXt/压缩 iTXt 的受限解压支持仍待实现。 |
| 已修复 | S-12、U-01 | “停止监控文件夹”不再发送远端删除命令；重复 UI objectName 已改为语义化唯一名称。 |

未列入上表的项目仍按原优先级视为待处理；其中 Collections 路径身份迁移、预览下载取消域、更新检测批次状态、退出等待和同步协议需要单独设计，未在本轮冒险改动。

## 优先级说明

| 级别 | 含义 |
|---|---|
| P0 | 可能直接造成用户文件丢失或严重安全后果，应最先处理 |
| P1 | 可能造成崩溃、长时间卡死、错误模型数据、状态机失效或配置损坏 |
| P2 | 明显功能错误、性能问题或跨页面不一致 |
| P3 | 边缘问题、可维护性风险或低概率兼容问题 |

置信度“确认”表示从当前控制流可直接推出；“高”表示与已观察现象高度吻合；“中”表示依赖特定文件、时序或外部数据，需要运行验证。

## 一、图库与缩略图

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| G-01 | P1 | 高 | [windows/mainwindow.cpp:8452](windows/mainwindow.cpp#L8452)、[windows/mainwindow.ui:2054](windows/mainwindow.ui#L2054) | Tag 筛选只逐项 `setHidden()`；`IconMode + Adjust` 列表没有固定 `gridSize/uniformItemSizes`，筛选后也没有延迟布局刷新、滚动位置校正和缩略图优先级重算。 | 与截图高度吻合：筛选后旧几何/滚动偏移残留，左上角出现被 viewport 裁切的“悬浮”缩略图或明显分层；随后加载的图标可能继续按旧位置回写。 | 筛选结束后统一触发延迟布局，校正滚动值，再调度可见缩略图；为图库设稳定网格尺寸，并以当前可见首项作为滚动锚点。 |
| G-02 | P1 | 确认 | [windows/mainwindow.cpp:7994](windows/mainwindow.cpp#L7994)、[windows/mainwindow.cpp:8282](windows/mainwindow.cpp#L8282) | 每次图库扫描都创建新 watcher，但没有扫描代次 token，也没有取消/丢弃旧扫描结果。 | 快速切换模型、全局图库或连续刷新时，先启动但后完成的旧扫描会把旧模型图片追加到当前图库，并覆盖状态栏和缓存。 | 为扫描增加 generation token；回调先核对 token、当前模式和当前模型路径。 |
| G-03 | P1 | 高 | [windows/mainwindow.cpp:7912](windows/mainwindow.cpp#L7912)、[windows/mainwindow.cpp:7985](windows/mainwindow.cpp#L7985)、[utils/imageloader.h:18](utils/imageloader.h#L18) | 重置缩略图加载仅清空集合和停止 timer，已在线程池运行的任务不能取消；任务 ID 只含图片路径，不含图库代次。 | 旧图库任务在新图库建立后仍可回写同路径项，造成错误缩略图、额外重绘和筛选后布局抖动。 | 任务 ID 加入图库 generation；回调仅接受当前 generation。 |
| G-04 | P2 | 确认 | [windows/mainwindow.cpp:6280](windows/mainwindow.cpp#L6280)、[windows/mainwindow.cpp:7988](windows/mainwindow.cpp#L7988) | `onIconLoaded()` 收到空图后立即返回，没有从 `queuedUserImageThumbPaths` 移除路径。 | 一次瞬时读取失败后，该缩略图在当前会话内永久保持占位符，滚动回来也不会重试。 | 空图回调也必须完成队列清理，并做有限次数重试/退避。 |
| G-05 | P2 | 确认 | [windows/mainwindow.cpp:6315](windows/mainwindow.cpp#L6315)、[windows/mainwindow.cpp:6351](windows/mainwindow.cpp#L6351)、[windows/mainwindow.cpp:6448](windows/mainwindow.cpp#L6448) | 无前缀任务被同时当成 Home 和 Sidebar 任务；图库缩略图使用原始路径作为 ID，因此每张图回调都会遍历主页、模型列表、Collections、详情缩略图和返图列表。 | 图库大量缩略图显示时，GUI 线程执行多组 O(N) 扫描，造成滚动和筛选卡顿。 | 为用户图库增加专用任务前缀和 O(1) 路径到 item 映射。 |
| G-06 | P2 | 确认 | [windows/mainwindow.cpp:7878](windows/mainwindow.cpp#L7878)、[windows/mainwindow.cpp:8489](windows/mainwindow.cpp#L8489) | 进入全局图库只调用 `clearSelection()`，没有清除 `currentItem/currentIndex`；手动刷新却根据 `currentItem()` 决定是否扫描某个模型。 | 在“所有本地返图”页面点击刷新，可能意外只扫描上次当前模型。 | 使用显式图库模式状态，不以列表 current item 推断全局/模型模式。 |
| G-07 | P2 | 确认 | [windows/mainwindow.cpp:8192](windows/mainwindow.cpp#L8192) | 本地图库扫描只包含 PNG/JPG/JPEG；项目其它图片入口已经支持 WebP。 | ComfyUI/WebUI 生成的 WebP 不进入返图图库、Tag 统计和使用分析。 | 统一复用项目图片扩展名 helper，至少加入 WebP。 |
| G-08 | P2 | 高 | [windows/mainwindow.cpp:8332](windows/mainwindow.cpp#L8332) | 非“仅当前图片”模式统计所有 list item，没有跳过当前 Tag 筛选已隐藏的图片。 | 页面显示“筛选后图片”，TagFlow 数量却仍来自筛选前全集；用户继续点击 Tag 时结果和数量不一致。 | 明确“全集统计/当前可见统计”语义；若是当前结果，应跳过 hidden item。 |
| G-09 | P2 | 确认 | [windows/mainwindow.cpp:8340](windows/mainwindow.cpp#L8340)、[windows/mainwindow.cpp:8467](windows/mainwindow.cpp#L8467) | 单图去重使用大小写敏感 `QSet<QString>`，筛选却大小写不敏感；空格/下划线也没有统一规范化。 | `Tag` 与 `tag`、`spoken heart` 与 `spoken_heart` 可能分开计数但筛选时视为相同，数量和筛选结果不一致。 | 计数、去重和筛选共用同一个 normalized key，同时保留首个显示文本。 |
| G-10 | P2 | 确认 | [windows/mainwindow.cpp:1860](windows/mainwindow.cpp#L1860)、[windows/mainwindow.cpp:8206](windows/mainwindow.cpp#L8206) | 解析前先写入当前 `parserVersion`；即使图片暂时不可读或解析失败，也会缓存为“已由最新版解析”。 | 文件写入尚未结束、网络盘短暂失败或解析器异常后，之后扫描不会重试，图片长期显示 Unknown/No prompt。 | 仅在成功解析或明确判定“无元数据”后写入成功版本；失败状态单独记录并允许重试。 |
| G-11 | P2 | 确认 | [windows/mainwindow.cpp:12025](windows/mainwindow.cpp#L12025)、[windows/mainwindow.cpp:8291](windows/mainwindow.cpp#L8291) | 扫描只合并新/更新缓存，从不移除已删除、移动或已移出启用路径的图片。 | `user_gallery_cache.json` 持续膨胀；Tag Picker、使用次数和模型使用分析可能统计已不存在图片。 | 每次全量扫描生成有效路径集合，并清理不再存在或不属于启用根目录的缓存项。 |
| G-12 | P1 | 确认 | [windows/mainwindow.cpp:12025](windows/mainwindow.cpp#L12025)、[windows/mainwindow.cpp:12054](windows/mainwindow.cpp#L12054) | 启动时在主线程读取、解析并重建整个图库缓存；扫描结束后在主线程重建完整 JSON 并用普通 QFile 覆盖写。 | 大图库启动/扫描完成时明显假死；异常退出或磁盘写失败可能损坏整个缓存。 | 后台解析/序列化，主线程只交换结果；写入改用 `QSaveFile`。 |
| G-13 | P2 | 确认 | [windows/mainwindow.cpp:8301](windows/mainwindow.cpp#L8301) | 扫描结束后一次性在 GUI 线程为全部图片创建 `QListWidgetItem`；源码注释也承认大量数据会卡顿。 | 扫描进度结束后仍长时间冻结，图片越多越明显。 | 分批插入或改用虚拟化 model/view；每批让出事件循环。 |

## 二、模型身份、详情与元数据

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| M-01 | P1 | 确认 | [windows/mainwindow.cpp:11588](windows/mainwindow.cpp#L11588) | Collections 刷新使用 `BaseName -> QListWidgetItem` 的 QMap；不同根目录下同名模型会互相覆盖。 | 同名模型在 Collections 中消失、显示成另一路径的模型，右键/详情跳到错误文件。 | Collections 持久化和映射统一改为绝对路径或稳定模型 ID，显示名仅用于 UI。 |
| M-02 | P1 | 确认 | [windows/mainwindow.cpp:5741](windows/mainwindow.cpp#L5741)、[windows/mainwindow.cpp:5768](windows/mainwindow.cpp#L5768)、[windows/mainwindow.cpp:5863](windows/mainwindow.cpp#L5863) | 元数据 API 回调多次仅按 `localBaseName` 查找第一个 item，没有核对绝对路径。 | 同名模型同步后，另一个模型的名称、ID、Hash、触发词、Local 状态被覆盖。 | 所有异步请求携带并按绝对 `filePath` 定位 item。 |
| M-03 | P1 | 确认 | [windows/mainwindow.cpp:5909](windows/mainwindow.cpp#L5909) | 下载预览图后的 UI 更新也按 basename 匹配 modelList、Collections 和详情。 | 同名模型收到错误封面，或当前详情被另一个目录的同名模型刷新。 | `applyDownloadedPreviewToUi()` 接收模型绝对路径，并只用路径匹配。 |
| M-04 | P2 | 确认 | [windows/mainwindow.cpp:5749](windows/mainwindow.cpp#L5749)、[windows/mainwindow.cpp:5782](windows/mainwindow.cpp#L5782) | `ROLE_MODEL_TYPE` 在 `meta.type` 从 JSON 赋值之前写入 item。 | 新同步模型的类型暂为空，直到重扫；Checkpoint/LoRA 判断、分类和图库匹配可能临时走错分支。 | 先完整构造 `ModelMeta`，再一次性更新所有 roles。 |
| M-05 | P1 | 确认 | [windows/mainwindow.cpp:5512](windows/mainwindow.cpp#L5512) | `readLocalJson()` 没有检查 `QJsonParseError` 或文档类型，空/损坏 JSON 也走到 `return true`。 | 损坏 metadata 被当成有效本地元数据，自动同步被抑制，详情显示空字段或错误“本地模型”状态。 | 严格校验 JSON；失败返回 false 并进入“JSON 异常”状态。 |
| M-06 | P1 | 高 | [windows/mainwindow.cpp:5601](windows/mainwindow.cpp#L5601)、[windows/mainwindow.cpp:5791](windows/mainwindow.cpp#L5791) | 版本有多个 files 时固定读取 `files[0]`，没有按本地文件名、primary 标志或 SHA256 选择实际文件。 | 同一版本的 fp16/pruned/不同格式文件可能关联错误 Hash、大小和服务端文件名，进而影响更新检测与重复判断。 | 建立“本地文件 -> API file”匹配 helper，优先 Hash，其次精确文件名，再用 primary。 |
| M-07 | P1 | 确认 | [windows/mainwindow.cpp:6002](windows/mainwindow.cpp#L6002) | 核心模型 metadata 使用普通 QFile 直接覆盖，且忽略 `write()` 结果。 | 崩溃、断电或磁盘满时 JSON 被截断；下次又会触发 M-05。 | 使用 `QSaveFile`，检查写入长度和 `commit()`。 |
| M-08 | P1 | 确认 | [windows/mainwindow.cpp:3336](windows/mainwindow.cpp#L3336)、[windows/mainwindow.cpp:3771](windows/mainwindow.cpp#L3771)、[windows/mainwindow.cpp:7731](windows/mainwindow.cpp#L7731) | 切换/清空详情会 abort 所有标记为 `isGalleryDownload` 的 reply 并清空全局 `downloadQueue`；批量元信息同步预览图也使用同一标记和队列。 | 用户切换模型时会取消下载页正在进行的批量预览同步；已排队计数项被直接丢弃，进度可能永久停在“剩余 N 个”。 | 详情页与批量任务使用独立队列、owner ID 和取消域；清队列时逐项完成/取消计数。 |
| M-09 | P1 | 确认 | [windows/mainwindow.cpp:6116](windows/mainwindow.cpp#L6116)、[windows/mainwindow.cpp:6619](windows/mainwindow.cpp#L6619) | 单个共享 `QFutureWatcher` 运行时仍可再次 `setFuture()`，而回调从可变的 `currentProcessingPath/current_processing_file` 读取上下文。 | 快速从模型 A 切到 B 时，A 的 Hash 结果可能按 B 的路径发起请求并写错 metadata；也可能丢失某次 finished 信号对应关系。 | 同时只运行一个 Hash；活动任务上下文不可变，只保留最后一个待处理模型，旧结果按绝对路径丢弃。 |
| M-10 | P0 | 确认 | [windows/mainwindow.cpp:5333](windows/mainwindow.cpp#L5333)、[windows/mainwindow.cpp:5340](windows/mainwindow.cpp#L5340)、[windows/mainwindow.cpp:5369](windows/mainwindow.cpp#L5369) | 编辑预览图删除、改名和封面交换忽略 `remove/rename` 返回值，且部分流程先删除目标再改名。 | 文件锁定、跨卷、权限不足时可能丢失封面或只完成一半操作，内存 metadata 与磁盘文件不一致。 | 使用可回滚的临时文件事务；每一步失败都停止并恢复原文件。 |
| M-11 | P2 | 确认 | [windows/mainwindow.cpp:10152](windows/mainwindow.cpp#L10152) | 模型文件更新完成后遍历图片，但首个有效图片入队后立即 `break`。 | 新版本只下载封面，不补齐其余预览图，与单模型刷新/批量元信息同步行为不一致。 | 复用统一的 `syncPreviewImagesFromMetadata()`，由策略决定是否下载全部。 |
| M-12 | P1 | 确认 | [windows/mainwindow.cpp:10028](windows/mainwindow.cpp#L10028) | 缺失 metadata 且无法可靠匹配本地版本时，直接选 `modelVersions.first()`。 | 本地文件可能被写入另一个版本的 metadata，造成图片数量、触发词、版本号和更新结果错误。 | 无可靠 versionId/Hash/文件名匹配时标记“无法判断”，不要猜第一个版本。 |
| M-13 | P1 | 确认 | [windows/mainwindow.cpp:828](windows/mainwindow.cpp#L828)、[windows/mainwindow.cpp:7406](windows/mainwindow.cpp#L7406) | 原图虽在后台读取，但 QPixmap 转换、NSFW 模糊和大背景 `QGraphicsBlurEffect` 都在 GUI 回调执行；关闭 downscale 时处理完整大图。 | 切换高分辨率模型封面、改变详情高度或窗口尺寸时明显卡顿/假死。 | 后台完成 QImage 缩放与模糊，GUI 线程只做最终 QPixmap 赋值。 |
| M-14 | P2 | 中 | [windows/mainwindow.cpp:7364](windows/mainwindow.cpp#L7364) | 快速切图只对共享 watcher 调 `cancel()`，随后立即 `setFuture()`；QtConcurrent 工作本身仍继续。 | 连续切模型会积压无用高分辨率解码，抢占线程池；旧任务虽有路径校验，但仍消耗 CPU/IO。 | 使用独立 token/可取消任务，或限制为单一串行图片解码队列。 |
| M-15 | P2 | 确认 | [windows/mainwindow.cpp:5461](windows/mainwindow.cpp#L5461) | 将完整 modelRoot 的所有字段（包括 `modelVersions`）复制进每个版本 JSON 的 `model` 对象。 | 每个本地版本文件重复存储整模型的全部版本和图片，metadata 体积迅速膨胀，扫描/启动变慢；本地 schema 也不再等同 API 任一原始对象。 | 若必须完整保留 API，单独保存原始 modelRoot 命名空间/文件，并在版本视图中只保存引用；避免删除任何原字段。 |

## 三、版本更新、下载与元信息扫描

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| D-01 | P1 | 确认 | [utils/fileutils.cpp:13](utils/fileutils.cpp#L13) | SHA256 循环遇到 `read() <= 0` 就退出，但不检查 `QFile::error()`；中途 I/O 错误仍返回已读取前缀的 Hash。 | 产生看似合法但错误的 SHA256，导致更新检测 404、重复模型误判或校验错误。 | 只有正常 EOF 才返回 Hash；任何读错误返回失败并带错误文本。 |
| D-02 | P1 | 确认 | [pages/downloadmanager.cpp:572](pages/downloadmanager.cpp#L572) | 下载写入 `.part` 时忽略 `QFile::write()` 返回值和文件错误。 | 磁盘满、权限或设备故障时生成截断文件，进度仍继续并可能进入完成流程。 | 检查每次写入长度；失败立即 abort、标错并保留原模型。 |
| D-03 | P1 | 确认 | [pages/downloadmanager.cpp:611](pages/downloadmanager.cpp#L611) | 预期 Hash 存在时，只有“实际 Hash 非空且不相等”才失败；实际 Hash 为空反而通过。 | Hash 计算失败的下载被当成校验成功。 | `actual.isEmpty()` 必须是校验失败。 |
| D-04 | P1 | 确认 | [pages/downloadmanager.cpp:611](pages/downloadmanager.cpp#L611) | 大模型 SHA256 在 `QNetworkReply::finished` 的 GUI 槽中同步计算。 | 下载完成后数 GB 模型导致界面长时间假死。 | Hash 放到后台，下载状态改为“校验中”。 |
| D-05 | P0 | 确认 | [pages/downloadmanager.cpp:627](pages/downloadmanager.cpp#L627) | 覆盖模式先删除原目标文件，再尝试把 `.part` rename 为目标。 | rename 因权限、杀软、跨设备或文件锁失败时，旧模型已永久删除。 | 原文件先原子改名为备份，成功替换后再删备份；失败则回滚。 |
| D-06 | P2 | 确认 | [pages/downloadmanager.cpp:549](pages/downloadmanager.cpp#L549) | 在检查网络管理器/请求回调之前已创建并打开 `m_activeFile`；初始化缺失分支没有关闭和清理它。 | 留下打开的 `.part`、句柄泄漏，后续任务可能覆盖 `m_activeFile` 指针。 | 先校验依赖再创建文件；所有 early return 走统一 cleanup。 |
| D-07 | P2 | 确认 | [pages/downloadmanager.cpp:454](pages/downloadmanager.cpp#L454)、[pages/downloadmanager.cpp:473](pages/downloadmanager.cpp#L473) | 入队只根据状态文本过滤一次，没有对 active/queued target 做结构化去重。 | 多次点击或状态文案变化时，同一模型可重复入队，重复弹覆盖策略、重复下载。 | 维护 queued/active filePath 集合，入队接口幂等。 |
| D-08 | P1 | 确认 | [pages/downloadmanager.cpp:306](pages/downloadmanager.cpp#L306) | 退出时同步等待每个预览 QFutureWatcher 完成；取消并不能停止正在解码的 QtConcurrent 任务。 | 关闭软件时长时间无响应，历史上的退出崩溃/异常更难定位。 | 结果通过 token 丢弃，任务放到可在对象销毁后安全完成的池；避免 GUI 析构逐个 wait。 |
| D-09 | P1 | 高 | [windows/mainwindow.cpp:8991](windows/mainwindow.cpp#L8991)、[windows/mainwindow.cpp:9103](windows/mainwindow.cpp#L9103) | 新一轮检查直接把共享 active 计数清零，但上一轮 watcher/reply 未取消；旧 Hash 回调在 token 判断前先递减新一轮计数。 | 连续点击检查时并发限制、完成计数和按钮恢复时机错乱；旧请求仍占 API 配额，增加 429。 | 每批维护独立 context/计数；旧回调不得修改新批状态。 |
| D-10 | P2 | 确认 | [pages/downloadspage.cpp:269](pages/downloadspage.cpp#L269)、[windows/mainwindow.cpp:806](windows/mainwindow.cpp#L806) | DownloadsPage 在 MainWindow 构造期立即同步读取并恢复完整元信息/健康缓存，即使用户从不进入下载页。 | 下载缓存较大时启动窗口延迟显示。 | 下载页缓存改为首次进入对应 Tab 后懒加载，并后台解析。 |
| D-11 | P1 | 确认 | [pages/downloadspage.cpp:1192](pages/downloadspage.cpp#L1192) | 批量同步每更新一个模型都重建整个 QTableWidget，并序列化/原子重写完整扫描与健康缓存。 | N 个模型产生近似 O(N²) 的 UI 构建和大量磁盘写，批量更新越接近完成越卡。 | 状态按行增量更新；缓存使用 debounce，在整批/阶段结束后写一次。 |
| D-12 | P2 | 确认 | [pages/downloadspage.cpp:228](pages/downloadspage.cpp#L228) | 用户每勾选/取消一个元信息项都会立即写完整缓存。 | Ctrl/Shift 或大量点击选择时明显卡顿并增加 SSD 写入。 | 选择状态仅内存更新，使用 300-500ms debounce 或页面离开时保存。 |
| D-13 | P2 | 确认 | [pages/downloadspage.cpp:1265](pages/downloadspage.cpp#L1265) | 恢复缓存时不校验当前模型库路径集合、文件存在性或根目录配置指纹。 | 模型移动/删除后仍显示旧扫描项和健康问题，按钮可能指向失效路径。 | 缓存加入模型库指纹；加载时快速剔除不存在项并标注缓存时间。 |
| D-14 | P2 | 确认 | [pages/downloadspage.cpp:1327](pages/downloadspage.cpp#L1327) | 下载分类依赖中文状态字符串的 `contains()` 判断，而不是结构化 enum。 | 修改翻译/文案后任务进入错误 Tab；复合错误文本也可能被误判为“本地/更新”。 | 卡片状态保存为 enum，文本只用于展示。 |

## 四、配置与用户数据持久化

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| C-01 | P1 | 确认 | [windows/mainwindow.cpp:10574](windows/mainwindow.cpp#L10574) | 全局设置不检查 JSON 解析错误，最终用普通 QFile 覆盖，忽略写入结果。 | settings.json 一次截断后，下次保存可能用空对象覆盖并丢失所有无关字段。 | 集中到单一 SettingsStore，严格解析并用 `QSaveFile`。 |
| C-02 | P1 | 确认 | [tools/llmpromptwidget.cpp:1565](tools/llmpromptwidget.cpp#L1565)、[tools/prompttemplatelibrarywidget.cpp:2790](tools/prompttemplatelibrarywidget.cpp#L2790)、[tools/promptparserwidget.cpp:1098](tools/promptparserwidget.cpp#L1098)、[pages/launcherwidget.cpp:524](pages/launcherwidget.cpp#L524) | 多个页面各自 read-modify-write 同一 `settings.json`，且多数不是原子写入。 | 嵌套信号、异常退出或未来线程化后可能覆盖其它页面刚写的字段；写入实现和错误处理不一致。 | 所有页面只发设置变更信号，由一个持久化服务串行合并与原子保存。 |
| C-03 | P2 | 确认 | [windows/mainwindow.cpp:10180](windows/mainwindow.cpp#L10180)、[pages/settingspage.h:26](pages/settingspage.h#L26) | settings.json 不存在时 `SettingsState` 的过滤词为空；默认 `DEFAULT_FILTER_TAGS` 只在“文件存在”分支传入。 | 首次运行设置页显示空过滤词，首次保存后永久覆盖预期默认过滤。 | 初始化 `SettingsState` 时直接带默认过滤词，无论配置文件是否存在。 |
| C-04 | P1 | 确认 | [tools/llmpromptwidget.cpp:607](tools/llmpromptwidget.cpp#L607)、[tools/prompttemplatelibrarywidget.cpp:1155](tools/prompttemplatelibrarywidget.cpp#L1155) | 对话历史和模板/收藏库直接 truncate 覆盖写，没有事务提交。 | 保存中断后全部对话历史或模板收藏丢失。 | 改用 `QSaveFile`，保留最近一次有效备份并检查 JSON。 |
| C-05 | P1 | 确认 | [windows/mainwindow.cpp:1966](windows/mainwindow.cpp#L1966)、[windows/mainwindow.cpp:2005](windows/mainwindow.cpp#L2005) | Collections 与模型高亮颜色仍直接覆盖写，忽略失败。 | 收藏夹结构或全部颜色设置可能在异常退出后丢失。 | 统一原子 JSON helper。 |
| C-06 | P1 | 确认 | [tools/tagbrowserwidget.cpp:1462](tools/tagbrowserwidget.cpp#L1462) | 用户编辑的翻译 CSV 直接覆盖原文件。 | 保存中断会破坏词表，影响所有 Tag 翻译页面。 | 写临时文件并原子替换；保存前可保留 `.bak`。 |

## 五、Tag、模板库与 LLM

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| T-01 | P2 | 确认 | [tools/tagbrowserwidget.cpp:264](tools/tagbrowserwidget.cpp#L264)、[tools/prompttemplatelibrarywidget.cpp:535](tools/prompttemplatelibrarywidget.cpp#L535)、[windows/mainwindow.cpp:8340](windows/mainwindow.cpp#L8340) | 图库按“一张图一个 Tag 一次”计数，但 Tag 浏览和模板 Tag Picker 按 prompt 中每次出现计数。 | 同一个 Tag 在三个页面显示不同使用次数；一张图重复 Tag 会在两个工具页被重复统计。 | 提取统一的 per-image Tag 计数 helper，所有页面复用。 |
| T-02 | P3 | 确认 | [tools/tagbrowserwidget.cpp:283](tools/tagbrowserwidget.cpp#L283)、[tools/prompttemplatelibrarywidget.cpp:610](tools/prompttemplatelibrarywidget.cpp#L610) | 模板 Tag Picker 会跳过 `__*` 缓存元项，Tag 浏览 worker 不跳过。 | 缓存 schema 增加 `__metadata__` 后，两个页面结果可能分歧或出现空统计行。 | 两处统一忽略保留键。 |
| T-03 | P1 | 确认 | [tools/prompttemplatelibrarywidget.cpp:2936](tools/prompttemplatelibrarywidget.cpp#L2936) | 生成页和模板管理页两个 Tag Picker 共用一个 watcher、一个结果缓存和一个 scope；加载中另一 picker 只显示“正在加载”后返回，完成时只刷新发起者。 | 快速切换两个 picker 时，一侧永久空白/旧数据，或正负范围显示错位。 | 数据加载服务共享，但每个 picker 保持独立 view state；完成后刷新所有等待相同 scope 的视图。 |
| T-04 | P2 | 确认 | [tools/tagbrowserwidget.cpp:602](tools/tagbrowserwidget.cpp#L602)、[tools/prompttemplatelibrarywidget.cpp:927](tools/prompttemplatelibrarywidget.cpp#L927) | 析构/重载时在 GUI 线程 `waitForFinished()`；QtConcurrent worker 没有合作式取消点。 | 关闭工具页、切换词表或退出应用时会卡到后台读取结束。 | watcher 生命周期与 worker 数据解耦，取消只丢弃结果，不在 GUI 阻塞等待。 |
| T-05 | P1 | 确认 | [tools/llmpromptwidget.cpp:1164](tools/llmpromptwidget.cpp#L1164)、[tools/llmpromptwidget.cpp:2598](tools/llmpromptwidget.cpp#L2598) | 发送请求时在 GUI 线程同步读取并 Base64 编码图片。 | 点击生成/发送后，遇到多张大图会短时假死并产生多份内存拷贝。 | 后台准备图片 payload，限制单图/总大小，并显示准备进度。 |
| T-06 | P1 | 确认 | [tools/llmpromptwidget.cpp:1173](tools/llmpromptwidget.cpp#L1173)、[tools/llmpromptwidget.cpp:1200](tools/llmpromptwidget.cpp#L1200) | 每轮请求都会重新读取并发送历史中每条消息的所有图片。 | 长对话请求体无限增长，发送越来越慢、内存峰值增大，并快速触及后端上下文/请求限制。 | 对历史图片保存后端引用或只在首次出现时发送；增加会话图片预算和裁剪策略。 |
| T-07 | P2 | 确认 | [tools/tagbrowserwidget.cpp:267](tools/tagbrowserwidget.cpp#L267)、[tools/prompttemplatelibrarywidget.cpp:537](tools/prompttemplatelibrarywidget.cpp#L537) | 计数 map 直接以显示文本为 key，没有统一大小写、空格/下划线规范化。 | 语义相同 Tag 分成多行和多个计数，搜索却可能把它们视为相同。 | 用规范化 key 聚合，单独保存显示形式和翻译。 |

## 六、图片元数据与 ComfyUI 解析

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| P-01 | P1 | 确认 | [utils/imagemetadataparser.cpp:21](utils/imagemetadataparser.cpp#L21) | 手写 PNG chunk 解析直接信任 32 位 length 并调用 `file.read(length)`，没有上限和“剩余文件长度”校验；之后还把 length 转为 int。 | 损坏/恶意图片可触发超大内存分配、OOM、长时间卡死或整数边界错误。 | 限制文本 chunk 尺寸，先核对文件剩余长度，并使用安全的 qsizetype/qint64 比较。 |
| P-02 | P2 | 确认 | [utils/imagemetadataparser.cpp:48](utils/imagemetadataparser.cpp#L48) | iTXt 跳过 compression flag/method 后，无论是否压缩都直接当 UTF-8 文本读取；也不支持 zTXt。 | 合法的压缩 iTXt/zTXt 元数据会变成乱码或无法解析，可能对应用户看到的 JSON/Unknown。 | 按 PNG 规范解压压缩 iTXt/zTXt，并限制解压后大小。 |
| P-03 | P2 | 确认 | [utils/imagemetadataparser.cpp:64](utils/imagemetadataparser.cpp#L64) | CRC 仅跳过，不校验。 | 损坏 chunk 可能被当成有效 prompt/workflow，生成随机 Tag 或错误参数。 | 至少对目标文本 chunk 校验 CRC，失败后交给 QImageReader fallback。 |
| P-04 | P1 | 高 | [utils/imagemetadataparser.cpp:371](utils/imagemetadataparser.cpp#L371) | 多 KSampler 时以 QJsonObject 遍历中“最后遇到”的节点为主采样器，不依据工作流执行顺序、输出节点或图连接。 | 多阶段/高分修复工作流可能提取错误正负 prompt、Checkpoint 和 LoRA，导致图库筛选错漏。 | 从 SaveImage/最终输出反向追踪实际采样链；无法唯一判断时返回候选而非猜测。 |
| P-05 | P2 | 高 | [utils/imagemetadataparser.cpp:459](utils/imagemetadataparser.cpp#L459) | workflow-only 转换把 KSampler 的前五个 `widgets_values` 固定映射为 seed/steps/cfg/sampler/scheduler。 | 自定义节点或版本改变 widget 顺序后，参数错位，甚至把布尔开关当 seed。 | 结合节点 input/widget 元数据按名称映射；不可靠时不填该字段。 |
| P-06 | P2 | 确认 | [utils/imagemetadataparser.cpp:379](utils/imagemetadataparser.cpp#L379) | 仅识别名称包含 `KSampler` 的节点；`SamplerCustom` 等常见链路不会作为采样器解析。 | 一部分 ComfyUI 图仍显示 Unknown/No prompt，Checkpoint/LoRA 图库筛选失败。 | 添加 SamplerCustom/Guider/Noise/Scheduler 链路适配，并建立解析器插件表。 |
| P-07 | P1 | 确认 | [utils/imagemetadataparser.cpp:502](utils/imagemetadataparser.cpp#L502) | prompt JSON 一旦解析出任意 `hasContent()` 就立即返回，不再用 workflow 补齐缺失的 Checkpoint/LoRA；`parameters` 是 JSON 但 Comfy 解析失败时也直接返回空结果。 | prompt 有文字但模型信息只在 workflow 时，正负提示词能显示却无法按模型筛图；另一些图片存在其它有效 chunk 也被提前判定为空。 | 将 prompt/workflow/parameters 解析结果按字段合并；失败时继续尝试其它来源，不做早退。 |
| P-08 | P2 | 确认 | [utils/imagemetadataparser.cpp:400](utils/imagemetadataparser.cpp#L400) | 无法从 KSampler 链追踪 Checkpoint 时固定取收集列表第一项。 | 多 Checkpoint 工作流可能按错误底模匹配图库。 | 无法确定时保存全部候选和歧义状态；筛选不应静默选第一个。 |

## 七、图片同步工具的安全性与一致性

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| S-01 | P1 | 确认 | [tools/syncwidget.cpp:255](tools/syncwidget.cpp#L255)、[tools/syncwidget.cpp:303](tools/syncwidget.cpp#L303) | 用户密钥不足 32 字节时用零字节补齐；空密钥等价于公开可预测的全零 AES key。 | 用户未设置密钥时，同一网络内任何知道协议的人都能构造/解密同步流量。 | 拒绝空/弱密钥；使用 KDF（Argon2/PBKDF2/scrypt）派生固定长度密钥并保存 salt。 |
| S-02 | P1 | 确认 | [tools/syncwidget.cpp:424](tools/syncwidget.cpp#L424) | 白名单只信任客户端 JSON 自报的 deviceId，没有与密钥或公钥绑定。 | 知道某个白名单 ID 的客户端可冒充该设备。 | 每设备公钥/令牌认证，deviceId 只作为显示字段。 |
| S-03 | P2 | 确认 | [tools/syncwidget.cpp:752](tools/syncwidget.cpp#L752) | 从白名单删除设备不会断开已认证连接；代码内循环为空并留有未实现注释。 | 已移除设备在当前连接期间仍可继续同步。 | 保存 socket->deviceId 映射，移除时立即断开并撤销会话。 |
| S-04 | P1 | 确认 | [tools/syncwidget.cpp:392](tools/syncwidget.cpp#L392)、[tools/syncwidget.cpp:400](tools/syncwidget.cpp#L400) | 单包允许 512MB；接收时同时持有 socket 缓冲、ciphertext 和 plaintext，多份内存峰值。 | 一个客户端可造成超过 1GB 瞬时内存占用，应用卡死或被系统终止。 | 使用流式分块协议和更小帧上限，文件写入临时文件。 |
| S-05 | P1 | 确认 | [tools/syncwidget.cpp:415](tools/syncwidget.cpp#L415) | 解密后读取 `plainTotalLen/jsonLen`，但未验证 plaintext 至少 8 字节、jsonLen 边界、totalLen 与实际长度一致。 | 畸形但认证通过的包被截断解析，协议状态不一致；后续扩展为文件接收时风险更大。 | 在任何 `mid()`/JSON 解析前做严格长度和上限校验。 |
| S-06 | P1 | 确认 | [tools/syncwidget.cpp:478](tools/syncwidget.cpp#L478)、[tools/syncwidget.cpp:499](tools/syncwidget.cpp#L499) | `sendFile()` 在 GUI 线程 `readAll()`，随后又构造明文、密文和最终包；manifest 差异循环同步发送所有文件。 | 初次同步大量/大图时界面长时间冻结并产生数倍文件大小的内存峰值。 | 后台分块读取、加密和写 socket，使用发送队列与背压。 |
| S-07 | P2 | 确认 | [tools/syncwidget.cpp:508](tools/syncwidget.cpp#L508) | 差异比较只看文件大小。 | 内容改变但大小相同的图片不会重新同步。 | 使用 mtime + hash，或至少 size + mtime；协议保存版本。 |
| S-08 | P1 | 高 | [tools/syncwidget.cpp:551](tools/syncwidget.cpp#L551) | 目录 watcher 看到新文件且 size>0 就立即发送，没有等待文件稳定；只处理 added set。 | 生成器仍在写入时同步出半张/损坏图片，后续文件大小变化不一定触发再次发送。 | 文件大小/mtime 连续稳定若干次后发送，并处理 modified 文件。 |
| S-09 | P2 | 确认 | [tools/syncwidget.cpp:519](tools/syncwidget.cpp#L519) | 忽略 `QFileSystemWatcher::addPath()` 返回值，却无条件记入 watched map。 | 达到系统监控上限或路径无效后，UI 仍显示已监控，但实际不再同步。 | 仅记录成功路径并在 UI 显示失败目录。 |
| S-10 | P1 | 确认 | [tools/syncwidget.cpp:461](tools/syncwidget.cpp#L461) | 根目录匹配使用原始字符串 `startsWith()`；没有 canonical path、分隔符边界和 Windows 大小写规范。 | `C:\foo` 会误匹配 `C:\foobar`，重叠根目录可能生成错误远端路径或删除通知。 | canonical/clean path 后按目录边界判断，并优先最长根目录。 |
| S-11 | P1 | 确认 | [tools/syncwidget.cpp:345](tools/syncwidget.cpp#L345)、[tools/syncwidget.cpp:361](tools/syncwidget.cpp#L361) | 忽略 `RAND_bytes()`、加密结果和 socket `write()` 返回值/背压。 | 极端熵源失败仍发送零 IV；断线或缓冲满时数据静默丢失或无限堆积。 | 所有密码学/IO API 必须检查结果；实现 bytesWritten 驱动的发送队列。 |
| S-12 | P0 | 高 | [tools/syncwidget.cpp:735](tools/syncwidget.cpp#L735) | 用户从本地“移除监控文件夹”会立即向客户端发送 `DELETE_FOLDER`。 | 用户本意仅停止同步，却可能删除远端整个文件夹，属于高风险语义耦合。 | “停止监控”和“删除远端”拆成不同操作，删除必须二次确认。 |
| S-13 | P2 | 中 | [tools/syncwidget.cpp:703](tools/syncwidget.cpp#L703) | 活动列表把 socket 以裸 `void*` 存入 QVariant。 | socket 删除与 UI item 清理时序不一致时可能留下悬空指针，点击断开存在访问无效对象风险。 | 使用 `QPointer<QTcpSocket>` 或稳定连接 ID，不把裸指针放入 QVariant。 |

## 八、UI、启动/退出与结构性风险

| ID | 优先级 | 置信度 | 错误位置 | 静态检查结果 | 可能导致的现象 | 建议方向 |
|---|---|---|---|---|---|---|
| U-01 | P2 | 确认 | [windows/mainwindow.ui:528](windows/mainwindow.ui#L528)、[windows/mainwindow.ui:1356](windows/mainwindow.ui#L1356) | 两个控件都命名为 `line`；uic 会警告并自动把第二个改为 `line1`。 | Designer、生成 UI 和手写代码中的 objectName 可能不一致；未来 QSS/findChild 容易命中错误对象。 | 在 `.ui` 中为两条分割线设置唯一、语义化名称。 |
| U-02 | P2 | 确认 | [windows/mainwindow.cpp:804](windows/mainwindow.cpp#L804)、[windows/mainwindow.cpp:10313](windows/mainwindow.cpp#L10313) | 构造时先应用一次默认主题，读取配置后再完整应用一次主题。 | 启动重复解析 QSS、repolish 全部控件，并可能出现主题闪烁和额外延迟。 | 先读取轻量主题配置，再构建/首次应用一次最终主题。 |
| U-03 | P1 | 确认 | [windows/mainwindow.cpp:1894](windows/mainwindow.cpp#L1894) | MainWindow 析构在 GUI 线程等待多个 watcher 和两个线程池无限完成。 | 有大图解码、Hash、图库扫描或网络相关 worker 时，退出长时间卡住；历史退出异常更难与真正内存错误区分。 | closeEvent 先进入异步关闭阶段，任务支持 token/取消；避免无限 `waitForDone()`。 |
| U-04 | P2 | 确认 | [pages/launcherwidget.cpp:353](pages/launcherwidget.cpp#L353) | Launcher 关闭每个进程树后仍在 GUI 线程最多等待 2 秒。 | 同时运行多个后端时，退出应用可额外阻塞数秒。 | 异步终止并显示关闭进度，超时后再强制处理。 |
| U-05 | P2 | 确认 | [windows/mainwindow.cpp](windows/mainwindow.cpp)、[windows/mainwindow.h](windows/mainwindow.h) | MainWindow 仍集中管理图库、详情、元数据、更新检测、预览下载、配置、主题、工具页和多组共享计数/队列。 | 不同页面误用同一队列/状态的缺陷已实际出现（如 M-08、D-09）；后续修改容易产生跨功能回归。 | 下一阶段优先拆 `GalleryController`、`MetadataSyncManager`、`ModelRepository` 和统一 `SettingsStore`，以绝对路径作为模型身份。 |

## 建议审核顺序

1. 先审核 P0：D-05、M-10、S-12，避免模型文件或远端图库数据丢失。
2. 再审核截图相关链路：G-01、G-02、G-03、G-05。
3. 处理模型身份与元数据正确性：M-01 至 M-08、M-12。
4. 处理批量更新与主线程卡顿：D-04、D-09 至 D-12、M-13、G-12、G-13。
5. 最后统一持久化、Tag 统计、ComfyUI 解析和同步协议。

## 静态验证记录

- 未启动应用，未做截图/视觉运行验证，符合本次要求。
- 所有 14 个 `.ui` 均用 Qt 6.11.1 `uic` 做了生成检查；U-01 修复后不再出现重复 objectName 警告。
- `git diff --check` 通过。
- 使用 Qt 6.11.1 Release 构建生成的完整编译参数和 MSVC 环境，直接编译了本轮所有修改的 `.cpp`；最新的 `windows/mainwindow.cpp` 与 `utils/imagemetadataparser.cpp` 也在最终补丁后重新编译通过。
- 完整 CMake/JOM 构建在载入 MSVC 环境后仍由 Qt Creator 自带 `jom.exe` 异常退出（`0xc0000409`），未输出源码诊断；这属于当前命令行构建器异常，仍建议由用户在 Qt Creator 中执行最终 Release 链接和运行验证。
