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
│                 WidgetJsonBridgeTests.cpp         # 自动化测试（3 个）
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

结果查 `<Project>/Saved/Logs/<Project>.log`。三个测试各自覆盖：

| 测试 | 覆盖范围 |
|---|---|
| `WidgetJsonBridge.RoundTrip` | 纯内存：构造 WidgetTree → 导出 JSON → 修改文本 → 导入 → 断言文本与 slot 位置复原 |
| `WidgetJsonBridge.ExportToFile` | 端到端：`Tick()` 从磁盘 JSON 导入资产 → 改文本 → 导出写回文件 → 断言落盘内容与导出结果一致 |
| `WidgetJsonBridge.MappingAndHash` | 资产/JSON 路径双向映射、越出 `/Game` 与 `UIWidgets` 的拒绝、`contentHash` 稳定性 |

全部通过时日志里会有：

```
LogAutomationCommandLine: Display: Found 3 automation tests based on 'WidgetJsonBridge'
LogAutomationController: Display: Test Completed. Result={Success} Name={RoundTrip} Path={WidgetJsonBridge.RoundTrip}
LogAutomationCommandLine: Display: ...Automation Test Queue Empty 3 tests performed.
```

测试会用到/产生真实资产：`UIWidgets/Test/SyncTest.json` ↔ `/Game/Test/SyncTest`。

> 注意：跑测试会**改写** `UIWidgets/Test/SyncTest.json`（`ExportToFile` 会把文本改成 `Editor Edited` 再导出），也会刷新 `Content/{Examples,Test}/*.uasset`。这是设计如此，不是副作用。

## 核心机制

- **路径映射**：`<项目>/UIWidgets/A/B.json` ↔ `/Game/A/B.uasset`（唯一真源关系见 Docs）
- **属性序列化**：导出 `CPF_Edit` 且非 Transient/Deprecated/BlueprintReadOnly/delegate 的属性；基础类型输出 JSON 原生值，结构体（颜色/字体/边距/刷子）用引擎 `ExportTextItem` 文本格式；导入走 `ImportText_InContainer`
- **hash 防回环**：`contentHash` 字段为树内容的规范化 MD5（key 排序）；导入器保存资产时通过 `ImportingPackageNames` 跳过反向导出；冲突时编辑器版本写 `xxx_editor_conflict.json` 并告警，不覆盖主文件
- **控件树构建**：`NewObject<UWidget>(WidgetTree, Class, MakeUniqueObjectName, RF_Transactional)`；容器用 `UPanelWidget::AddChild` / `UContentWidget::SetContent`；槽位属性写到返回的 `UPanelSlot` 上

## UE 5.8 适配要点（改代码时注意）

- 属性标志：5.8 移除了 `CPF_EditAnywhere/EditDefaultsOnly`，统一用 `CPF_Edit`
- 文本序列化：老 `ExportTextItem/ImportText` 已移除，用 `ExportTextItem_InContainer/ImportText_InContainer`（指针需包 `TNotNull`，头 `Misc/NotNull.h`）
- 属性值读写：`TProperty<...>::GetPropertyValue_InContainer/SetPropertyValue_InContainer`；`FByteProperty::Enum` 已私有，用 `GetIntPropertyEnum()`
- 保存事件：`UPackage::PackageSavedWithContextEvent`（3 参数，含 `FObjectPostSaveContext`，头 `UObject/ObjectSaveContext.h`）
- 保存资产：`UEditorLoadingAndSavingUtils::SavePackages`（头在 `FileHelpers.h`）
- `FJsonObject::Values` 键类型为 `FSharedString`（`ToView()` 转 `FString`）
- MD5：`FMD5::HashAnsiString` / `FMD5::HashBytes`
- `CreateAsset(ObjectName, PackagePath, ...)` 的 PackagePath 是**目录**不是资产全路径
- `-ExecutePythonScript` 模式下脚本执行完编辑器自动退出，验证脚本不能依赖跨 tick 等待

## 已知限制（迭代方向）

- 事件绑定、BindWidget、动画、跨 WB 命名槽引用未支持
- 导出含默认值属性（可做 CDO 差异过滤缩小 JSON）
- 删除 JSON 文件不会删除资产
- 引擎升级后需按新版本重新编译（插件 Binaries 不随 git）
