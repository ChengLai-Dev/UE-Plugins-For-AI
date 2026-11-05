#include "WidgetJsonSync.h"

#include "WidgetJsonSerializer.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "WidgetBlueprintOperationUtils.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "FileHelpers.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "Components/Widget.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Components/PanelSlot.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/IConsoleManager.h"

FWidgetJsonSync& FWidgetJsonSync::Get()
{
	static FWidgetJsonSync Instance;
	return Instance;
}

FString FWidgetJsonSync::GetSyncRootDir()
{
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("UIWidgets"));
}

bool FWidgetJsonSync::AssetPathToJsonPath(const FString& AssetPath, FString& OutJsonPath)
{
	static const FString GameRoot(TEXT("/Game/"));
	if (!AssetPath.StartsWith(GameRoot))
	{
		return false;
	}
	const FString Relative = AssetPath.RightChop(GameRoot.Len());
	OutJsonPath = FPaths::Combine(GetSyncRootDir(), Relative) + TEXT(".json");
	return true;
}

namespace
{
	/** 规范化后判断路径是否真的落在目录内，避免 "UIWidgets_backup" 被当成 "UIWidgets" 的子路径。 */
	bool IsPathUnderDirectory(const FString& InPath, const FString& InDirectory)
	{
		FString Path = InPath;
		FString Directory = InDirectory;
		FPaths::NormalizeFilename(Path);
		FPaths::NormalizeFilename(Directory);
		Directory.RemoveFromEnd(TEXT("/"));
		if (!Path.StartsWith(Directory))
		{
			return false;
		}
		if (Path.Len() == Directory.Len())
		{
			return true;
		}
		return Path[Directory.Len()] == TEXT('/');
	}

	struct FDeclaredEventBinding
	{
		FName WidgetName;
		FName DelegateName;
	};

