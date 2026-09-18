#pragma once

#include "CoreMinimal.h"

class UWidgetBlueprint;

class FWidgetJsonSync
{
public:
	static FWidgetJsonSync& Get();

	void Tick();

	void OnAssetSaved(UWidgetBlueprint* Blueprint);

	void ImportAll();

	bool ExportAsset(UWidgetBlueprint* Blueprint, FString& OutError);

	static FString GetSyncRootDir();

	static bool AssetPathToJsonPath(const FString& AssetPath, FString& OutJsonPath);

	static bool JsonPathToAssetPath(const FString& JsonPath, FString& OutAssetPath);

private:
	FWidgetJsonSync() = default;

	void ImportJsonFile(const FString& JsonFilePath);

	void SyncVariableGuids(class UWidgetBlueprint* Blueprint);

	bool ExportAssetInternal(UWidgetBlueprint* Blueprint, const FString& JsonPath, bool bAllowConflictFile, FString& OutError);

	FString GetFileHash(const FString& FilePath) const;

	TMap<FString, FString> SyncedFileHash;

	TSet<FString> ImportingPackageNames;
};
