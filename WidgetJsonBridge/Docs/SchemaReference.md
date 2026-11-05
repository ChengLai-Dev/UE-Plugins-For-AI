# WidgetJsonBridge Schema 参考（AI 编辑指南）

本插件将 UMG User Widget 与 JSON 文件双向同步。**JSON 文件是 AI 的编辑界面**。

## 同步机制

- 同步目录：`<项目根>/UIWidgets/`，文件路径映射资产路径：
  `UIWidgets/HUD/MainMenu.json` ↔ `/Game/HUD/MainMenu.uasset`
- AI 修改 JSON 文件 → 编辑器（运行中）自动检测并重新构建资产（约 1 秒轮询）
- 编辑器中手工修改 Widget 并保存（Ctrl+S）→ 自动回写 JSON
- 冲突（两边同时改）：编辑器版本存为 `xxx_editor_conflict.json`，主文件保留 AI 版本，人工合并。**冲突文件不会被自动导入**，合并完删掉即可
- 编辑器必须处于打开状态，同步才会发生

### 导入失败时

- **旧资产不会被破坏**：新树先完整构建，成功后才替换旧树
- 错误原因写到 `<项目>/Saved/WidgetJsonBridge/Errors/<相对路径>.error.json`
- **属性级错误也算失败**：未知属性名、值格式错误、单子容器塞多个子控件、抽象类，都会让整次导入失败并写报告，不会静默跳过
- **编译失败不落盘**：编译不过时不保存资产
- 同一份失败内容不会每秒重试，文件一改动自动重试
- 整个导入包在一个编辑器事务里（`Ctrl+Z` 可回退；此路径未做 GUI 自动化实测）

## JSON 结构

```json
{
  "schemaVersion": 1,
  "assetPath": "/Game/HUD/MainMenu",
  "contentHash": "5f4dcc3b5aa765d61d8327deb882cf99",
  "root": {
    "class": "/Script/UMG.CanvasPanel",
    "className": "CanvasPanel",
    "name": "CanvasPanel_0",
    "isVariable": false,
    "properties": { },
    "events": ["OnClicked"],
    "slot": null,
    "children": [ ]
  }
}
```

- `schemaVersion`：固定为 `1`
- `structMode`：可选，`"string"`（缺省）或 `"object"`。`"object"` 让非白名单结构体也用 JSON 对象导出，详见「结构体对象模式」
- `assetPath`：信息性字段，实际映射以文件路径为准
- `contentHash`：由插件维护，**AI 不要手动修改**（导入时忽略）
- `root`：根控件节点；`children` 递归嵌套

### 控件节点字段

| 字段 | 说明 |
|---|---|
| `class` | 类的完整对象路径。C++ 类如 `/Script/UMG.TextBlock`；蓝图类如 `/Game/UI/WBP_Avatar.WBP_Avatar_C` |
| `className` | 短名（如 `TextBlock`），仅信息性，导入忽略 |
| `name` | 控件名。**按原名创建**；只有同一 WidgetTree 下真的重名才会加数字后缀并打警告 |
| `isVariable` | 是否暴露为蓝图变量（bool）。节点声明了 `events` 时会被强制设为 `true` |
| `events` | 事件委托名数组，如 `["OnClicked"]`。导入时自动创建对应的蓝图事件节点；导出时从蓝图绑定表回写 |
| `properties` | 可编辑属性表（见下） |
| `slot` | 父容器分配给它的槽位（根节点为 null）。`slot.properties` 里是槽位属性（位置/尺寸/对齐等） |
| `children` | 子控件数组 |

> `name` 稳定是硬约定：动画绑定与蓝图绑定表都按控件名索引。改名等于新建一个控件。

## 属性值格式

`properties` 是 `属性名 -> 值` 的映射。

**未写的属性 = 使用该类默认值。** 导出时已做 CDO 差异过滤，与默认值相同的属性不会出现在 JSON 里，所以一份干净界面的 JSON 通常很小。

值按属性类型输出：

- `bool` → JSON bool
- 整数/浮点 → JSON number
- 枚举 → 枚举名字符串（如 `"Left"`、`"SelfHitTestInvisible"`）
- `FText`/字符串/`FName` → 纯文本字符串（AI 直接写要显示的文本即可）
- 对象引用 → 路径字符串，如 `"/Game/UI/Textures/T_Icon.T_Icon"` 或 `"None"`
- 结构体 → 见下（白名单用专用原生 JSON 对象；默认其余结构体用引擎文本格式字符串，顶层写 `structMode: object` 后可改写成对象）