	/**
	 * 读现有 JSON 顶层的 structMode，决定导出时是否把非白名单结构体写成对象。
	 * 文件不存在、解析失败或字段缺省都按旧的 "string" 模式处理，不动已有 JSON。
	 * 字段存在但类型/取值非法时打 Warning，避免 AI 写了对象却因模式无效被静默改回字符串。
	 */
	bool ReadStructModeFromJsonFile(const FString& JsonPath, bool& bOutStructsAsObjects)
	{
		bOutStructsAsObjects = false;

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
		{
			return false;
		}

		TSharedPtr<FJsonObject> RootObj;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonValue> ModeValue = RootObj->TryGetField(TEXT("structMode"));
		if (!ModeValue.IsValid() || ModeValue->IsNull())
		{
			return false;
		}

		FString ModeText;
		if (!ModeValue->TryGetString(ModeText))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("WidgetJsonBridge: %s 的 structMode 不是字符串，按 string 模式处理"),
				*JsonPath);
			return false;
		}

		if (ModeText.Equals(TEXT("object"), ESearchCase::IgnoreCase))
		{
			bOutStructsAsObjects = true;
			return true;
		}
		if (ModeText.Equals(TEXT("string"), ESearchCase::IgnoreCase))
		{
			bOutStructsAsObjects = false;
			return true;
		}

		UE_LOG(LogTemp, Warning,
			TEXT("WidgetJsonBridge: %s 的 structMode 值 '%s' 无效（可选 string / object），按 string 模式处理"),
			*JsonPath,
			*ModeText);
		return false;
	}

	/** 深度优先遍历 JSON 树，收集所有节点上的 events 声明。 */
	void CollectDeclaredEventsFromNode(const TSharedPtr<FJsonObject>& Node, TArray<FDeclaredEventBinding>& Out)
	{
		if (!Node.IsValid())
		{
			return;
		}

		FString WidgetName;
		Node->TryGetStringField(TEXT("name"), WidgetName);

		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (!WidgetName.IsEmpty() && Node->TryGetArrayField(TEXT("events"), Events) && Events)
		{
			for (const TSharedPtr<FJsonValue>& EventValue : *Events)
			{
				FString DelegateName;
				if (EventValue.IsValid() && EventValue->TryGetString(DelegateName) && !DelegateName.IsEmpty())
				{
					FDeclaredEventBinding Binding;
					Binding.WidgetName = FName(*WidgetName);
					Binding.DelegateName = FName(*DelegateName);
					Out.Add(Binding);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (Node->TryGetArrayField(TEXT("children"), Children) && Children)
		{
			for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
			{
				const TSharedPtr<FJsonObject>* ChildObj = nullptr;
				if (ChildValue.IsValid() && ChildValue->TryGetObject(ChildObj) && ChildObj)
				{
					CollectDeclaredEventsFromNode(*ChildObj, Out);
				}
			}
		}
	}
}

bool FWidgetJsonSync::JsonPathToAssetPath(const FString& JsonPath, FString& OutAssetPath)
{
	const FString FullRoot = FPaths::ConvertRelativePathToFull(GetSyncRootDir());
	const FString FullJson = FPaths::ConvertRelativePathToFull(JsonPath);
	if (!IsPathUnderDirectory(FullJson, FullRoot))
	{
		return false;
	}

	FString NormalizedRoot = FullRoot;
	FString NormalizedJson = FullJson;
	FPaths::NormalizeFilename(NormalizedRoot);
	FPaths::NormalizeFilename(NormalizedJson);
	NormalizedRoot.RemoveFromEnd(TEXT("/"));

	FString Relative = NormalizedJson.RightChop(NormalizedRoot.Len() + 1);
	Relative = FPaths::ChangeExtension(Relative, TEXT(""));
	OutAssetPath = TEXT("/Game/") + Relative;
	return true;
}

FString FWidgetJsonSync::GetFileHash(const FString& FilePath) const
{
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
	{
		return FString();
	}
	return FMD5::HashBytes(FileBytes.GetData(), FileBytes.Num());
}

void FWidgetJsonSync::Tick()
{
	IFileManager& FileManager = IFileManager::Get();
	const FString Root = GetSyncRootDir();
	if (!FileManager.DirectoryExists(*Root))
	{
		return;
	}
	TArray<FString> JsonFiles;
	FileManager.FindFilesRecursive(JsonFiles, *Root, TEXT("*.json"), true, false);
	for (const FString& JsonFile : JsonFiles)
	{
		// 冲突文件不是界面定义，绝不能被当成新界面导入（否则会凭空造出 xxx_editor_conflict 资产，
		// 然后它的 Ctrl+S 又写回同名 JSON，形成自我维持的垃圾）。
		if (JsonFile.EndsWith(TEXT("_editor_conflict.json"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString Hash = GetFileHash(JsonFile);
		if (Hash.IsEmpty())
		{
			continue;
		}
		const FString* OldHash = SyncedFileHash.Find(JsonFile);
		if (OldHash && *OldHash == Hash)
		{
			continue;
		}

		// 同一份内容已经导入失败过就不再重试，避免坏 JSON 每秒刷一次日志；
		// 文件一改动 hash 就变，会自然重新尝试。
		const FString* FailedHash = FailedFileHash.Find(JsonFile);
		if (FailedHash && *FailedHash == Hash)
		{
			continue;
		}

		if (ImportJsonFile(JsonFile))
		{
			// 导入只写资产、不写 JSON，所以这里复用导入前算出的 hash，省掉一次全文件读取。
			SyncedFileHash.Add(JsonFile, Hash);
			FailedFileHash.Remove(JsonFile);
		}
		else
		{
			SyncedFileHash.Remove(JsonFile);

			// 失败可能只是因为读到了 AI 写了一半的文件。重算 hash：若与本轮快照不同，
			// 说明文件还在变化，本轮不记失败，下一 tick 重试；内容确实稳定才进入冷却。
			const FString CurrentHash = GetFileHash(JsonFile);
			if (CurrentHash == Hash)
			{
				FailedFileHash.Add(JsonFile, Hash);
			}
			else
			{
				FailedFileHash.Remove(JsonFile);
			}
		}
	}
}

void FWidgetJsonSync::ImportAll()
{
	SyncedFileHash.Reset();
	FailedFileHash.Reset();
	Tick();
}

bool FWidgetJsonSync::ImportJsonFile(const FString& JsonFilePath)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *JsonFilePath))
	{
		ReportImportFailure(JsonFilePath, FString(), TEXT("读取 JSON 文件失败"));
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 读取 JSON 失败 %s"), *JsonFilePath);
		return false;
	}

	FString AssetPath;
	if (!JsonPathToAssetPath(JsonFilePath, AssetPath))
	{
		ReportImportFailure(JsonFilePath, FString(), TEXT("JSON 路径无法映射到资产路径"));
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: JSON 路径无法映射到资产路径 %s"), *JsonFilePath);
		return false;
	}

	// 资产创建与导入包在同一个事务里，Ctrl+Z 可以整步撤销。
	TUniquePtr<FScopedTransaction> Transaction;
	if (GEditor)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("WidgetJsonBridge", "ImportWidgetJson", "Import Widget JSON"));
	}

	UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		const FString PackagePath = FPaths::GetPath(AssetPath);
		const FString ObjectName = FPaths::GetBaseFilename(AssetPath);
		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->ParentClass = UUserWidget::StaticClass();
		UObject* NewAsset = AssetTools.CreateAsset(ObjectName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
		Blueprint = Cast<UWidgetBlueprint>(NewAsset);
		if (!Blueprint)
		{
			ReportImportFailure(JsonFilePath, AssetPath, TEXT("创建资产失败"));
			UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 创建资产失败 %s"), *AssetPath);
			return false;
		}
	}

	FString Error;
	FString RequestedDesignSizeMode;
	if (!FWidgetJsonSerializer::ImportFromJson(Blueprint, JsonText, Error, &RequestedDesignSizeMode))
	{
		ReportImportFailure(JsonFilePath, AssetPath, Error);
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 导入失败 %s : %s"), *JsonFilePath, *Error);
		return false;
	}

	// GUID 表交给引擎在编译期维护（FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids
	// 会为缺失项补 GUID、剔除已删除项，并同时覆盖控件变量与动画变量）。
	// 但它对"缺 GUID"的控件会打 ensureAlwaysMsgf，所以这里先把新控件登记进去。
	// OnVariableAdded 内部是 Add(Name, NewGuid()) 且要求名字未登记，因此必须先判断再调。
	// 已经有 GUID 的条目（含动画）一律不动，避免跨蓝图引用失去稳定标识。
	if (Blueprint->WidgetTree)
	{
		TArray<UWidget*> AllWidgets;
		Blueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			const FName WidgetName = Widget->GetFName();
			if (!Blueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
			{
				Blueprint->OnVariableAdded(WidgetName);
			}
		}
	}

	// 编译失败时不要把坏资产存盘，也不要把该 JSON 标记为已同步。
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);
	if (CompileResults.NumErrors > 0)
	{
		Error = FString::Printf(TEXT("WidgetBlueprint 编译失败（%d 个错误），详见 Output Log"), CompileResults.NumErrors);
		// 光"不落盘"不够：树已经换进内存了，不取消事务的话用户 Save All 仍会把坏树写出去。
		if (Transaction.IsValid())
		{
			Transaction->Cancel();
		}
		// 撤销的是 WidgetBlueprint 快照，不会回滚已经跑完的编译；补一次让 GeneratedClass 回到旧树。
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		ReportImportFailure(JsonFilePath, AssetPath, Error);
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 编译失败 %s : %s"), *JsonFilePath, *Error);
		return false;
	}

	// 命名检查：Hierarchy 显示名必须等于对象名，否则照着面板写字符串路径就会找不到控件。
	// 只做诊断：导入时已经给每个控件 SetDisplayLabel(对象名)，这里正常应该 0 命中。
	// 不在这里中断导入——FScopedTransaction::Cancel() 只把事务踢出撤销栈、并不回滚已换上的树
	// （EditorTransaction.cpp 的 UTransBuffer::Cancel 不调 Apply），中断反而会留下「坏树 + 脏包」。
	// 默认只把「变量控件」的不一致记为 Error（蓝图会按名字引用它），其余记 Warning；
	// WidgetJsonBridge.StrictNameCheck 1 可把所有不一致升级为 Error。
	ValidateDisplayNames(Blueprint, TEXT("Import"));

	// 编译之后 SkeletonGeneratedClass 里才有控件变量，BindToEventProperty 依赖它。
	BindDeclaredEvents(Blueprint, JsonText);

	// designSizeMode 落在生成类 CDO 上，必须在 CompileBlueprint 重建 CDO 之后再写，否则会被编译覆盖。
	if (!RequestedDesignSizeMode.IsEmpty())
	{
		FString DesignModeError;
		if (!FWidgetJsonSerializer::SetDesignSizeMode(Blueprint, RequestedDesignSizeMode, DesignModeError))
		{
			UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: designSizeMode 应用失败 %s : %s"),
				*JsonFilePath, *DesignModeError);
		}
	}

	const FString PackageName = Blueprint->GetOutermost()->GetName();
	ImportingPackageNames.Add(PackageName);
	{
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Blueprint->GetOutermost());
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	}
	ImportingPackageNames.Remove(PackageName);

	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 已导入 %s -> %s"), *JsonFilePath, *AssetPath);
	++ImportCount;
	ClearImportFailure(JsonFilePath);
	return true;
}

