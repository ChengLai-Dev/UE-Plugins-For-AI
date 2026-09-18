#include "WidgetJsonSerializer.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/NamedSlot.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/NotNull.h"

#define LOCTEXT_NAMESPACE "WidgetJsonSerializer"

const FString FWidgetJsonSerializer::KeySchemaVersion = TEXT("schemaVersion");
const FString FWidgetJsonSerializer::KeyAssetPath = TEXT("assetPath");
const FString FWidgetJsonSerializer::KeyContentHash = TEXT("contentHash");
const FString FWidgetJsonSerializer::KeyRoot = TEXT("root");
const FString FWidgetJsonSerializer::KeyClass = TEXT("class");
const FString FWidgetJsonSerializer::KeyClassName = TEXT("className");
const FString FWidgetJsonSerializer::KeyName = TEXT("name");
const FString FWidgetJsonSerializer::KeyIsVariable = TEXT("isVariable");
const FString FWidgetJsonSerializer::KeyProperties = TEXT("properties");
const FString FWidgetJsonSerializer::KeySlot = TEXT("slot");
const FString FWidgetJsonSerializer::KeyChildren = TEXT("children");

const int32 FWidgetJsonSerializer::SchemaVersion = 1;

bool FWidgetJsonSerializer::ExportToJson(UWidgetBlueprint* Blueprint, FString& OutJsonText, FString& OutError)
{
	if (!Blueprint || !Blueprint->WidgetTree || !Blueprint->WidgetTree->RootWidget)
	{
		OutError = TEXT("WidgetBlueprint 无有效的 WidgetTree 或根控件");
		return false;
	}

	TArray<UWidget*> AllWidgets;
	Blueprint->WidgetTree->GetAllWidgets(AllWidgets);

	TMap<UWidget*, UWidget*> ParentMap;
	for (UWidget* Widget : AllWidgets)
	{
		UPanelSlot* Slot = GetWidgetSlot(Widget);
		if (Slot && Slot->Parent)
		{
			ParentMap.Add(Widget, Slot->Parent);
		}
	}

	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	RootObj->SetNumberField(KeySchemaVersion, SchemaVersion);
	RootObj->SetStringField(KeyAssetPath, Blueprint->GetOutermost()->GetName());
	RootObj->SetObjectField(KeyRoot, ExportWidgetRecursive(Blueprint->WidgetTree->RootWidget, ParentMap, AllWidgets));
	RootObj->SetStringField(KeyContentHash, ComputeContentHash(RootObj));

	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJsonText);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);
	return true;
}

bool FWidgetJsonSerializer::ImportFromJson(UWidgetBlueprint* Blueprint, const FString& JsonText, FString& OutError)
{
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		OutError = TEXT("WidgetBlueprint 无效");
		return false;
	}

	TSharedPtr<FJsonObject> RootObj;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		OutError = TEXT("JSON 解析失败");
		return false;
	}

	int32 Version = 0;
	RootObj->TryGetNumberField(KeySchemaVersion, Version);
	if (Version != SchemaVersion)
	{
		OutError = FString::Printf(TEXT("不支持的 schemaVersion: %d（期望 %d）"), Version, SchemaVersion);
		return false;
	}

	const TSharedPtr<FJsonObject>* RootWidgetObj = nullptr;
	if (!RootObj->TryGetObjectField(KeyRoot, RootWidgetObj))
	{
		OutError = TEXT("缺少 root 节点");
		return false;
	}

	UWidgetTree* Tree = Blueprint->WidgetTree;

	if (Tree && Tree->RootWidget)
	{
		TArray<UWidget*> OldWidgets;
		Tree->GetAllWidgets(OldWidgets);
		for (UWidget* Widget : OldWidgets)
		{
			Widget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		}
		Tree->RootWidget = nullptr;
	}

	UWidget* NewRoot = nullptr;
	if (!ImportWidgetRecursive(Blueprint, *RootWidgetObj, nullptr, NewRoot, OutError))
	{
		return false;
	}

	Blueprint->Modify();
	return true;
}

FString FWidgetJsonSerializer::ComputeContentHash(const TSharedPtr<FJsonObject>& RootObject)
{
	FString Canonical;
	AppendCanonical(RootObject, Canonical);
	return FMD5::HashAnsiString(*Canonical);
}