### 结构体对象模式（顶层 `"structMode": "object"`）

> **AI 注意**：写对象形式的结构体时，**必须同时在顶层写 `"structMode": "object"`**。
> 导入不依赖该字段，但导出会按它选格式；缺少该字段时，编辑器下次保存会把结构体对象改回引擎文本字符串，你的对象写法会丢失。

顶层字段 `structMode` 控制**导出**格式：

| 值 | 行为 |
|---|---|
| 缺省 / `"string"` | 旧行为。白名单结构体仍用下面的专用格式，其余结构体导出为引擎文本字符串 |
| `"object"` | 所有可导出结构体递归导出为 JSON 对象；白名单结构体仍保持专用格式（如 `FAnchorData` 还是 `offsets/anchors/alignment`） |

对象模式下每个字段的 JSON key 就是 **C++ 属性名**（`Property->GetName()`），例如 `FSlateBrush` 的 `TintColor`、`DrawAs`、`ResourceName`、`ResourceObject`。

- 嵌套结构体递归对象化（`FSlateBrush` → `FSlateColor` → `FLinearColor`）
- 白名单结构体不参与通用对象化，继续写 `{"x":..,"y":..}` / `{"r":..,"g":..}` 这类专用格式
- 对象引用（`FSlateBrush.ResourceObject` 等）写资产路径字符串：`"/Game/UI/T_Icon.T_Icon"`，空值写 `"None"`；导入也接受 `Texture2D'/Game/UI/T_Icon.T_Icon'` 这种带类名的外壳
- `FName` / `FText` / bool / 数值 / 枚举与普通属性写法一致
- 容器字段（`TArray` / `TMap` / `TSet`）暂不支持对象化，仍是引擎文本字符串
- `FSlateBrush` 的 `UVRegion` / `bIsDynamicallyLoaded` 没有 `CPF_Edit`，**不参与对象化**（不要手写，保持缺省）；`FSlateColor.ColorTableId` 不是 UPROPERTY，也不会被序列化
- 循环引用或嵌套超过 16 层时导出会回退为引擎文本字符串，导入会报错

**导入不依赖 `structMode`**：JSON 值是对象就按对象解析，是字符串就按旧的 `ImportText` 解析，所以旧文件不需要迁移。
结构体对象里出现不存在的字段名会让整次导入失败并写入错误报告。

### 原生 JSON 对象（推荐）

以下结构体支持写成对象，不用拼引擎文本：

| 结构体 | 写法 |
|---|---|
| `FVector2D` | `{"x": 1.0, "y": 2.0}` |
| `FLinearColor` | `{"r": 1, "g": 0.5, "b": 0, "a": 1}` |
| `FMargin` | `{"left": 10, "top": 5, "right": 10, "bottom": 5}` |
| `FAnchors` | `{"min": {"x": 0.5, "y": 0.5}, "max": {"x": 0.5, "y": 0.5}}` |
| `FAnchorData` | `{"offsets": {...}, "anchors": {...}, "alignment": {"x":..,"y":..}}` |
| `FSlateChildSize` | `{"value": 1.0, "rule": "Fill"}`，`rule` 为 `Fill` 或 `Automatic` |

### 引擎文本格式（其余结构体，默认模式）

未设置 `structMode: object` 时，白名单之外的结构体（`FSlateBrush`、`FSlateFontInfo`、`FSlateColor`、`FWidgetStyle` 等）仍写引擎文本字符串：

- Slate 颜色 `"(SpecifiedColor=(R=1.000000,G=1.000000,B=1.000000,A=1.000000))"`
- 字体 `"(FontObject=None,Size=24.000000,TypefaceFontName=\"Bold\")"`

> 这类结构体格式复杂、容易写错。**可靠做法**：在编辑器里手工设一次并 Ctrl+S 回写 JSON，然后照抄。或者查 `Saved/WidgetJsonBridge/PropertySchema/` 里对应类的 `format` 字段。

#### 对象模式示例：ProgressBar 的 WidgetStyle