FString FWidgetJsonSync::GetImportErrorReportPath(const FString& JsonFilePath)
{
	FString NormalizedRoot = FPaths::ConvertRelativePathToFull(GetSyncRootDir());
	FString NormalizedJson = FPaths::ConvertRelativePathToFull(JsonFilePath);
	FPaths::NormalizeFilename(NormalizedRoot);
	FPaths::NormalizeFilename(NormalizedJson);
	NormalizedRoot.RemoveFromEnd(TEXT("/"));

	FString Relative = NormalizedJson;
	const FString RootPrefix = NormalizedRoot + TEXT("/");
	if (Relative.StartsWith(RootPrefix))
	{
		Relative.RightChopInline(RootPrefix.Len());
	}
	Relative.ReplaceInline(TEXT("/"), TEXT("__"));

	const FString ErrorDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WidgetJsonBridge"), TEXT("Errors"));
	return FPaths::Combine(ErrorDir, FPaths::ChangeExtension(Relative, FString(TEXT("error.json"))));
}

void FWidgetJsonSync::ReportImportFailure(const FString& JsonFilePath, const FString& AssetPath, const FString& Error)
{
	const FString ErrorPath = GetImportErrorReportPath(JsonFilePath);
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(ErrorPath));

	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("jsonPath"), JsonFilePath);
	Report->SetStringField(TEXT("assetPath"), AssetPath);
	Report->SetStringField(TEXT("error"), Error);
	Report->SetStringField(TEXT("timestamp"), FDateTime::Now().ToIso8601());

	FString ReportText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportText);
	FJsonSerializer::Serialize(Report.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(ReportText, *ErrorPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FWidgetJsonSync::ClearImportFailure(const FString& JsonFilePath)
{
	const FString ErrorPath = GetImportErrorReportPath(JsonFilePath);
	IFileManager::Get().Delete(*ErrorPath, false, true, true);
}

bool FWidgetJsonSync::ExportPropertySchemaToFile(FString& OutFilePath, FString& OutError)
{
	TArray<UClass*> WidgetClasses;
	GetDerivedClasses(UWidget::StaticClass(), WidgetClasses, true);
	WidgetClasses.Add(UWidget::StaticClass());

	TArray<UClass*> SlotClasses;
	GetDerivedClasses(UPanelSlot::StaticClass(), SlotClasses, true);
	SlotClasses.Add(UPanelSlot::StaticClass());

	if (WidgetClasses.Num() == 0)
	{
		OutError = TEXT("没有收集到任何控件类，UMG 模块可能尚未加载");
		return false;
	}

	const FString OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WidgetJsonBridge"), TEXT("PropertySchema"));
	const FString WidgetDir = FPaths::Combine(OutDir, TEXT("widgets"));
	const FString SlotDir = FPaths::Combine(OutDir, TEXT("slots"));
	IFileManager& FileManager = IFileManager::Get();
	FileManager.MakeDirectory(*WidgetDir, true);
	FileManager.MakeDirectory(*SlotDir, true);

	// 旧版本写过单文件，清掉避免和分文件结构混淆。
	FileManager.Delete(*FPaths::Combine(OutDir, TEXT("PropertySchema.json")), false, true, true);

	// 单文件会到几百 KB，AI 一次读不动。按类拆文件，AI 只读它要用的那几个类。
	auto SaveSchemaFile = [](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	};

	TArray<TSharedPtr<FJsonValue>> WidgetIndex;
	int32 WidgetCount = 0;
	for (UClass* WidgetClass : WidgetClasses)
	{
		TSharedPtr<FJsonObject> ClassSchema;
		if (!FWidgetJsonSerializer::ExportClassSchema(WidgetClass, ClassSchema))
		{
			continue;
		}
		const FString ClassName = WidgetClass->GetName();
		const FString ClassPath = FPaths::Combine(WidgetDir, ClassName + TEXT(".json"));
		if (!SaveSchemaFile(ClassPath, ClassSchema))
		{
			continue;
		}
		++WidgetCount;
		WidgetIndex.Add(MakeShared<FJsonValueString>(ClassName));
	}

	TArray<TSharedPtr<FJsonValue>> SlotIndex;
	int32 SlotCount = 0;
	for (UClass* SlotClass : SlotClasses)
	{
		TSharedPtr<FJsonObject> ClassSchema;
		if (!FWidgetJsonSerializer::ExportClassSchema(SlotClass, ClassSchema))
		{
			continue;
		}
		const FString ClassName = SlotClass->GetName();
		const FString ClassPath = FPaths::Combine(SlotDir, ClassName + TEXT(".json"));
		if (!SaveSchemaFile(ClassPath, ClassSchema))
		{
			continue;
		}
		++SlotCount;
		SlotIndex.Add(MakeShared<FJsonValueString>(ClassName));
	}

	if (WidgetCount == 0)
	{
		OutError = TEXT("没有写出任何控件类 Schema 文件");
		return false;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("generatedAt"), FDateTime::Now().ToIso8601());
	Root->SetStringField(TEXT("note"),
		TEXT("属性清单来自 UWidget/UPanelSlot 的真实 UPROPERTY，比任何手写文档可靠。")
		TEXT("format=object 表示可写原生 JSON 对象；format=string 表示必须写引擎文本格式字符串。")
		TEXT("按类名到 widgets/ 或 slots/ 下读对应文件。"));
	Root->SetArrayField(TEXT("widgetClasses"), WidgetIndex);
	Root->SetArrayField(TEXT("slotClasses"), SlotIndex);

	OutFilePath = FPaths::Combine(OutDir, TEXT("index.json"));
	if (!SaveSchemaFile(OutFilePath, Root))
	{
		OutError = FString::Printf(TEXT("写入 Schema 索引失败：%s"), *OutFilePath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 属性 Schema 已导出到 %s（%d 个控件类 / %d 个槽类）"),
		*OutDir, WidgetCount, SlotCount);
	return true;
}

void FWidgetJsonSync::BindDeclaredEvents(UWidgetBlueprint* Blueprint, const FString& JsonText)
{
	if (!Blueprint || !Blueprint->WidgetTree || !Blueprint->WidgetTree->RootWidget)
	{
		return;
	}

	TSharedPtr<FJsonObject> RootObj;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* RootWidgetObj = nullptr;
	if (!RootObj->TryGetObjectField(TEXT("root"), RootWidgetObj) || !RootWidgetObj)
	{
		return;
	}

	TArray<FDeclaredEventBinding> Declarations;
	CollectDeclaredEventsFromNode(*RootWidgetObj, Declarations);
	if (Declarations.Num() == 0)
	{
		return;
	}

	for (const FDeclaredEventBinding& Declaration : Declarations)
	{
		UWidget* Widget = Blueprint->WidgetTree->FindWidget(Declaration.WidgetName);
		if (!Widget)
		{
			UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 事件 %s 的目标控件 %s 不存在，已跳过"),
				*Declaration.DelegateName.ToString(), *Declaration.WidgetName.ToString());
			continue;
		}

		FText BindError;
		if (!FWidgetBlueprintOperationUtils::BindToEventProperty(
			Blueprint,
			Declaration.DelegateName,
			Declaration.WidgetName,
			Widget->GetClass(),
			false,
			BindError))
		{
			// 已经绑过的条目官方 API 会返回 false 并说明 already bound，这里只记 Verbose。
			UE_LOG(LogTemp, Verbose, TEXT("WidgetJsonBridge: 事件 %s.%s 未新建：%s"),
				*Declaration.WidgetName.ToString(),
				*Declaration.DelegateName.ToString(),
				*BindError.ToString());
		}
	}
}

void FWidgetJsonSync::OnAssetSaved(UWidgetBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}
	if (ImportingPackageNames.Contains(Blueprint->GetOutermost()->GetName()))
	{
		return;
	}
	FString JsonPath;
	if (!AssetPathToJsonPath(Blueprint->GetOutermost()->GetName(), JsonPath))
	{
		return;
	}
	FString Error;
	if (ExportAssetInternal(Blueprint, JsonPath, true, Error))
	{
		// 设计器里改一次名，引擎就会把 DisplayLabel 设成新名字，和对象名可能分叉；
		// 保存时顺手检查一遍，让问题在保存那一刻就暴露。
		ValidateDisplayNames(Blueprint, TEXT("Save"));
	}
}

namespace
{
	/** 1 = 所有控件「Hierarchy 显示名 != 对象名」都记为 Error；0 = 只有变量控件算 Error。 */
	TAutoConsoleVariable<int32> CVarStrictNameCheck(
		TEXT("WidgetJsonBridge.StrictNameCheck"),
		0,
		TEXT("1 = 所有控件「Hierarchy 显示名 != 对象名」都算错误（默认 0：只有变量控件算错误）"),
		ECVF_Default);
}

int32 FWidgetJsonSync::ValidateDisplayNames(UWidgetBlueprint* Blueprint, const TCHAR* Context)
{
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		return 0;
	}

	TArray<UWidget*> AllWidgets;
	Blueprint->WidgetTree->GetAllWidgets(AllWidgets);

	const bool bStrict = CVarStrictNameCheck.GetValueOnGameThread() != 0;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	for (UWidget* Widget : AllWidgets)
	{
		if (!Widget)
		{
			continue;
		}

		const FString ObjectName = Widget->GetName();
		const FString ShownName = Widget->GetLabelText().ToString();
		if (ShownName == ObjectName)
		{
			continue;
		}

		// GetLabelText() 与对象名分叉只有两种来源：DisplayLabel 非空，或被判定为自动生成的类名。
		const FString Reason = Widget->IsGeneratedName()
			? TEXT("名字被判定为自动生成（等于类名或以「类名_」开头），面板显示类名")
			: TEXT("DisplayLabel 与对象名不一致");

		const FString Message = FString::Printf(
			TEXT("%s：控件 %s 的 Hierarchy 显示名是「%s」（%s），代码按对象名查找会失败"),
			*Blueprint->GetName(), *ObjectName, *ShownName, *Reason);

		// 变量控件会被蓝图按名字引用，名字分叉是真错误；装饰节点只影响可读性
		if (bStrict || Widget->bIsVariable)
		{
			++ErrorCount;
			UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge[%s]: %s"), Context, *Message);
		}
		else
		{
			++WarningCount;
			UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge[%s]: %s"), Context, *Message);
		}
	}

	if (ErrorCount + WarningCount > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge[%s]: %s 命名检查：%d 个错误、%d 个提示"),
			Context, *Blueprint->GetName(), ErrorCount, WarningCount);
	}

	return ErrorCount;
}