TSharedPtr<FJsonObject> FWidgetJsonSerializer::ExportWidgetRecursive(UWidget* Widget, const TMap<UWidget*, UWidget*>& ParentMap, const TArray<UWidget*>& AllWidgets)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	UClass* WidgetClass = Widget->GetClass();
	Obj->SetStringField(KeyClass, FSoftObjectPath(WidgetClass).ToString());
	Obj->SetStringField(KeyClassName, WidgetClass->GetName());
	Obj->SetStringField(KeyName, Widget->GetFName().ToString());
	Obj->SetBoolField(KeyIsVariable, GetWidgetIsVariable(Widget));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<FProperty*> ExportProperties;
	CollectExportableProperties(WidgetClass, ExportProperties);
	for (FProperty* Property : ExportProperties)
	{
		TSharedPtr<FJsonValue> Value = ExportPropertyValue(Property, Widget);
		if (Value.IsValid())
		{
			Properties->SetField(Property->GetName(), Value);
		}
	}
	Obj->SetObjectField(KeyProperties, Properties);

	UPanelSlot* Slot = GetWidgetSlot(Widget);
	if (Slot)
	{
		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetStringField(KeyClass, FSoftObjectPath(Slot->GetClass()).ToString());
		SlotObj->SetStringField(KeyClassName, Slot->GetClass()->GetName());
		TSharedPtr<FJsonObject> SlotProperties = MakeShared<FJsonObject>();
		TArray<FProperty*> SlotProps;
		CollectExportableProperties(Slot->GetClass(), SlotProps);
		for (FProperty* Property : SlotProps)
		{
			const FName PropName = Property->GetFName();
			if (PropName == TEXT("Content") || PropName == TEXT("Parent"))
			{
				continue;
			}
			TSharedPtr<FJsonValue> Value = ExportPropertyValue(Property, Slot);
			if (Value.IsValid())
			{
				SlotProperties->SetField(Property->GetName(), Value);
			}
		}
		SlotObj->SetObjectField(KeyProperties, SlotProperties);
		Obj->SetObjectField(KeySlot, SlotObj);
	}

	TArray<TSharedPtr<FJsonValue>> Children;
	for (UWidget* Candidate : AllWidgets)
	{
		const UWidget* const* ParentPtr = ParentMap.Find(Candidate);
		if (ParentPtr && *ParentPtr == Widget)
		{
			Children.Add(MakeShared<FJsonValueObject>(ExportWidgetRecursive(Candidate, ParentMap, AllWidgets)));
		}
	}
	if (Children.Num() > 0)
	{
		Obj->SetArrayField(KeyChildren, Children);
	}
	return Obj;
}

