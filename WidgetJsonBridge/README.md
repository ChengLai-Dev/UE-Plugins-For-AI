# WidgetJsonBridge

UMG User Widget ↔ JSON 双向自动同步的 UE 编辑器插件。AI 通过编辑 JSON 文件读写 User Widget，编辑器内手工修改保存后自动回写 JSON。

```
AI 改 UIWidgets/**/*.json  ──ticker(1s)──> 重建 WidgetBlueprint 资产 ──> 保存
编辑器改 UMG ──Ctrl+S──> PackageSavedWithContextEvent ──hash 对比──> 回写 JSON / 写 .conflict
```

## 环境

- Unreal Engine 5.8
- 插件为纯 Editor 模块（`WidgetJsonBridgeEditor`），无 Runtime 代码
- 引入方式：在目标项目 `.uproject` 里用 `AdditionalPluginDirectories` 指向本插件所在目录，并启用 `WidgetJsonBridge`：

  ```json
  "AdditionalPluginDirectories": ["<plugins-root>"],
  "Plugins": [ { "Name": "WidgetJsonBridge", "Enabled": true } ]
  ```

  > **注意**：经 `AdditionalPluginDirectories` 引入的外部插件，编译产物落在**目标工程的** `Binaries/` 下，不在插件自己的目录里。判断插件是否已编译要看工程侧。

## 目录结构

```
WidgetJsonBridge/
├── WidgetJsonBridge.uplugin
├── Source/WidgetJsonBridgeEditor/
│   ├── Public/  WidgetJsonBridgeEditorModule.h, WidgetJsonSerializer.h
│   └── Private/ WidgetJsonBridgeEditorModule.cpp   # 模块入口：菜单/保存事件/Ticker
│                 WidgetJsonSerializer.cpp          # 核心：UWidgetTree ↔ JSON（反射序列化）
│                 WidgetJsonSync.cpp                # 同步调度：目录监听/hash 跟踪/冲突处理
│                 WidgetJsonBridgeTests.cpp         # 自动化测试（10 个）
└── Docs/
    ├── SchemaReference.md   # 给"用插件做 UI 的 AI"的 JSON 格式手册
    └── UserManual.md        # 用户使用手册
```

## 构建

```powershell
& "<engine>/Build/BatchFiles/Build.bat" <Project>Editor Win64 Development -Project="<path>/<Project>.uproject" -NoHotReloadFromIDE -WaitMutex
```

### 关于 `-NoHotReloadFromIDE`（实测必需）

UBT 有一个**按可执行文件路径命名的全局互斥体**检查（`HotReload.cs` 的 `CheckForLiveCodingSessionActive`）。因为所有 UE 编辑器都是同一个 `Engine/Binaries/Win64/UnrealEditor.exe`，所以**任何一个**开着 Live Coding 的编辑器都会锁死**所有**工程的编译——哪怕是完全无关的工程。

报错原文：

```
Unable to build while Live Coding is active. Exit the editor and game,
or press Ctrl+Alt+F11 if iterating on code in the editor or game
```

该检查的条件是 `BuildConfiguration.bAllowHotReloadFromIDE && ... && IsLiveCodingSessionActive(...)`；传 `-NoHotReloadFromIDE` 会把 `bAllowHotReloadFromIDE` 置为 `false` 直接跳过，**无需关闭编辑器，也不需要杀 LiveCodingConsole 进程**（互斥体是编辑器进程持有的，杀 console 没用）。

## 测试

```powershell
& "D:\Code\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:/Code/UE Projects/GreyCrown/GreyCrown.uproject" -unattended -nosplash -NullRHI -log -ExecCmds="Automation RunTests WidgetJsonBridge" -TestExit="Automation Test Queue Empty"
```

结果查 `<Project>/Saved/Logs/<Project>.log`。十个测试各自覆盖：

| 测试 | 覆盖范围 |
|---|---|
| `WidgetJsonBridge.RoundTrip` | 纯内存：构造 WidgetTree → 导出 JSON → 修改文本 → 导入 → 断言文本、slot 位置与控件名复原 |
| `WidgetJsonBridge.CanvasSlotLayout` | CanvasPanelSlot 语义：点锚点时 `offsets.right/bottom` 就是宽高、导入后 `GetPosition/GetSize` 与 JSON 一致；写成右下边界坐标（负宽度）时导入必须报错且旧树保留；拉伸锚点的负边距不被误判 |
| `WidgetJsonBridge.ExportToFile` | 端到端：`Tick()` 从磁盘 JSON 导入资产 → 改文本 → 导出写回文件 → 断言落盘内容与导出结果一致 |
| `WidgetJsonBridge.MappingAndHash` | 资产/JSON 路径双向映射、越出 `/Game` 与 `UIWidgets` 的拒绝（含同前缀兄弟目录）、`contentHash` 稳定性 |
| `WidgetJsonBridge.ImportFailureKeepsTree` | 原子性：坏 JSON 导入失败后，旧 WidgetTree 与控件属性保持原样 |
| `WidgetJsonBridge.ExportSchema` | 属性 Schema 按类分文件导出，索引与单类文件内容正确 |
| `WidgetJsonBridge.EventBinding` | `events` 声明自动置变量；已有绑定表被导出成 `events` 且导入后不清空 |
| `WidgetJsonBridge.Utf8RoundTrip` | 中文文本导出/导入往返正确 |
| `WidgetJsonBridge.StructObjectMode` | `structMode: object` 下 `FProgressBarStyle`→`FSlateBrush` 递归对象导出/导入（DrawAs/Tiling/Mirroring/ImageType/ImageSize/Margin/ResourceName/ResourceObject/CornerRadii/ColorUseRule/EnableFillAnimation）；无 `structMode` 的对象 JSON 仍可导入；导出入口按文件 `structMode` 选模式；非法 `structMode` 退回字符串并告警；结构体对象里的非法字段名报错 |
| `WidgetJsonBridge.HashNoFalseLoop` | 导出后再次 tick 不会因 hash 编码域不一致而重复导入（C1 的回归保护） |