int32 FWidgetJsonSync::ValidateAllWidgetBlueprints()
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// 只查 /Game：引擎/插件自带的 WBP 不归这套桥接管，全量加载它们既慢又会制造误报。
	FARFilter Filter;
	Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	int32 ErrorCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Asset.GetAsset());
		if (!Blueprint)
		{
			continue;
		}
		ErrorCount += ValidateDisplayNames(Blueprint, TEXT("Validate"));
	}

	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge[Validate]: 检查了 %d 个 Widget Blueprint，%d 个 Error"),
		Assets.Num(), ErrorCount);
	return ErrorCount;
}

bool FWidgetJsonSync::ExportAsset(UWidgetBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("WidgetBlueprint 为空");
		return false;
	}
	FString JsonPath;
	if (!AssetPathToJsonPath(Blueprint->GetOutermost()->GetName(), JsonPath))
	{
		OutError = FString::Printf(TEXT("资产 %s 不在 /Game 下，无法映射到 UIWidgets"), *Blueprint->GetOutermost()->GetName());
		return false;
	}
	return ExportAssetInternal(Blueprint, JsonPath, false, OutError);
}

bool FWidgetJsonSync::ExportToJsonForPath(UWidgetBlueprint* Blueprint, const FString& JsonPath, FString& OutJsonText, FString& OutError)
{
	// 导出模式跟随磁盘上已有的 JSON：只有它声明了 structMode=object 才写对象，
	// 旧文件缺省仍是引擎文本字符串。自动保存与手动导出都走这里。
	bool bStructsAsObjects = false;
	ReadStructModeFromJsonFile(JsonPath, bStructsAsObjects);
	return FWidgetJsonSerializer::ExportToJson(Blueprint, OutJsonText, OutError, bStructsAsObjects);
}