bool FWidgetJsonSerializer::ImportWidgetRecursive(UWidgetBlueprint* Blueprint, const TSharedPtr<FJsonObject>& JsonObj, UWidget* Parent, UWidget*& OutWidget, FString& OutError)
{
	FString ClassPath;
	JsonObj->TryGetStringField(KeyClass, ClassPath);
	if (ClassPath.IsEmpty())
	{
		OutError = TEXT("控件节点缺少 class 字段");
		return false;
	}

	UClass* WidgetClass = ResolveWidgetClass(ClassPath, OutError);
	if (!WidgetClass)
	{
		return false;
	}
	if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		OutError = FString::Printf(TEXT("类 '%s' 不是 UWidget 子类"), *ClassPath);
		return false;
	}

	UWidgetTree* Tree = Blueprint->WidgetTree;
	FString WidgetName;
	JsonObj->TryGetStringField(KeyName, WidgetName);
	const FName UniqueName = WidgetName.IsEmpty()
		? NAME_None
		: MakeUniqueObjectName(Tree, WidgetClass, *WidgetName);

	UWidget* Widget = NewObject<UWidget>(Tree, WidgetClass, UniqueName, RF_Transactional);
	if (!Widget)
	{
		OutError = FString::Printf(TEXT("创建控件 '%s' 失败"), *ClassPath);
		return false;
	}

	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (JsonObj->TryGetObjectField(KeyProperties, PropertiesObj))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObj)->Values)
		{
			FProperty* Property = WidgetClass->FindPropertyByName(*Pair.Key);
			if (!Property)
			{
				UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 属性 %s 不存在于 %s，已跳过"), *Pair.Key, *WidgetClass->GetName());
				continue;
			}
			FString PropertyError;
			if (!ImportPropertyValue(Property, Widget, Pair.Value, Widget, PropertyError))
			{
				UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 属性 %s.%s 导入失败: %s"), *WidgetClass->GetName(), *Pair.Key, *PropertyError);
			}
		}
	}

	bool bIsVariable = false;
	if (JsonObj->TryGetBoolField(KeyIsVariable, bIsVariable))
	{
		SetWidgetIsVariable(Widget, bIsVariable);
	}

	if (Parent)
	{
		UPanelSlot* CreatedSlot = nullptr;
		if (UPanelWidget* PanelParent = Cast<UPanelWidget>(Parent))
		{
			CreatedSlot = PanelParent->AddChild(Widget);
		}
		else if (UContentWidget* ContentParent = Cast<UContentWidget>(Parent))
		{
			CreatedSlot = ContentParent->SetContent(Widget);
		}
		else
		{
			OutError = FString::Printf(TEXT("控件 %s 不能作为子控件容器"), *Parent->GetClass()->GetName());
			return false;
		}

		const TSharedPtr<FJsonObject>* SlotObj = nullptr;
		if (CreatedSlot && JsonObj->TryGetObjectField(KeySlot, SlotObj))
		{
			const TSharedPtr<FJsonObject>* SlotProps = nullptr;
			if ((*SlotObj)->TryGetObjectField(KeyProperties, SlotProps))
			{
				UClass* SlotClass = CreatedSlot->GetClass();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SlotProps)->Values)
				{
					FProperty* Property = SlotClass->FindPropertyByName(*Pair.Key);
					if (!Property)
					{
						UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 槽位属性 %s 不存在于 %s，已跳过"), *Pair.Key, *SlotClass->GetName());
						continue;
					}
					const FName PropName = Property->GetFName();
					if (PropName == TEXT("Content") || PropName == TEXT("Parent"))
					{
						continue;
					}
					FString SlotError;
					if (!ImportPropertyValue(Property, CreatedSlot, Pair.Value, CreatedSlot, SlotError))
					{
						UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 槽位属性 %s.%s 导入失败: %s"), *SlotClass->GetName(), *Pair.Key, *SlotError);
					}
				}
			}
		}
	}
	else
	{
		Tree->RootWidget = Widget;
	}

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (JsonObj->TryGetArrayField(KeyChildren, Children))
	{
		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			const TSharedPtr<FJsonObject>* ChildObj = nullptr;
			if (!ChildValue->TryGetObject(ChildObj))
			{
				OutError = TEXT("children 中包含非对象节点");
				return false;
			}
			UWidget* ChildWidget = nullptr;
			if (!ImportWidgetRecursive(Blueprint, *ChildObj, Widget, ChildWidget, OutError))
			{
				return false;
			}
		}
	}

	OutWidget = Widget;
	return true;
}

void FWidgetJsonSerializer::CollectExportableProperties(UClass* Class, TArray<FProperty*>& OutProperties)
{
	TSet<FName> SeenNames;
	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FProperty* Property = *It;
		const FName PropName = Property->GetFName();
		if (SeenNames.Contains(PropName))
		{
			continue;
		}
		SeenNames.Add(PropName);

		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_BlueprintReadOnly | CPF_EditConst | CPF_DisableEditOnInstance | CPF_TextExportTransient))
		{
			continue;
		}
		if (Property->IsA<FMulticastDelegateProperty>())
		{
			continue;
		}
		const FString Category = Property->GetMetaData(TEXT("Category"));
		if (Category.Contains(TEXT("Events")) || Category.Contains(TEXT("Bindings")))
		{
			continue;
		}
		if (PropName == TEXT("Slot") || PropName == TEXT("bIsVariable") || PropName == TEXT("CachedGeometry"))
		{
			continue;
		}
		OutProperties.Add(Property);
	}
}

