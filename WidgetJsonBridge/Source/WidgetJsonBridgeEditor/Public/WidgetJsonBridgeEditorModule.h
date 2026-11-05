#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

class FWidgetJsonBridgeEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();

	FDelegateHandle MenuStartupHandle;
	void ExportEditedWidgetToJson();

	void ImportAllJsonFiles();

	void OpenSyncFolder();

	void OpenDocsFolder();

	void ExportPropertySchema();

	static void ValidateNames();

	bool OnTick(float DeltaTime);

	void OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext SaveContext);

	FString GetPluginBaseDir() const;

	FTSTicker::FDelegateHandle TickerHandle;

	FDelegateHandle SavedEventHandle;

	struct IConsoleCommand* ExportSchemaCommand = nullptr;

	struct IConsoleCommand* ValidateNamesCommand = nullptr;
};