bool FWidgetJsonSync::ExportAssetInternal(UWidgetBlueprint* Blueprint, const FString& JsonPath, bool bAllowConflictFile, FString& OutError)
{
	FString JsonText;
	if (!ExportToJsonForPath(Blueprint, JsonPath, JsonText, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 导出失败 %s : %s"), *Blueprint->GetPathName(), *OutError);
		return false;
	}

	const FString DiskHash = GetFileHash(JsonPath);
	const FString* SyncedHash = SyncedFileHash.Find(JsonPath);

	// 冲突判定必须在写盘之前：磁盘内容既不是空的、也不等于上次同步记录，说明两边都改过。
	// SyncedHash 为空表示"这个文件我们不知道它当前对应哪份内容"（例如上一次导入失败了），
	// 这种情况同样按冲突处理，否则编辑器保存会把 AI 写的 JSON 整份静默覆盖。
	if (bAllowConflictFile && !DiskHash.IsEmpty() && (!SyncedHash || *SyncedHash != DiskHash))
	{
		const FString ConflictPath = FPaths::ChangeExtension(JsonPath, TEXT("")) + TEXT("_editor_conflict.json");
		if (!FFileHelper::SaveStringToFile(JsonText, *ConflictPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("写入冲突文件失败：%s"), *ConflictPath);
			return false;
		}
		UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: JSON 与编辑器同时被修改，编辑器版本已保存至 %s，请人工合并"), *ConflictPath);
		return true;
	}

	// 目标目录可能还没建（例如在 Content Browser 里新建 /Game/New/UI 下的 WBP）。
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(JsonPath));

	if (!FFileHelper::SaveStringToFile(JsonText, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("写入 JSON 失败：%s"), *JsonPath);
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 写入 JSON 失败 %s"), *JsonPath);
		return false;
	}

	// 用磁盘字节重新算 hash：Tick 侧也是 GetFileHash，两边同处 UTF-8 字节编码域。
	// 不能用 HashAnsiString(JsonText)，它会经 TCHAR_TO_ANSI 把非 ASCII 字符降级成 '?'，
	// 导致中文 JSON 的 hash 永远对不上，进而每次保存都触发一次假回环重导入。
	SyncedFileHash.Add(JsonPath, GetFileHash(JsonPath));
	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 已同步 %s"), *JsonPath);
	return true;
}
