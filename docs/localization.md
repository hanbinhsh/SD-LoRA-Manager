# 界面语言

在设置的“外观”页选择“简体中文”或“English”。修改自动保存到 `config/settings.json` 的 `ui_language`，重启后生效。旧配置默认简体中文。

语言在创建主窗口之前加载，已打开页面和后续懒加载的工具页使用同一种语言。翻译及 Qt 标准按钮翻译均编入程序资源，无需复制外部 `.qm` 文件。构建环境需要 Qt LinguistTools。

## 文案维护

- 固定界面继续在 `.ui` 中编辑。`<string>` 是翻译的源键，运行时由目录提供当前语言的文本。语言下拉框中的“简体中文”和“English”使用 `notr="true"`，保持可辨识。
- 动态菜单、按钮、标题、提示使用 `AppText::tr("源文案")`；格式化参数在翻译后调用 `.arg(...)`。
- `translations/catalog.json` 集中保存源文案对应的 `zh_CN`、`en_US`。`app_zh_CN.ts` 和 `app_en_US.ts` 由脚本生成，CMake 使用 `lrelease` 编译。
- `.ui` 和 C++ 共用 `AppText` 翻译目录。翻译器先查原始 Qt context，再查共用目录，Qt 标准控件翻译保留原有 context。
- 元数据、模型名称、实际 Tag、用户备注、提示词和路径是用户数据，不传入翻译函数。下载分类、任务状态、配置键和枚举继续使用稳定值；仅在显示状态标签时调用 `AppText::display()`。
- 需要展示已经保存的格式化状态时，目录中的 `%1` 等占位符只匹配展示文案，捕获的模型名、路径或错误内容原样保留。原始状态不被改写。

更新并检查翻译目录：

```powershell
python -X utf8 scripts/update_ui_translations.py
# 在 catalog.json 中补齐新增文案后再次生成
python -X utf8 scripts/update_ui_translations.py --check
```

检查包含英文遗漏及两种语言的格式占位符一致性。`--mark-display-text` 是初次迁移辅助功能；后续新增动态文案应直接写 `AppText::tr()`。

## 验证

启用 `BUILD_CORE_UTIL_TESTS` 后，`app_language_test` 在离屏模式下检查所有 `.ui` 的英文文案、各页面的固定按钮/标签/Tab、设置保存恢复、动态弹窗文本、状态占位符换序及用户数据保留。另验证英文下载卡片仍使用原始状态分类和重试规则。测试使用专用目录，不启动主应用。
