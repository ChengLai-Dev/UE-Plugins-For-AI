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
| Export Property Schema | 把每个控件类/槽类的可写属性名、类型与值格式导出到 `Saved/WidgetJsonBridge/PropertySchema/`，供 AI 查表 |
| Open Sync Folder | 打开 `UIWidgets` 目录 |
| Open Docs Folder | 打开本插件文档目录 |

对应的控制台命令：`WidgetJsonBridge.ExportSchema`

## 命名检查（显示名 vs 对象名）

Designer 的 Hierarchy 显示的是 `UWidget::GetLabelText()`，它不一定是对象名：名字以「类名_」开头且控件不是变量时，引擎会把整段名字换成类名（`Border_IconFrame` 显示成 `Border`）；设过 `DisplayLabel` 时又会显示 `DisplayLabel`。代码里 `GetName()` / `FindWidget` / `BindWidget` / 字符串路径用的都是对象名，两者分叉就会出现「面板上看得见、代码里找不到」。

**插件导入时自动对齐**：每个控件迁进真实 WidgetTree（Rename 之后）会执行 `SetDisplayLabel(对象名)`，所以面板名字不会再被替换成类名，也不需要手写 `DisplayLabel`；手动设的会在下次重建时被覆盖。

在此基础上，**导入成功后**和**Designer 保存后**各跑一次检查（诊断用，**不阻断导入**）：

| 情况 | 级别 |
|---|---|
| 控件暴露成变量（`isVariable` / 事件绑定）且显示名 ≠ 对象名 | **Error** |
| 其余控件（装饰节点）显示名 ≠ 对象名 | **Warning** |
| 设置 `WidgetJsonBridge.StrictNameCheck 1` 后 | 所有不一致都算 **Error** |

日志前缀 `WidgetJsonBridge[Import]` / `WidgetJsonBridge[Save]`。全量体检（只扫 `/Game`，不用改 JSON 触发）：

```
WidgetJsonBridge.ValidateNames
```

不在这里中断导入，是因为 `FScopedTransaction::Cancel()` 只把事务踢出撤销栈、并不回滚已经换上的树；中断会留下「坏树 + 脏包」，比继续导入更糟。

检查比的是 `GetLabelText()`（不带装饰）；面板上非变量控件显示成 `[名字]`（TextBlock 还会附文本摘要），方括号和多出来的摘要是 `GetLabelTextWithMetadata()` 的固定格式，不是命名问题。

若确实要改名字本身：把对象名改成不以「类名_」开头（如 `IconFrame_Border`），或勾上 `Is Variable`。

## 结构体对象模式（`structMode`）

> **AI 写对象结构体时必须同时写顶层 `"structMode": "object"`。**
> 导入只看 JSON 值类型，所以缺少该字段也能导入；但导出按它选格式，缺少时编辑器下次保存会把对象改回引擎文本字符串。

默认情况下，只有 `FVector2D` / `FLinearColor` / `FMargin` / `FAnchors` / `FAnchorData` / `FSlateChildSize` 有专用对象写法，`FSlateBrush`、`FProgressBarStyle` 等结构体导出为引擎文本字符串。

在 JSON 顶层加一个字段即可切换：

```json
{
  "schemaVersion": 1,
  "structMode": "object",
  "root": { }
}
```

- `"string"`（缺省）：旧行为，非白名单结构体仍是引擎文本字符串
- `"object"`：所有可导出结构体递归导出为 JSON 对象，字段名就是 C++ 属性名（如 `WidgetStyle.FillImage.TintColor`）；对象引用写资产路径字符串
- 白名单结构体仍保持专用格式，例如 `FAnchorData` 还是 `offsets` / `anchors` / `alignment`
- 容器字段（`TArray` / `TMap` / `TSet`）暂不回退为对象，仍是引擎文本字符串
- `structMode` 值非法（不是 `"string"` / `"object"`）时按 `"string"` 处理，并在 Output Log 打 Warning
- **导出模式跟随文件**：插件导出前读取目标 JSON 的 `structMode`，手动导出与 Ctrl+S 自动回写都按它执行；文件不存在或没有该字段时按 `"string"`
- **导入不依赖该字段**：JSON 是对象就按对象解析，是字符串就按旧文本解析，旧 JSON 无需迁移
- 结构体对象里写错字段名会让整次导入失败并写入错误报告

示例见 `SchemaReference.md` 的「对象模式示例：ProgressBar 的 WidgetStyle」。

## 工作流

1. 编辑器保持打开
2. AI 在 `UIWidgets/` 下写 JSON（格式见 `SchemaReference.md`）
3. 1 秒内资产自动重建，可在 Content Browser / Designer 中查看
4. 也可在 Designer 中手工调整并 Ctrl+S，JSON 自动更新
5. 若出现 `_editor_conflict.json`，说明两边同时改了，需要人工合并

## 导入失败时的行为

- **旧资产不会被破坏**：新树先在临时 WidgetTree 上完整构建，构建成功后才替换旧树；中途失败则资产保持原样
- **错误报告**：失败原因写入 `<项目>/Saved/WidgetJsonBridge/Errors/<相对路径>.error.json`，包含 `jsonPath` / `assetPath` / `error` / `timestamp`；导入成功后该文件自动删除
- **不刷日志**：同一份内容失败过就不会每秒重试，文件一改动（内容变化）自动重新尝试
- **可撤销**：一次导入包在一个编辑器事务里（`Blueprint->Modify()` + `WidgetTree->Modify()`，控件 Rename 也在事务内），Ctrl+Z 可回退。此路径未做 GUI 自动化实测，若发现异常请先备份 JSON

## 限制

- 动画（WidgetAnimation）、BindWidget 暂不支持（后续版本）
- 引擎版本适配：当前按 UE 5.8 编译；插件 Binaries 由各引擎版本分别编译
- 删除 JSON 文件不会删除资产，需在 Content Browser 中手动删除
