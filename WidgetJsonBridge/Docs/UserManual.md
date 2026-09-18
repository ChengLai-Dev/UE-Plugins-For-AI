# WidgetJsonBridge 使用手册

## 简介

WidgetJsonBridge 是一个 UE 编辑器插件，为 UMG User Widget 提供 JSON 双向同步：

- **AI / 外部工具 → 引擎**：编辑项目根目录 `UIWidgets/` 下的 JSON 文件，编辑器自动重新构建对应 Widget 资产
- **引擎 → JSON**：在 UMG Designer 中修改 Widget 并保存（Ctrl+S）后，改动自动回写 JSON
- **冲突保护**：两边同时修改时，编辑器版本存为 `xxx_editor_conflict.json`，绝不静默覆盖

## 安装

1. 本插件位于仓库 `UE-Plugins-For-AI` 的 `WidgetJsonBridge/` 目录下；使用时可整体拷贝或 clone 到任意位置
2. 目标项目的 `.uproject` 需包含：
   ```json
   "AdditionalPluginDirectories": ["<插件所在目录>"],
   "Plugins": [
     { "Name": "WidgetJsonBridge", "Enabled": true }
   ]
   ```
3. 重新编译项目的 Editor 目标（见 README「构建」，注意需要 `-NoHotReloadFromIDE`）

## 路径映射

| JSON 文件 | 资产 |
|---|---|
| `<项目>/UIWidgets/HUD/MainMenu.json` | `/Game/HUD/MainMenu.uasset` |

- JSON 文件放入 `UIWidgets/` 后（目录不存在可手动创建），编辑器检测到即创建对应资产
- 资产已存在时，导入会重建其 WidgetTree（旧树被替换）

## 菜单（Tools > Widget Json Bridge）

| 菜单项 | 作用 |
|---|---|
| Export Edited Widget to JSON | 将当前打开的 Widget Blueprint 导出为 JSON（手动触发，无视冲突检测直接覆盖） |
| Import All JSON Files | 清空同步记录并全量重导入 `UIWidgets/` 下所有 JSON |
| Open Sync Folder | 打开 `UIWidgets` 目录 |
| Open Docs Folder | 打开本插件文档目录 |

## 工作流

1. 编辑器保持打开
2. AI 在 `UIWidgets/` 下写 JSON（格式见 `SchemaReference.md`）
3. 1 秒内资产自动重建，可在 Content Browser / Designer 中查看
4. 也可在 Designer 中手工调整并 Ctrl+S，JSON 自动更新
5. 若出现 `_editor_conflict.json`，说明两边同时改了，需要人工合并

## 限制

- 事件绑定（OnClicked 等）、属性绑定、BindWidget、动画暂不支持（后续版本）
- 引擎版本适配：当前按 UE 5.8 编译；插件 Binaries 由各引擎版本分别编译
- 删除 JSON 文件不会删除资产，需在 Content Browser 中手动删除
