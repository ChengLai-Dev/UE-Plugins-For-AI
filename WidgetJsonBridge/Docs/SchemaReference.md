# WidgetJsonBridge Schema 参考（AI 编辑指南）

本插件将 UMG User Widget 与 JSON 文件双向同步。**JSON 文件是 AI 的编辑界面**。

## 同步机制

- 同步目录：`<项目根>/UIWidgets/`，文件路径映射资产路径：
  `UIWidgets/HUD/MainMenu.json` ↔ `/Game/HUD/MainMenu.uasset`
- AI 修改 JSON 文件 → 编辑器（运行中）自动检测并重新构建资产（约 1 秒轮询）
- 编辑器中手工修改 Widget 并保存（Ctrl+S）→ 自动回写 JSON
- 冲突（两边同时改）：编辑器版本存为 `xxx_editor_conflict.json`，主文件保留 AI 版本，人工合并
- 编辑器必须处于打开状态，同步才会发生

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
    "isVariable": true,
    "properties": { ... },
    "slot": null,
    "children": [ ... ]
  }
}
```

- `schemaVersion`：固定为 `1`
- `assetPath`：信息性字段，实际映射以文件路径为准
- `contentHash`：由插件维护，**AI 不要手动修改**（导入时忽略）
- `root`：根控件节点；`children` 递归嵌套

### 控件节点字段

| 字段 | 说明 |
|---|---|
| `class` | 类的完整对象路径。C++ 类如 `/Script/UMG.TextBlock`；蓝图类如 `/Game/UI/WBP_Avatar.WBP_Avatar_C` |
| `className` | 短名（如 `TextBlock`），仅信息性，导入忽略 |
| `name` | 控件名（唯一）。新建时若与既有控件重名会自动加后缀 |
| `isVariable` | 是否暴露为蓝图变量（bool） |
| `properties` | 可编辑属性表（见下） |
| `slot` | 父容器分配给它的槽位（根节点为 null）。`slot.properties` 里是槽位属性（位置/尺寸/对齐等） |
| `children` | 子控件数组 |

## 属性值格式

`properties` 是 `属性名 -> 值` 的映射。值按属性类型输出为 JSON 原生类型或文本：

- `bool` → JSON bool
- 整数/浮点 → JSON number
- 枚举 → 枚举名字符串（如 `"Left"`、`"SelfHitTestInvisible"`）
- `FText`/字符串/`FName` → 纯文本字符串（AI 直接写要显示的文本即可）
- 结构体（颜色、边距、刷子、字体等）→ 引擎文本格式字符串，例如：
  - 线性颜色 `"(R=1.000000,G=0.500000,B=0.000000,A=1.000000)"`
  - Slate 颜色 `"(SpecifiedColor=(R=1.000000,G=1.000000,B=1.000000,A=1.000000))"`
  - 边距 `"(Left=10.000000,Top=5.000000,Right=10.000000,Bottom=5.000000)"`
  - 纹理引用（在结构体内）`Texture2D'/Game/UI/Textures/T_Icon.T_Icon'`
- 对象引用 → 路径字符串，如 `"/Game/UI/Textures/T_Icon.T_Icon"` 或 `"None"`

规则：**导出哪个属性 AI 就能改哪个属性**。未知属性名会在导入时告警并跳过（不会报错中断）。

## 常用控件属性表

### TextBlock
| 属性 | 类型 | 说明 |
|---|---|---|
| `Text` | 文本 | 显示内容 |
| `Font.Size` | 数值 | 字号（在 Font 文本内） |
| `FontSize` | 数值 | 快捷字号覆盖（-1 时用 Font.Size） |
| `ColorAndOpacity` | 颜色 | 文字颜色 |
| `Justification` | 枚举 | 对齐：`Left`/`Center`/`Right` |
| `AutoWrapText` | bool | 自动换行 |
| `MinDesiredWidth` | 数值 | 最小期望宽度 |

### Image
| 属性 | 说明 |
|---|---|
| `Brush` | 含 `ResourceObject=Texture2D'/Game/...'` 与 `ImageSize=(X=..,Y=..)` |

### Button
| 属性 | 说明 |
|---|---|
| `ClickMethod` | 枚举：`DownAndUp`/`PreciseClick`/`MouseDown`/`MouseUp` |
| 背景/内容颜色 | `WidgetStyle` 或 `BackgroundColor` |

### SizeBox
| 属性 | 说明 |
|---|---|
| `WidthOverride` / `HeightOverride` | 数值 |
| `MinDesiredWidth` / `MinDesiredHeight` | 数值 |

### CanvasPanel 子控件的 slot（`slot.properties`）
| 属性 | 类型 | 说明 |
|---|---|---|
| `Position` | 文本 | `(X=100.000000,Y=50.000000)` |
| `Size` | 文本 | `(X=200.000000,Y=80.000000)` |
| `Anchors` | 文本 | `(Minimum=(X=0,Y=0),Maximum=(X=1,Y=1))` |
| `Alignment` | 文本 | `(X=0.5,Y=0.5)` |
| `ZOrder` | 数值 | 渲染顺序，越大越靠上 |

### HorizontalBox / VerticalBox 子控件的 slot
| 属性 | 说明 |
|---|---|
| `Size.Rule` | `Auto` 或 `Fill`（在 Size 文本内） |
| `HorizontalAlignment` / `VerticalAlignment` | 枚举 |

## 常见任务示例

### 1. 修改文字与字号
```json
"properties": {
  "Text": "开始游戏",
  "FontSize": 36
}
```

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
      "className": "TextBlock",
      "name": "Text_Start",
      "isVariable": false,
      "properties": { "Text": "开始" }
    }
  ]
}
```

### 3. 设置 CanvasPanel 子控件位置与锚点
```json
"slot": {
  "class": "/Script/UMG.CanvasPanelSlot",
  "className": "CanvasPanelSlot",
  "properties": {
    "Anchors": "(Minimum=(X=0.5,Y=0.5),Maximum=(X=0.5,Y=0.5))",
    "Alignment": "(X=0.5,Y=0.5)",
    "Position": "(X=-100.000000,Y=-40.000000)",
    "Size": "(X=200.000000,Y=80.000000)"
  }
}
```

## 约束与注意

- 只能给容器控件（CanvasPanel、HorizontalBox、VerticalBox、Overlay、ScrollBox 等）加 `children`；单子容器（SizeBox、Border、Button、ScaleBox、NamedSlot 等）最多一个子控件
- 普通叶子控件（TextBlock、Image、Spacer、ProgressBar 等）不能有 `children`
- 删除 JSON 节点 = 删除控件；**删除 JSON 文件不会删除资产**（资产需要手动删除）
- 事件绑定（OnClicked 等）、动画、BindWidget 暂不支持，后续版本提供
- `contentHash` 字段不要动；`schemaVersion` 必须为 1