TSharedPtr<FJsonValue> FWidgetJsonSerializer::ExportPropertyValue(FProperty* Property, const void* Container)
{
	if (Property->ArrayDim > 1)
	{
		FString Text;
		Property->ExportTextItem_InContainer(Text, TNotNull<const void*>(Container), nullptr, nullptr, PPF_None, nullptr);
		return MakeShared<FJsonValueString>(Text);
	}
	if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue_InContainer(Container));
	}
	if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProperty->GetIntPropertyEnum())
		{
			const int64 EnumIndex = Enum->GetIndexByValue(ByteProperty->GetPropertyValue_InContainer(Container));
			if (EnumIndex != INDEX_NONE)
			{
				return MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(EnumIndex));
			}
			return MakeShared<FJsonValueString>(FString::FromInt(ByteProperty->GetPropertyValue_InContainer(Container)));
		}
		return MakeShared<FJsonValueNumber>(static_cast<double>(ByteProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
		const int64 EnumValue = Underlying->GetSignedIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Container));
		const int64 EnumIndex = EnumProperty->GetEnum()->GetIndexByValue(EnumValue);
		if (EnumIndex != INDEX_NONE)
		{
			return MakeShared<FJsonValueString>(EnumProperty->GetEnum()->GetNameStringByIndex(EnumIndex));
		}
		return MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), EnumValue));
	}
	if (FInt8Property* IntProperty = CastField<FInt8Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FInt16Property* IntProperty = CastField<FInt16Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FInt64Property* IntProperty = CastField<FInt64Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FUInt16Property* IntProperty = CastField<FUInt16Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FUInt32Property* IntProperty = CastField<FUInt32Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FUInt64Property* IntProperty = CastField<FUInt64Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(FloatProperty->GetPropertyValue_InContainer(Container)));
	}
	if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(DoubleProperty->GetPropertyValue_InContainer(Container));
	}
	if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue_InContainer(Container).ToString());
	}
	if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue_InContainer(Container).ToString());
	}
	if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
	{
		return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue_InContainer(Container));
	}

	FString Text;
	Property->ExportTextItem_InContainer(Text, TNotNull<const void*>(Container), nullptr, nullptr, PPF_None, nullptr);
	return MakeShared<FJsonValueString>(Text);
}

bool FWidgetJsonSerializer::ImportPropertyValue(FProperty* Property, void* Container, const TSharedPtr<FJsonValue>& JsonValue, UObject* OwnerObject, FString& OutError)
{
	if (JsonValue->IsNull())
	{
		return true;
	}

	if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		TextProperty->SetPropertyValue_InContainer(Container, FText::FromString(JsonValue->AsString()));
		return true;
	}
	if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
	{
		StrProperty->SetPropertyValue_InContainer(Container, JsonValue->AsString());
		return true;
	}
	if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		NameProperty->SetPropertyValue_InContainer(Container, FName(*JsonValue->AsString()));
		return true;
	}
	if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (UEnum* Enum = ByteProperty->GetIntPropertyEnum())
		{
			FString ValueText;
			if (JsonValue->Type == EJson::Number)
			{
				ValueText = FString::Printf(TEXT("%d"), static_cast<int32>(JsonValue->AsNumber()));
			}
			else
			{
				ValueText = JsonValue->AsString();
			}
			const int64 EnumIndex = Enum->GetIndexByNameString(ValueText);
			if (EnumIndex == INDEX_NONE)
			{
				if (ValueText.IsNumeric())
				{
					ByteProperty->SetPropertyValue_InContainer(Container, static_cast<uint8>(FCString::Atoi(*ValueText)));
					return true;
				}
				OutError = FString::Printf(TEXT("无效枚举值 '%s'"), *ValueText);
				return false;
			}
			ByteProperty->SetPropertyValue_InContainer(Container, static_cast<uint8>(Enum->GetValueByIndex(EnumIndex)));
			return true;
		}
	}
	if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		FString ValueText;
		if (JsonValue->Type == EJson::Number)
		{
			ValueText = FString::Printf(TEXT("%d"), static_cast<int32>(JsonValue->AsNumber()));
		}
		else
		{
			ValueText = JsonValue->AsString();
		}
		const int64 EnumIndex = EnumProperty->GetEnum()->GetIndexByNameString(ValueText);
		if (EnumIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("无效枚举值 '%s'"), *ValueText);
			return false;
		}
		FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
		Underlying->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Container), EnumProperty->GetEnum()->GetValueByIndex(EnumIndex));
		return true;
	}

	FString ValueText;
	switch (JsonValue->Type)
	{
	case EJson::Boolean:
		ValueText = JsonValue->AsBool() ? TEXT("true") : TEXT("false");
		break;
	case EJson::Number:
		ValueText = FString::SanitizeFloat(JsonValue->AsNumber());
		break;
	case EJson::String:
		ValueText = JsonValue->AsString();
		break;
	default:
		ValueText = TEXT("");
		break;
	}

	const TCHAR* EndPtr = Property->ImportText_InContainer(*ValueText, TNotNull<void*>(Container), OwnerObject, PPF_None, nullptr);
	if (EndPtr == nullptr)
	{
		OutError = FString::Printf(TEXT("属性 '%s' 无法解析值 '%s'"), *Property->GetName(), *ValueText);
		return false;
	}
	return true;
}