全部通过时日志里会有：

```
LogAutomationCommandLine: Display: Found 10 automation tests based on 'WidgetJsonBridge'
LogAutomationController: Display: Test Completed. Result={Success} Name={RoundTrip} Path={WidgetJsonBridge.RoundTrip}
LogAutomationCommandLine: Display: ...Automation Test Queue Empty 10 tests performed.
```

测试会用到/产生真实资产：`UIWidgets/Test/SyncTest.json` ↔ `/Game/Test/SyncTest`。

> 注意：跑测试会**改写** `UIWidgets/Test/SyncTest.json`（`ExportToFile` 会把文本改成 `Editor Edited` 再导出），也会刷新 `Content/{Examples,Test}/*.uasset`。这是设计如此，不是副作用。

## 核心机制

- **路径映射**：`<项目>/UIWidgets/A/B.json` ↔ `/Game/A/B.uasset`（唯一真源关系见 Docs）
- **属性序列化**：导出 `CPF_Edit` 且非 Transient/Deprecated/BlueprintReadOnly/delegate 的属性；基础类型输出 JSON 原生值，白名单结构体（`FVector2D`/`FLinearColor`/`FMargin`/`FAnchors`/`FAnchorData`/`FSlateChildSize`）输出原生对象，其余结构体用引擎 `ExportTextItem` 文本格式；导入走 `ImportText_InContainer`，并在与类默认值相同时跳过导出
- **`designSizeMode`（顶层可选字段）**：写的是 `UUserWidget::DesignSizeMode`（生成类 CDO，编辑器专用），控制设计器工具栏里 Fill Screen / Custom / CustomOnScreen / Desired / DesiredOnScreen 那个下拉。它不在 WidgetTree 里，所以导入时先校验、编译重建 CDO 之后再写入，导出时回写；值不合法按严格校验直接判失败
- **hash 防回环**：`contentHash` 字段为树内容的规范化 MD5（key 排序，按 UTF-8 字节计算）；导入器保存资产时通过 `ImportingPackageNames` 跳过反向导出；导出后用磁盘字节重算 hash，与 ticker 侧同处 UTF-8 编码域（早期版本误用 `HashAnsiString`，会让中文 JSON 每次保存都触发假回环 + 假冲突）；冲突时编辑器版本写 `xxx_editor_conflict.json` 并告警，不覆盖主文件
- **冲突文件不参与同步**：`*_editor_conflict.json` 会被 ticker 跳过，不会被当成新界面导入成垃圾资产
- **原子导入**：新树先在临时 `UWidgetTree` 上完整构建，全部成功后才移除旧树并迁移新控件；构建中途失败（含属性级错误）时旧树完全未被触碰，资产不会停在半损坏状态
- **严格属性校验**：未知属性名、值格式错误、单子容器塞多个子控件、抽象/废弃类，都会让整次导入失败并写入错误报告，不再"打条 warning 继续"
- **Canvas 槽位语义与引擎一致**：`LayoutData.offsets` 按轴解释——锚点重合的轴上 `right/bottom` 是**宽/高**（Designer 里的 `Size X/Size Y`），锚点拉伸的轴上是距右/下的边距。点锚点轴上出现负的 `right/bottom`（典型错误：把它当成右下边界坐标）会让整次导入失败，报告里直接给出换算后的正确值（`WidgetJsonBridge.CanvasSlotLayout` 回归保护）
- **可撤销**：`Blueprint->Modify()` + `WidgetTree->Modify()` 在任何破坏性操作之前调用，控件 Rename 也纳入事务，整次导入包在 `FScopedTransaction` 里。（Ctrl+Z 路径尚未做 GUI 自动化实测）
- **失败可见**：导入失败写 `<项目>/Saved/WidgetJsonBridge/Errors/<相对路径>.error.json`（含 `jsonPath` / `assetPath` / `error` / `timestamp`），成功时自动删除；同一份失败内容不会每秒重试，文件一改动即重试（写了一半的文件不会被误判为永久失败）
- **编译失败不落盘**：`CompileBlueprint` 的错误会被检查，编译不过时不保存资产、也不把该 JSON 标记为已同步
- **控件名稳定**：`MakeUniqueObjectName` 保证返回值不等于传入名（会无条件加后缀），因此按 JSON 原名直接创建，只有同 Outer 下真重名才回退到唯一名生成；迁移时若仍被改名会告警。名字稳定是动画绑定与蓝图绑定表能存活的前提
- **变量 GUID 交给引擎**：插件不再自己维护 `WidgetVariableNameToGuidMap`（早期版本整体 Reset 再重建，会打乱动画 GUID 并触发引擎 ensure）。编译期由 `FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids` 统一补齐与清理
- **CDO 差异过滤**：与类默认值相同的属性不写进 JSON，一个 TextBlock 从 36 个字段降到个位数
- **结构体原生 JSON**：`FVector2D` / `FLinearColor` / `FMargin` / `FAnchors` / `FAnchorData` / `FSlateChildSize` 支持对象写法，导入端同时兼容旧的引擎文本格式
- **结构体对象模式（`structMode`）**：顶层写 `"structMode": "object"` 后，其余 `FStructProperty`（如 `FProgressBarStyle`、`FSlateBrush`、`FSlateFontInfo`）也会递归导出为 JSON 对象，字段名就是 C++ 属性名，对象引用写资产路径字符串；导入端不依赖该字段，JSON 是对象就按对象解析、是字符串就按 `ImportText` 解析，旧文件无需迁移。**AI 写对象结构体时必须同时写顶层 `structMode: object`，否则编辑器下次保存会把对象改回字符串。** 容器字段暂回退为引擎文本字符串。该字段缺省时行为与旧版完全一致
- **导出模式跟随文件**：`WidgetJsonSync` 导出前读取目标 JSON 顶层的 `structMode` 再传给 `ExportToJson`，手动导出与 Ctrl+S 自动回写都生效；文件不存在或字段缺省按旧字符串模式
- **事件绑定**：JSON 节点上的 `events: ["OnClicked"]` 会通过 `FWidgetBlueprintOperationUtils::BindToEventProperty` 生成蓝图事件节点，并自动把控件标成变量；导出时只回写 `EBindingKind::Function` 的条目（属性绑定不会被误当成事件）
- **属性 Schema**：菜单 `Export Property Schema`（或 `WidgetJsonBridge.ExportSchema`）按类导出真实 UPROPERTY 清单到 `Saved/WidgetJsonBridge/PropertySchema/`，AI 查表而不是猜
- **PIE / cook 防护**：PIE 期间不重建资产；cook 期间的保存事件不反写项目 JSON
- **控件树构建**：`NewObject<UWidget>(WidgetTree, Class, DesiredName, RF_Transactional)`，按 JSON 原名创建；容器用 `UPanelWidget::AddChild`，槽位属性写到返回的 `UPanelSlot` 上

