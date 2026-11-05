#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UWidget;
class UWidgetBlueprint;
class UWidgetTree;
class UPanelSlot;

class FWidgetJsonSerializer
{
public:
	static const FString KeySchemaVersion;
	static const FString KeyAssetPath;
	static const FString KeyContentHash;
	static const FString KeyRoot;
	static const FString KeyDesignSizeMode;
	static const FString KeyClass;
	static const FString KeyClassName;
	static const FString KeyName;
	static const FString KeyIsVariable;
	static const FString KeyProperties;
	static const FString KeySlot;
	static const FString KeyChildren;
	static const FString KeyEvents;
	static const FString KeyStructMode;

	static const int32 SchemaVersion;

	/**
	 * 导出整棵树。
	 * @param bStructsAsObjects true 时顶层写 "structMode": "object"，非白名单结构体递归导出为 JSON 对象；
	 *        false（默认）保持旧行为，非白名单结构体仍是引擎文本字符串。
	 */
	static bool ExportToJson(UWidgetBlueprint* Blueprint, FString& OutJsonText, FString& OutError, bool bStructsAsObjects = false);

	/**
	 * 导入整棵树。OutDesignSizeMode 非空时回传 JSON 里声明的 designSizeMode，
	 * 由调用方在编译完成后再写到生成类 CDO 上（编译会重建 CDO，这里写了也会丢）。
	 */
	static bool ImportFromJson(UWidgetBlueprint* Blueprint, const FString& JsonText, FString& OutError, FString* OutDesignSizeMode = nullptr);

	/** 校验 designSizeMode 的名称是否合法（FillScreen / Custom / CustomOnScreen / Desired / DesiredOnScreen）。 */
	static bool IsValidDesignSizeModeName(const FString& ModeName);

	/** 把设计器预览尺寸模式写到生成类 CDO 上，必须在 CompileBlueprint 之后调用。 */
	static bool SetDesignSizeMode(UWidgetBlueprint* Blueprint, const FString& ModeName, FString& OutError);

	/** 读回生成类 CDO 上的预览尺寸模式，用于导出。 */
	static FString GetDesignSizeModeName(UWidgetBlueprint* Blueprint);

	static FString ComputeContentHash(const TSharedPtr<FJsonObject>& RootObject);

	/** 导出一个控件/槽类可写的属性清单，供 AI 查表而不是猜测属性名与格式。 */
	static bool ExportClassSchema(UClass* Class, TSharedPtr<FJsonObject>& OutObj);

private:
	static TSharedPtr<FJsonObject> ExportWidgetRecursive(UWidget* Widget, const TMap<UWidget*, UWidget*>& ParentMap, const TArray<UWidget*>& AllWidgets, const TMap<FName, TArray<TSharedPtr<FJsonValue>>>* EventsByWidget, bool bStructsAsObjects);

	static bool ImportWidgetRecursive(UWidgetTree* Tree, const TSharedPtr<FJsonObject>& JsonObj, UWidget* Parent, UWidget*& OutWidget, FString& OutError);

	static void CollectExportableProperties(UClass* Class, TArray<FProperty*>& OutProperties);

	static TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* Container, bool bStructsAsObjects, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth);

	static bool ImportPropertyValue(FProperty* Property, void* Container, const TSharedPtr<FJsonValue>& JsonValue, UObject* OwnerObject, FString& OutError, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth);

	/** 把一个结构体递归导出为 JSON 对象；VisitingStructs/Depth 用于检测循环引用与限制深度。 */
	static bool ExportStructObject(const UScriptStruct* Struct, const void* ValuePtr, TSharedPtr<FJsonValue>& OutValue, FString& OutError, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth);

	/** 把 JSON 对象递归写回结构体；字段名用 C++ 属性名，未知字段按错误返回。 */
	static bool ImportStructObject(const UScriptStruct* Struct, void* ValuePtr, const TSharedPtr<FJsonObject>& Obj, UObject* OwnerObject, FString& OutError, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth);

	static UPanelSlot* GetWidgetSlot(UWidget* Widget);

	static bool GetWidgetIsVariable(UWidget* Widget);

	static void SetWidgetIsVariable(UWidget* Widget, bool bIsVariable);

	static UClass* ResolveWidgetClass(const FString& ClassPath, FString& OutError);

	static void AppendCanonical(const TSharedPtr<FJsonObject>& Object, FString& Out);

	static void AppendCanonicalValue(const TSharedPtr<FJsonValue>& Value, FString& Out);

	static FString EscapeJsonString(const FString& In);
};
