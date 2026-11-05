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

	/**
	 * 按目标 JSON 文件顶层的 structMode 导出为文本，不写盘。
	 * ExportAssetInternal 与测试共用，保证「读模式」只有一份实现。
	 */
	static bool ExportToJsonForPath(UWidgetBlueprint* Blueprint, const FString& JsonPath, FString& OutJsonText, FString& OutError);

	static FString GetSyncRootDir();

	static bool AssetPathToJsonPath(const FString& AssetPath, FString& OutJsonPath);

	static bool JsonPathToAssetPath(const FString& JsonPath, FString& OutAssetPath);

	/** 把每个 UWidget/UPanelSlot 子类的可写属性清单导出成 JSON 文件，供 AI 查表。 */
	static bool ExportPropertySchemaToFile(FString& OutFilePath, FString& OutError);

	/**
	 * 检查 WidgetTree 里每个控件的「Hierarchy 显示名」是否等于对象名。
	 * 显示名由 GetLabelText() 决定（DisplayLabel / bIsVariable / 名字是否以「类名_」开头），
	 * 与代码用的 GetName() 分叉时，照着面板写字符串路径就会找不到控件。
	 * 只做诊断（导入时会统一 SetDisplayLabel(对象名)，正常应为 0 命中），不阻断导入。
	 * @return Error 数量（变量控件的不一致；WidgetJsonBridge.StrictNameCheck 1 时全部算 Error）
	 */
	static int32 ValidateDisplayNames(UWidgetBlueprint* Blueprint, const TCHAR* Context);

	/** 对 /Game 下所有 Widget Blueprint 跑一遍命名检查，返回 Error 数量。 */
	static int32 ValidateAllWidgetBlueprints();

	/** 诊断用：本进程内成功导入的次数。用于验证「导出后不会被误判为文件改动而重复导入」。 */
	int32 GetImportCount() const { return ImportCount; }

private:
	FWidgetJsonSync() = default;

	bool ImportJsonFile(const FString& JsonFilePath);

	bool ExportAssetInternal(UWidgetBlueprint* Blueprint, const FString& JsonPath, bool bAllowConflictFile, FString& OutError);

	FString GetFileHash(const FString& FilePath) const;

	static FString GetImportErrorReportPath(const FString& JsonFilePath);

	void ReportImportFailure(const FString& JsonFilePath, const FString& AssetPath, const FString& Error);

	void ClearImportFailure(const FString& JsonFilePath);

	void BindDeclaredEvents(class UWidgetBlueprint* Blueprint, const FString& JsonText);

	TMap<FString, FString> SyncedFileHash;

	/** 记录"这份内容已经导入失败过"，避免坏 JSON 每秒刷日志；文件一变（hash 变）自动重试。 */
	TMap<FString, FString> FailedFileHash;

	TSet<FString> ImportingPackageNames;

	int32 ImportCount = 0;
};