顶层写 `"structMode": "object"`，`FProgressBarStyle` / `FSlateBrush` 就可以直接写对象。**未写的字段取类默认值；只改一个字段时不要删掉同级其它字段**（删掉等于把该字段重置为默认值）：

```json
{
  "schemaVersion": 1,
  "structMode": "object",
  "root": {
    "class": "/Script/UMG.ProgressBar",
    "name": "Progress_Health",
    "properties": {
      "Percent": 0.42,
      "FillColorAndOpacity": { "r": 0.8, "g": 0.1, "b": 0.1, "a": 1.0 },
      "WidgetStyle": {
        "BackgroundImage": {
          "TintColor": {
            "SpecifiedColor": { "r": 0.1, "g": 0.1, "b": 0.1, "a": 1.0 },
            "ColorUseRule": "UseColor_Specified"
          },
          "DrawAs": "Image",
          "ImageSize": { "X": 32.0, "Y": 32.0 }
        },
        "FillImage": {
          "TintColor": {
            "SpecifiedColor": { "r": 1.0, "g": 0.25, "b": 0.25, "a": 1.0 },
            "ColorUseRule": "UseColor_Specified"
          },
          "DrawAs": "Image",
          "ResourceObject": "/Game/UI/Textures/T_BarFill.T_BarFill"
        },
        "MarqueeImage": {}
      }
    }
  }
}
```

> `FSlateBrush` 的字段名是 C++ 属性名：`TintColor`、`DrawAs`、`Tiling`、`Mirroring`、`ImageType`、`ImageSize`、`Margin`、`OutlineSettings`、`ResourceName`、`ResourceObject`。
> `ImageSize` 是 `FDeprecateSlateVector2D`，字段名是 `X` / `Y`（大写）。
> 不确定某个结构体有哪些字段时，查 `PropertySchema/` 里该属性的 `fields` 数组。

规则：**导出哪个属性 AI 就能改哪个属性**。未知属性名、值格式错误、容器拒绝子控件、抽象/废弃类，都会让**整次导入失败**并写入错误报告。以前是"告警后跳过"，那会让 AI 无从得知自己写错了什么。改不熟悉的属性名，先查 `PropertySchema/`。

## 属性查表（优先于本文档）

菜单 **Tools > Widget Json Bridge > Export Property Schema**（或控制台命令 `WidgetJsonBridge.ExportSchema`）会按类导出**真实的 UPROPERTY 清单**：

```
<项目>/Saved/WidgetJsonBridge/PropertySchema/
├── index.json                 # 有哪些类、文件在哪
├── widgets/TextBlock.json     # 单个类几 KB
└── slots/CanvasPanelSlot.json
```

每个属性条目形如：

```json
{ "name": "Justification", "category": "Appearance", "type": "ETextJustify",
  "format": "枚举名字符串", "values": ["Left", "Center", "Right"] }
```

`format` 取值：
- `"object"` → 可写原生 JSON 对象（非白名单结构体需要顶层 `"structMode": "object"`；条目里的 `fields` 列出对象字段）
- `"string"` → 必须写引擎文本格式字符串（非白名单结构体的 `legacyFormat` 也是它）
- `"原生 JSON 值"` → bool / number / 字符串
- `"资产路径字符串..."` → 资产引用

> 非白名单结构体条目的 `format` 为 `"object"`，同时带 `legacyFormat: "string"` 和递归的 `fields`：对象模式可用，旧模式仍按字符串。

**改不熟悉的控件属性前，先读对应的 schema 文件**，比读本文档或猜属性名可靠得多。

## 常用控件属性表

### TextBlock
| 属性 | 类型 | 说明 |
|---|---|---|
| `Text` | 文本 | 显示内容 |
| `Font` | `FSlateFontInfo` | 字体与字号，文本格式；**没有 `FontSize` 属性**（引擎里它只是 `SetFontSize` 函数） |
| `ColorAndOpacity` | 结构体 | 文字颜色 |
| `Justification` | 枚举 | 对齐：`Left`/`Center`/`Right` |
| `AutoWrapText` | bool | 自动换行 |
| `MinDesiredWidth` | 数值 | 最小期望宽度 |

### Image
| 属性 | 说明 |
|---|---|
| `Brush` | 含 `ResourceObject=Texture2D'/Game/...'` 与 `ImageSize`，文本格式 |

