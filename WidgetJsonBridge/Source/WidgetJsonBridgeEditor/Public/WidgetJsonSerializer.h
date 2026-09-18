#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UWidget;
class UWidgetBlueprint;
class UPanelSlot;

class FWidgetJsonSerializer
{
public:
	static const FString KeySchemaVersion;
	static const FString KeyAssetPath;
	static const FString KeyContentHash;
	static const FString KeyRoot;
	static const FString KeyClass;
	static const FString KeyClassName;
	static const FString KeyName;
	static const FString KeyIsVariable;
	static const FString KeyProperties;
	static const FString KeySlot;
	static const FString KeyChildren;

	static const int32 SchemaVersion;

	static bool ExportToJson(UWidgetBlueprint* Blueprint, FString& OutJsonText, FString& OutError);

	static bool ImportFromJson(UWidgetBlueprint* Blueprint, const FString& JsonText, FString& OutError);

	static FString ComputeContentHash(const TSharedPtr<FJsonObject>& RootObject);

private:
	static TSharedPtr<FJsonObject> ExportWidgetRecursive(UWidget* Widget, const TMap<UWidget*, UWidget*>& ParentMap, const TArray<UWidget*>& AllWidgets);

	static bool ImportWidgetRecursive(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& JsonObj, UWidget* Parent, UWidget*& OutWidget, FString& OutError);

	static void CollectExportableProperties(UClass* Class, TArray<FProperty*>& OutProperties);

	static TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* Container);

	static bool ImportPropertyValue(FProperty* Property, void* Container, const TSharedPtr<FJsonValue>& JsonValue, UObject* OwnerObject, FString& OutError);

	static UPanelSlot* GetWidgetSlot(UWidget* Widget);

	static bool GetWidgetIsVariable(UWidget* Widget);

	static void SetWidgetIsVariable(UWidget* Widget, bool bIsVariable);

	static UClass* ResolveWidgetClass(const FString& ClassPath, FString& OutError);

	static void AppendCanonical(const TSharedPtr<FJsonObject>& Object, FString& Out);

	static void AppendCanonicalValue(const TSharedPtr<FJsonValue>& Value, FString& Out);

	static FString EscapeJsonString(const FString& In);
};
