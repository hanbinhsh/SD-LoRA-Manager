# 模型网页站点

设置 → 网络 → Civitai 模型网页，可选择 `civitai.red` 或 `civitai.com`。默认使用 `civitai.red`，旧配置中没有该选项时也使用此默认值。设置保存到 `config/settings.json` 的 `civitai_browser_host`，修改后点击浏览器按钮立即生效。

模型详情和下载卡片的浏览器入口共用 `utils/modelbrowserurl.h`。只转换 `civitai.com`、`civitai.red` 及对应 `www` 主机下的 `/models` 页面，保留模型路径、slug、版本查询参数和片段。

API 请求、模型下载、预览图地址、自定义外站链接及 CivArchive 链接不受影响。本地 metadata 中的原始 URL 不被改写。

`core_utils_browser_urls` 覆盖两个站点之间转换、查询参数保留、精确主机匹配及非模型网页不转换；`app_language_test` 覆盖设置默认值、保存恢复和切换信号。