UPanelSlot* FWidgetJsonSerializer::GetWidgetSlot(UWidget* Widget)
{
	FProperty* SlotProperty = Widget->GetClass()->FindPropertyByName(TEXT("Slot"));
	if (!SlotProperty)
	{
		return nullptr;
	}
	if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(SlotProperty))
	{
		return Cast<UPanelSlot>(ObjectProperty->GetObjectPropertyValue(ObjectProperty->ContainerPtrToValuePtr<void>(Widget)));
	}
	return nullptr;
}

bool FWidgetJsonSerializer::GetWidgetIsVariable(UWidget* Widget)
{
	FProperty* Property = Widget->GetClass()->FindPropertyByName(TEXT("bIsVariable"));
	if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		return BoolProperty->GetPropertyValue(BoolProperty->ContainerPtrToValuePtr<void>(Widget));
	}
	return false;
}

void FWidgetJsonSerializer::SetWidgetIsVariable(UWidget* Widget, bool bIsVariable)
{
	FProperty* Property = Widget->GetClass()->FindPropertyByName(TEXT("bIsVariable"));
	if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		BoolProperty->SetPropertyValue(BoolProperty->ContainerPtrToValuePtr<void>(Widget), bIsVariable);
	}
}

UClass* FWidgetJsonSerializer::ResolveWidgetClass(const FString& ClassPath, FString& OutError)
{
	UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
	if (!Class)
	{
		const FString ShortPath = FString::Printf(TEXT("/Script/UMG.%s"), *ClassPath);
		Class = LoadObject<UClass>(nullptr, *ShortPath);
	}
	if (!Class)
	{
		OutError = FString::Printf(TEXT("无法加载控件类 '%s'"), *ClassPath);
		return nullptr;
	}
	return Class;
}

void FWidgetJsonSerializer::AppendCanonical(const TSharedPtr<FJsonObject>& Object, FString& Out)
{
	TArray<FJsonObject::FStringType> Keys;
	Object->Values.GetKeys(Keys);
	Keys.Sort();

	Out += TEXT("{");
	bool bFirstWritten = true;
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		const FJsonObject::FStringType& Key = Keys[Index];
		const FString KeyStr(Key.ToView());
		if (KeyStr == KeySchemaVersion || KeyStr == KeyAssetPath || KeyStr == KeyContentHash)
		{
			continue;
		}
		if (!bFirstWritten)
		{
			Out += TEXT(",");
		}
		bFirstWritten = false;
		Out += TEXT("\"");
		Out += EscapeJsonString(KeyStr);
		Out += TEXT("\":");
		AppendCanonicalValue(Object->Values[Key], Out);
	}
	Out += TEXT("}");
}

void FWidgetJsonSerializer::AppendCanonicalValue(const TSharedPtr<FJsonValue>& Value, FString& Out)
{
	switch (Value->Type)
	{
	case EJson::Object:
		AppendCanonical(Value->AsObject(), Out);
		break;
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
		Out += TEXT("[");
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(",");
			}
			AppendCanonicalValue(Array[Index], Out);
		}
		Out += TEXT("]");
		break;
	}
	case EJson::String:
		Out += TEXT("\"");
		Out += EscapeJsonString(Value->AsString());
		Out += TEXT("\"");
		break;
	case EJson::Number:
		Out += FString::Printf(TEXT("%.17g"), Value->AsNumber());
		break;
	case EJson::Boolean:
		Out += Value->AsBool() ? TEXT("true") : TEXT("false");
		break;
	case EJson::Null:
		Out += TEXT("null");
		break;
	default:
		Out += TEXT("null");
		break;
	}
}

FString FWidgetJsonSerializer::EscapeJsonString(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());
	for (TCHAR Char : In)
	{
		switch (Char)
		{
		case TCHAR('\\'):
			Out += TEXT("\\\\");
			break;
		case TCHAR('"'):
			Out += TEXT("\\\"");
			break;
		case TCHAR('\n'):
			Out += TEXT("\\n");
			break;
		case TCHAR('\r'):
			Out += TEXT("\\r");
			break;
		case TCHAR('\t'):
			Out += TEXT("\\t");
			break;
		default:
			Out += Char;
			break;
		}
	}
	return Out;
}

#undef LOCTEXT_NAMESPACE