## UE 5.8 适配要点（改代码时注意）

- 属性标志：5.8 移除了 `CPF_EditAnywhere/EditDefaultsOnly`，统一用 `CPF_Edit`
- 文本序列化：老 `ExportTextItem/ImportText` 已移除，用 `ExportTextItem_InContainer/ImportText_InContainer`（指针需包 `TNotNull`，头 `Misc/NotNull.h`）
- 属性值读写：`TProperty<...>::GetPropertyValue_InContainer/SetPropertyValue_InContainer`；`FByteProperty::Enum` 已私有，用 `GetIntPropertyEnum()`
- 保存事件：`UPackage::PackageSavedWithContextEvent`（3 参数，含 `FObjectPostSaveContext`，头 `UObject/ObjectSaveContext.h`）
- 保存资产：`UEditorLoadingAndSavingUtils::SavePackages`（头在 `FileHelpers.h`）
- `FJsonObject::Values` 键类型为 `FSharedString`（`ToView()` 转 `FString`）
- MD5：**不要用 `FMD5::HashAnsiString` 算要落盘/比对的内容**——它走 `TCHAR_TO_ANSI`，非 ASCII 会被降级成 `?`，导致导出与读取两侧 hash 对不上。一律用 `FMD5::HashBytes` 配 `FTCHARToUTF8`，或直接对磁盘字节算
- `CreateAsset(ObjectName, PackagePath, ...)` 的 PackagePath 是**目录**不是资产全路径
- `-ExecutePythonScript` 模式下脚本执行完编辑器自动退出，验证脚本不能依赖跨 tick 等待

## 已知限制（迭代方向）

- 动画（WidgetAnimation）、BindWidget、跨 WBP 命名槽引用尚未支持
- 全量重建 WidgetTree：JSON 里声明的事件会被重新创建，但**只在编辑器里手工添加、且 JSON 里没有对应 `events` 声明的绑定**，在结构大改后可能失效
- 删除 JSON 文件不会删除资产
- 引擎升级后需按新版本重新编译（插件 Binaries 不随 git）