### Button
| 属性 | 说明 |
|---|---|
| `ClickMethod` | 枚举：`DownAndUp`/`PreciseClick`/`MouseDown`/`MouseUp` |
| `BackgroundColor` / `WidgetStyle` | 背景样式 |

### SizeBox
| 属性 | 说明 |
|---|---|
| `WidthOverride` / `HeightOverride` | 数值 |
| `MinDesiredWidth` / `MinDesiredHeight` | 数值 |

### CanvasPanel 子控件的 slot（`slot.properties`）

> ⚠️ **本节曾写错过**（旧版列的 `Position` / `Size` / `Anchors` / `Alignment` 都不是属性）。
> 下表已按「引擎反射 + 插件实际导出」双重核验订正。

`UCanvasPanelSlot` 的 UPROPERTY **只有三个**：

| 属性 | 类型 | 说明 |
|---|---|---|
| `LayoutData` | `FAnchorData` | 位置/尺寸/锚点全在这一个结构体里，见下 |
| `bAutoSize` | bool | 按内容自适应尺寸 |
| `ZOrder` | 整数 | 渲染顺序，越大越靠上 |

`LayoutData` **推荐写原生对象**：

```json
"LayoutData": {
  "offsets":   {"left": -100, "top": -40, "right": 200, "bottom": 80},
  "anchors":   {"min": {"x": 0.5, "y": 0.5}, "max": {"x": 0.5, "y": 0.5}},
  "alignment": {"x": 0.5, "y": 0.5}
}
```

也仍然接受旧的引擎文本格式：

```
(Offsets=(Left=..,Top=..,Right=..,Bottom=..),Anchors=(Minimum=(X=..,Y=..),Maximum=(X=..,Y=..)),Alignment=(X=..,Y=..))
```

#### `offsets` 的语义：位置 + 尺寸，**不是**右下角坐标

`offsets` 就是 `FAnchorData.Offsets`（一个 `FMargin`），**按轴**分别解释，判据是该轴的两个锚点是否重合：

| 该轴锚点 | `left/top` | `right/bottom` | Designer 里的标签 |
|---|---|---|---|
| 重合（点锚点，`min == max`） | 相对锚点的位置偏移 | **宽 / 高**（正的尺寸） | `Position X/Y` + `Size X/Y` |
| 不重合（拉伸，`min != max`） | 距左/上边的边距 | 距右/下边的边距 | `Offset Left/Top` + `Offset Right/Bottom` |

引擎里的实际代码：

- `SConstraintCanvas::ArrangeLayeredChildren`：锚点重合时 `Size = (Offset.Right, Offset.Bottom)`
- `UCanvasPanelSlot::SetSize`：`LayoutData.Offsets.Right = Size.X`、`LayoutData.Offsets.Bottom = Size.Y`
- `FCanvasSlotCustomization`：锚点重合时把 `Right/Bottom` 的标签换成 `Size X/Size Y`

所以：

- **点的 x 轴锚点重合时，`right` 是宽度**。960 宽的控件写 `right: 960`，不是 `right: 480`
- 想让 960×520 的控件在父容器中心附近：`left = -480, top = -260, right = 960, bottom = 520`
  （位置由 `left/top` 决定，`alignment=(0,0)` 表示控件左上角落在锚点偏移处）
- 想让它以自身中心对齐锚点：`alignment = (0.5, 0.5)`，此时 `left = -Width/2`、`top = -Height/2`、`right = Width`、`bottom = Height`
- 拉伸轴的 `right/bottom` 是边距，可以为负（控件故意超出父容器），保持这个语义

> ⚠️ **最容易踩的坑**：把 `right/bottom` 写成"右下角坐标"（例如 960 宽的控件写 `right: 480`，或 96 宽的格子写 `right: -360`）。
> 前一种会静默变成 480 宽；后一种是负宽度，Slate 里是退化矩形（画不出来也点不到），插件会**直接拒绝导入**并给出换算提示：
> `LayoutData.offsets.right = -360.0 是负数。锚点重合时 right 表示宽度（Designer 里的 Size X），不是右下边界 X 坐标；若这里原本写的是右下边界 -360.0（left = -456.0），应改成 right = 96.0`

> 旧版的 `Position` / `Size` / `Anchors` / `Alignment` 在引擎里只是 `UFUNCTION` 存取器（`SetPosition` / `GetSize` 等），**不是 UPROPERTY**，反射序列化碰不到，按旧文档写会静默无效。

