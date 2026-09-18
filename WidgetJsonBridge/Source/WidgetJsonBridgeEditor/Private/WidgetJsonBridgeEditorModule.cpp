#include "WidgetJsonBridgeEditorModule.h"

#include "WidgetJsonSync.h"
#include "WidgetBlueprint.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/ObjectSaveContext.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FWidgetJsonBridgeEditorModule"

void FWidgetJsonBridgeEditorModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FWidgetJsonBridgeEditorModule::RegisterMenus));

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWidgetJsonBridgeEditorModule::OnTick), 1.0f);

	SavedEventHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FWidgetJsonBridgeEditorModule::OnPackageSaved);
}

void FWidgetJsonBridgeEditorModule::ShutdownModule()
{
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

bool FWidgetJsonBridgeEditorModule::OnTick(float DeltaTime)
{
	FWidgetJsonSync::Get().Tick();
	return true;
}

void FWidgetJsonBridgeEditorModule::OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext SaveContext)
{
	if (!Package || !GEditor)
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
