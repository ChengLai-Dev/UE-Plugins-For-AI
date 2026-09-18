#include "WidgetJsonSync.h"

#include "WidgetJsonSerializer.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "FileHelpers.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Animation/WidgetAnimation.h"
#include "Components/Widget.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"

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
	if (!AssetPath.StartsWith(TEXT("/Game/")))
	{
		return false;
	}
	const FString Relative = AssetPath.RightChop(6);
	OutJsonPath = FPaths::Combine(GetSyncRootDir(), Relative) + TEXT(".json");
	return true;
}

bool FWidgetJsonSync::JsonPathToAssetPath(const FString& JsonPath, FString& OutAssetPath)
{
	const FString FullRoot = FPaths::ConvertRelativePathToFull(GetSyncRootDir());
	const FString FullJson = FPaths::ConvertRelativePathToFull(JsonPath);
	if (!FullJson.StartsWith(FullRoot))
	{
		return false;
	}
	FString Relative = FullJson.RightChop(FullRoot.Len() + 1);
	Relative = FPaths::ChangeExtension(Relative, TEXT(""));
	Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
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
		ImportJsonFile(JsonFile);
		SyncedFileHash.Add(JsonFile, GetFileHash(JsonFile));
	}
}

void FWidgetJsonSync::ImportAll()
{
	SyncedFileHash.Reset();
	Tick();
}

void FWidgetJsonSync::ImportJsonFile(const FString& JsonFilePath)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *JsonFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 读取 JSON 失败 %s"), *JsonFilePath);
		return;
	}

	FString AssetPath;
	if (!JsonPathToAssetPath(JsonFilePath, AssetPath))
	{
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: JSON 路径无法映射到资产路径 %s"), *JsonFilePath);
		return;
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
			UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 创建资产失败 %s"), *AssetPath);
			return;
		}
	}

	FString Error;
	if (!FWidgetJsonSerializer::ImportFromJson(Blueprint, JsonText, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("WidgetJsonBridge: 导入失败 %s : %s"), *JsonFilePath, *Error);
		return;
	}

	SyncVariableGuids(Blueprint);

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	const FString PackageName = Blueprint->GetOutermost()->GetName();
	ImportingPackageNames.Add(PackageName);
	{
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Blueprint->GetOutermost());
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	}
	ImportingPackageNames.Remove(PackageName);

	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 已导入 %s -> %s"), *JsonFilePath, *AssetPath);
}

void FWidgetJsonSync::SyncVariableGuids(UWidgetBlueprint* Blueprint)
{
	TMap<FName, FGuid> AnimationGuids;
	for (UWidgetAnimation* Animation : Blueprint->Animations)
	{
		if (Animation)
		{
			if (const FGuid* Guid = Blueprint->WidgetVariableNameToGuidMap.Find(Animation->GetFName()))
			{
				AnimationGuids.Add(Animation->GetFName(), *Guid);
			}
		}
	}

	Blueprint->WidgetVariableNameToGuidMap.Reset();

	TArray<UWidget*> AllWidgets;
	Blueprint->WidgetTree->GetAllWidgets(AllWidgets);
	for (UWidget* Widget : AllWidgets)
	{
		Blueprint->OnVariableAdded(Widget->GetFName());
	}

	for (const TPair<FName, FGuid>& Pair : AnimationGuids)
	{
		Blueprint->WidgetVariableNameToGuidMap.Add(Pair.Key, Pair.Value);
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
	ExportAssetInternal(Blueprint, JsonPath, true, Error);
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

bool FWidgetJsonSync::ExportAssetInternal(UWidgetBlueprint* Blueprint, const FString& JsonPath, bool bAllowConflictFile, FString& OutError)
{
	FString JsonText;
	if (!FWidgetJsonSerializer::ExportToJson(Blueprint, JsonText, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 导出失败 %s : %s"), *Blueprint->GetPathName(), *OutError);
		return false;
	}

	const FString NewHash = FMD5::HashAnsiString(*JsonText);
	const FString DiskHash = GetFileHash(JsonPath);

	if (DiskHash.IsEmpty())
	{
		FFileHelper::SaveStringToFile(JsonText, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		SyncedFileHash.Add(JsonPath, NewHash);
		UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 已导出 %s"), *JsonPath);
		return true;
	}

	const FString* SyncedHash = SyncedFileHash.Find(JsonPath);
	if (bAllowConflictFile && SyncedHash && *SyncedHash != DiskHash)
	{
		const FString ConflictPath = FPaths::ChangeExtension(JsonPath, TEXT("")) + TEXT("_editor_conflict.json");
		FFileHelper::SaveStringToFile(JsonText, *ConflictPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: JSON 与编辑器同时被修改，编辑器版本已保存至 %s，请人工合并"), *ConflictPath);
		return true;
	}

	FFileHelper::SaveStringToFile(JsonText, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	SyncedFileHash.Add(JsonPath, NewHash);
	UE_LOG(LogTemp, Log, TEXT("WidgetJsonBridge: 已同步 %s"), *JsonPath);
	return true;
}
