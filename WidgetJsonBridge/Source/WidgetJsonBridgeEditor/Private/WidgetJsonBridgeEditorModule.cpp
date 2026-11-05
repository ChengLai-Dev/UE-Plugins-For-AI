#include "WidgetJsonBridgeEditorModule.h"

#include "WidgetJsonSync.h"
#include "WidgetJsonSerializer.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Components/PanelSlot.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/ObjectSaveContext.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "FWidgetJsonBridgeEditorModule"

void FWidgetJsonBridgeEditorModule::StartupModule()
{
	MenuStartupHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FWidgetJsonBridgeEditorModule::RegisterMenus));

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWidgetJsonBridgeEditorModule::OnTick), 1.0f);

	SavedEventHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FWidgetJsonBridgeEditorModule::OnPackageSaved);

	ExportSchemaCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("WidgetJsonBridge.ExportSchema"),
		TEXT("导出所有 UMG 控件类与槽类的可写属性 Schema 到 Saved/WidgetJsonBridge/PropertySchema/"),
		FConsoleCommandDelegate::CreateRaw(this, &FWidgetJsonBridgeEditorModule::ExportPropertySchema));

	ValidateNamesCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("WidgetJsonBridge.ValidateNames"),
		TEXT("检查所有 Widget Blueprint 的 Hierarchy 显示名是否与对象名一致"),
		FConsoleCommandDelegate::CreateStatic(&FWidgetJsonBridgeEditorModule::ValidateNames));
}

void FWidgetJsonBridgeEditorModule::ShutdownModule()
{
	if (MenuStartupHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(MenuStartupHandle);
		MenuStartupHandle.Reset();
	}
	// 菜单条目持有 this，热重载/禁用插件后不清掉就是悬垂指针。
	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		ToolMenus->RemoveSection("LevelEditor.MainMenu.Tools", "WidgetJsonBridge");
	}
	if (ExportSchemaCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ExportSchemaCommand);
		ExportSchemaCommand = nullptr;
	}
	if (ValidateNamesCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ValidateNamesCommand);
		ValidateNamesCommand = nullptr;
	}
	if (SavedEventHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(SavedEventHandle);
		SavedEventHandle.Reset();
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
}

void FWidgetJsonBridgeEditorModule::RegisterMenus()
{
	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (!ToolsMenu)
	{
		return;
	}
	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("WidgetJsonBridge", LOCTEXT("WidgetJsonBridge", "Widget Json Bridge"));
	Section.AddMenuEntry(
		"ExportEditedWidgetToJson",
		LOCTEXT("ExportEditedWidgetToJson", "Export Edited Widget to JSON"),
		LOCTEXT("ExportEditedWidgetToJsonTooltip", "将当前打开的 Widget Blueprint 导出为 JSON 文件"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FWidgetJsonBridgeEditorModule::ExportEditedWidgetToJson)));
	Section.AddMenuEntry(
		"ImportAllJsonFiles",
		LOCTEXT("ImportAllJsonFiles", "Import All JSON Files"),
		LOCTEXT("ImportAllJsonFilesTooltip", "重新导入 UIWidgets 目录下所有 JSON 文件"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FWidgetJsonBridgeEditorModule::ImportAllJsonFiles)));
	Section.AddMenuEntry(
		"OpenSyncFolder",
		LOCTEXT("OpenSyncFolder", "Open Sync Folder"),
		LOCTEXT("OpenSyncFolderTooltip", "在资源管理器中打开 UIWidgets 同步目录"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FWidgetJsonBridgeEditorModule::OpenSyncFolder)));
	Section.AddMenuEntry(
		"OpenDocsFolder",
		LOCTEXT("OpenDocsFolder", "Open Docs Folder"),
		LOCTEXT("OpenDocsFolderTooltip", "打开插件文档目录（Schema 参考与使用手册）"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FWidgetJsonBridgeEditorModule::OpenDocsFolder)));
	Section.AddMenuEntry(
		"ExportPropertySchema",
		LOCTEXT("ExportPropertySchema", "Export Property Schema"),
		LOCTEXT("ExportPropertySchemaTooltip", "把每个控件类/槽类的可写属性名、类型与值格式导出成 JSON，供 AI 查表"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FWidgetJsonBridgeEditorModule::ExportPropertySchema)));
}

void FWidgetJsonBridgeEditorModule::ExportEditedWidgetToJson()
{
	if (!GEditor)
	{
		return;
	}
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return;
	}
	TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
	int32 ExportedCount = 0;
	for (UObject* Asset : EditedAssets)
	{
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Asset);
		if (!Blueprint)
		{
			continue;
		}
		FString Error;
		if (FWidgetJsonSync::Get().ExportAsset(Blueprint, Error))
		{
			++ExportedCount;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 导出失败 %s : %s"), *Blueprint->GetPathName(), *Error);
		}
	}
	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 已导出 %d 个 Widget Blueprint"), ExportedCount);
}

void FWidgetJsonBridgeEditorModule::ImportAllJsonFiles()
{
	FWidgetJsonSync::Get().ImportAll();
}

void FWidgetJsonBridgeEditorModule::OpenSyncFolder()
{
	FPlatformProcess::ExploreFolder(*FWidgetJsonSync::GetSyncRootDir());
}

void FWidgetJsonBridgeEditorModule::OpenDocsFolder()
{
	const FString DocsDir = FPaths::Combine(GetPluginBaseDir(), TEXT("Docs"));
	FPlatformProcess::ExploreFolder(*DocsDir);
}

void FWidgetJsonBridgeEditorModule::ValidateNames()
{
	FWidgetJsonSync::ValidateAllWidgetBlueprints();
}

void FWidgetJsonBridgeEditorModule::ExportPropertySchema()
{
	FString OutPath;
	FString Error;
	if (!FWidgetJsonSync::ExportPropertySchemaToFile(OutPath, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 属性 Schema 导出失败：%s"), *Error);
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 属性 Schema 已导出到 %s"), *OutPath);
	FPlatformProcess::ExploreFolder(*FPaths::GetPath(OutPath));
}

bool FWidgetJsonBridgeEditorModule::OnTick(float DeltaTime)
{
	// PIE 期间重建资产会让运行中的 widget 实例引用失效，跳过这一秒。
	if (GEditor && GEditor->PlayWorld)
	{
		return true;
	}
	FWidgetJsonSync::Get().Tick();
	return true;
}

void FWidgetJsonBridgeEditorModule::OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext SaveContext)
{
	if (!Package || !GEditor)
	{
		return;
	}
	// cook 期间也会触发保存事件，不能把被 cook 的 WBP 反写成项目里的 JSON。
	if (SaveContext.IsCooking())
	{
		return;
	}
	TArray<UWidgetBlueprint*> Blueprints;
	ForEachObjectWithOuter(Package, [&Blueprints](UObject* Object)
	{
		if (UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Object))
		{
			Blueprints.Add(Blueprint);
		}
	});
	for (UWidgetBlueprint* Blueprint : Blueprints)
	{
		FWidgetJsonSync::Get().OnAssetSaved(Blueprint);
	}
}

FString FWidgetJsonBridgeEditorModule::GetPluginBaseDir() const
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("WidgetJsonBridge"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir();
	}
	return FString();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWidgetJsonBridgeEditorModule, WidgetJsonBridgeEditor)