### HorizontalBox / VerticalBox 子控件的 slot

| 属性 | 类型 | 说明 |
|---|---|---|
| `Size` | `FSlateChildSize` | 可写 `{"value": 1.0, "rule": "Fill"}`；`rule` 为 `Fill` 或 `Automatic` |
| `Padding` | `FMargin` | 可写 `{"left":0,"top":8,"right":0,"bottom":8}` |
| `HorizontalAlignment` | 枚举 | `HAlign_Fill` / `HAlign_Left` / `HAlign_Center` / `HAlign_Right` |
| `VerticalAlignment` | 枚举 | `VAlign_Fill` / `VAlign_Top` / `VAlign_Center` / `VAlign_Bottom` |

### 其他槽类

| 槽类型 | 属性 |
|---|---|
| `UButtonSlot` | `Padding`、`HorizontalAlignment`、`VerticalAlignment` |

**通用办法**：任何槽类都可以先在编辑器里手工摆好一个，Ctrl+S 回写 JSON 后照抄格式，或查 `PropertySchema/slots/`。

## 常见任务示例

### 1. 修改文字

```json
"properties": { "Text": "开始游戏" }
```

改字号要写 `Font`（`FSlateFontInfo` 文本格式），先导出 Schema 确认字段，别写 `FontSize` —— 它不是 UPROPERTY，写了会让整次导入失败。

### 2. 添加一个按钮到 VerticalBox

在目标父控件的 `children` 数组末尾追加：

```json
{
  "class": "/Script/UMG.Button",
  "className": "Button",
  "name": "Btn_Start",
  "isVariable": true,
  "properties": {},
  "children": [
    {
      "class": "/Script/UMG.TextBlock",
      "name": "Text_Start",
      "properties": { "Text": "开始" }
    }
  ]
}
```

### 3. 设置 CanvasPanel 子控件位置与锚点

居中（0.5, 0.5）、尺寸 200×80、以自身中心为轴（`left/top` 是位置偏移，`right/bottom` 是**宽高**）：

```json
"slot": {
  "class": "/Script/UMG.CanvasPanelSlot",
  "properties": {
    "LayoutData": {
      "offsets":   {"left": -100, "top": -40, "right": 200, "bottom": 80},
      "anchors":   {"min": {"x": 0.5, "y": 0.5}, "max": {"x": 0.5, "y": 0.5}},
      "alignment": {"x": 0.5, "y": 0.5}
    }
  }
}
```

同样的控件如果 `alignment` 用 `(0,0)`（左上角对齐锚点），尺寸不变，位置写成 `left: -100, top: -40`，`right/bottom` 仍然是 `200/80`。

### 4. 给按钮绑点击事件

```json
{
  "class": "/Script/UMG.Button",
  "name": "Btn_Start",
  "isVariable": true,
  "events": ["OnClicked"],
  "properties": {},
  "children": [ ]
}
```

- `events` 里写**委托属性名**（见控件类的委托，如 Button 的 `OnClicked`、`OnPressed`、`OnHovered`）
- 控件会被自动标成变量（事件绑定要求它是变量）
- 导入时自动生成蓝图事件节点，函数名由引擎决定（形如 `BndEvt__Btn_Start_OnClicked`）
- 导出时只回写**事件**绑定（`EBindingKind::Function`），属性绑定不会混进 `events`
- **`events` 只增不减**：从 JSON 里删掉某一项**不会**解除已有绑定；要解绑请在 Designer 里删事件节点。控件整体被删时，引擎会自动清理它的绑定
- 只想在蓝图里加事件节点、不需要改布局时，也可以改完 JSON 等资产重建后直接去蓝图里写逻辑

## 约束与注意

- 只能给容器控件（CanvasPanel、HorizontalBox、VerticalBox、Overlay、ScrollBox 等）加 `children`；单子容器（SizeBox、Border、Button、ScaleBox、NamedSlot 等）最多一个子控件
- 普通叶子控件（TextBlock、Image、Spacer、ProgressBar 等）不能有 `children`
- 删除 JSON 节点 = 删除控件；**删除 JSON 文件不会删除资产**（资产需要手动删除）
- 动画、BindWidget 暂不支持，后续版本提供
- `contentHash` 字段不要动；`schemaVersion` 必须为 1
