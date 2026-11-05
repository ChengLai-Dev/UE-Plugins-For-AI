#include "WidgetJsonSerializer.h"

#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
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
#include "Math/Color.h"
#include "Layout/Margin.h"
#include "Widgets/Layout/Anchors.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SlateWrapperTypes.h"

namespace
{
	TSharedPtr<FJsonValue> MakeVector2DJson(const FVector2D& In)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), In.X);
		Obj->SetNumberField(TEXT("y"), In.Y);
		return MakeShared<FJsonValueObject>(Obj);
	}

	FVector2D ReadVector2D(const TSharedPtr<FJsonObject>& Obj)
	{
		double X = 0.0;
		double Y = 0.0;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("y"), Y);
		}
		return FVector2D(static_cast<float>(X), static_cast<float>(Y));
	}

	TSharedPtr<FJsonValue> MakeMarginJson(const FMargin& In)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("left"), In.Left);
		Obj->SetNumberField(TEXT("top"), In.Top);
		Obj->SetNumberField(TEXT("right"), In.Right);
		Obj->SetNumberField(TEXT("bottom"), In.Bottom);
		return MakeShared<FJsonValueObject>(Obj);
	}

	FMargin ReadMargin(const TSharedPtr<FJsonObject>& Obj)
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("left"), Left);
			Obj->TryGetNumberField(TEXT("top"), Top);
			Obj->TryGetNumberField(TEXT("right"), Right);
			Obj->TryGetNumberField(TEXT("bottom"), Bottom);
		}
		return FMargin(
			static_cast<float>(Left),
			static_cast<float>(Top),
			static_cast<float>(Right),
			static_cast<float>(Bottom));
	}

	TSharedPtr<FJsonValue> MakeAnchorsJson(const FAnchors& In)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetField(TEXT("min"), MakeVector2DJson(In.Minimum));
		Obj->SetField(TEXT("max"), MakeVector2DJson(In.Maximum));
		return MakeShared<FJsonValueObject>(Obj);
	}

	FAnchors ReadAnchors(const TSharedPtr<FJsonObject>& Obj)
	{
		FAnchors Anchors;
		if (!Obj.IsValid())
		{
			return Anchors;
		}
		const TSharedPtr<FJsonObject>* MinObj = nullptr;
		const TSharedPtr<FJsonObject>* MaxObj = nullptr;
		if (Obj->TryGetObjectField(TEXT("min"), MinObj) && MinObj)
		{
			Anchors.Minimum = ReadVector2D(*MinObj);
		}
		if (Obj->TryGetObjectField(TEXT("max"), MaxObj) && MaxObj)
		{
			Anchors.Maximum = ReadVector2D(*MaxObj);
		}
		return Anchors;
	}

	TSharedPtr<FJsonValue> MakeLinearColorJson(const FLinearColor& In)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("r"), In.R);
		Obj->SetNumberField(TEXT("g"), In.G);
		Obj->SetNumberField(TEXT("b"), In.B);
		Obj->SetNumberField(TEXT("a"), In.A);
		return MakeShared<FJsonValueObject>(Obj);
	}

	FLinearColor ReadLinearColor(const TSharedPtr<FJsonObject>& Obj)
	{
		double R = 1.0;
		double G = 1.0;
		double B = 1.0;
		double A = 1.0;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("r"), R);
			Obj->TryGetNumberField(TEXT("g"), G);
			Obj->TryGetNumberField(TEXT("b"), B);
			Obj->TryGetNumberField(TEXT("a"), A);
		}
		return FLinearColor(
			static_cast<float>(R),
			static_cast<float>(G),
			static_cast<float>(B),
			static_cast<float>(A));
	}

	/** 把 Class'/Game/.../Asset.Asset' 或 "None" 规范成可交给 FSoftObjectPath 的路径。 */
	FString NormalizeObjectPathText(const FString& InText)
	{
		FString Text = InText;
		Text.TrimStartAndEndInline();

		int32 QuoteIndex = INDEX_NONE;
		if (Text.FindChar(TEXT('\''), QuoteIndex) && Text.EndsWith(TEXT("'")))
		{
			const int32 InnerLength = Text.Len() - QuoteIndex - 2;
			if (InnerLength > 0)
			{
				return Text.Mid(QuoteIndex + 1, InnerLength);
			}
		}
		return Text;
	}

	/** 对象路径必须能被导入端重新加载；瞬态或未落盘对象写 None，避免导出自己都读不回来的路径。 */
	bool IsResolvableObjectPath(const FSoftObjectPath& Path)
	{
		return Path.IsValid() && Path.TryLoad() != nullptr;
	}

	FString SizeRuleToString(ESlateSizeRule::Type Rule)
	{
		return Rule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Automatic");
	}

	ESlateSizeRule::Type StringToSizeRule(const FString& Text)
	{
		return Text.Equals(TEXT("Fill"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic;
	}

	/** 该结构体是否有原生 JSON 写法（对应 TryExportKnownStruct / TryImportKnownStruct 的白名单）。 */
	bool IsKnownStruct(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return false;
		}
		return Struct == TBaseStructure<FVector2D>::Get()
			|| Struct == TBaseStructure<FLinearColor>::Get()
			|| Struct == TBaseStructure<FMargin>::Get()
			|| Struct == FAnchors::StaticStruct()
			|| Struct == FAnchorData::StaticStruct()
			|| Struct == FSlateChildSize::StaticStruct();
	}

	/** 把错误追加到 OutError（多行累积），不覆盖已有内容。 */
	void AppendError(FString& OutError, const FString& Message)
	{
		if (!OutError.IsEmpty())
		{
			OutError.Append(TEXT("\n"));
		}
		OutError.Append(Message);
	}

	/** UE5 的枚举名带命名空间（ETextJustify::Left），JSON 里用短名更好写也更好读。 */
	FString ShortEnumName(UEnum* Enum, int32 Index)
	{
		if (!Enum)
		{
			return FString();
		}
		const FString FullName = Enum->GetNameStringByIndex(Index);
		int32 ColonIndex = INDEX_NONE;
		if (FullName.FindLastChar(TEXT(':'), ColonIndex))
		{
			return FullName.RightChop(ColonIndex + 1);
		}
		return FullName;
	}

	/** 高频结构体的原生 JSON 表达。返回 false 表示该结构体不在白名单，调用方回退到 ExportTextItem。 */
	bool TryExportKnownStruct(const UScriptStruct* Struct, const void* ValuePtr, TSharedPtr<FJsonValue>& OutValue)
	{
		if (!Struct || !ValuePtr)
		{
			return false;
		}
		if (Struct == TBaseStructure<FVector2D>::Get())
		{
			OutValue = MakeVector2DJson(*static_cast<const FVector2D*>(ValuePtr));
			return true;
		}
		if (Struct == TBaseStructure<FLinearColor>::Get())
		{
			OutValue = MakeLinearColorJson(*static_cast<const FLinearColor*>(ValuePtr));
			return true;
		}
		if (Struct == TBaseStructure<FMargin>::Get())
		{
			OutValue = MakeMarginJson(*static_cast<const FMargin*>(ValuePtr));
			return true;
		}
		if (Struct == FAnchors::StaticStruct())
		{
			OutValue = MakeAnchorsJson(*static_cast<const FAnchors*>(ValuePtr));
			return true;
		}
		if (Struct == FAnchorData::StaticStruct())
		{
			const FAnchorData& Data = *static_cast<const FAnchorData*>(ValuePtr);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetField(TEXT("offsets"), MakeMarginJson(Data.Offsets));
			Obj->SetField(TEXT("anchors"), MakeAnchorsJson(Data.Anchors));
			Obj->SetField(TEXT("alignment"), MakeVector2DJson(Data.Alignment));
			OutValue = MakeShared<FJsonValueObject>(Obj);
			return true;
		}
		if (Struct == FSlateChildSize::StaticStruct())
		{
			const FSlateChildSize& Size = *static_cast<const FSlateChildSize*>(ValuePtr);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("value"), Size.Value);
			Obj->SetStringField(TEXT("rule"), SizeRuleToString(Size.SizeRule));
			OutValue = MakeShared<FJsonValueObject>(Obj);
			return true;
		}
		return false;
	}

	/** 把一个 JSON 对象写回白名单结构体。未知结构体返回 false 并给出可读错误。 */
	bool TryImportKnownStruct(const UScriptStruct* Struct, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!JsonValue->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			OutError = TEXT("该结构体属性必须写成 JSON 对象");
			return false;
		}
		const TSharedPtr<FJsonObject>& Obj = *ObjPtr;

		if (Struct == TBaseStructure<FVector2D>::Get())
		{
			*static_cast<FVector2D*>(ValuePtr) = ReadVector2D(Obj);
			return true;
		}
		if (Struct == TBaseStructure<FLinearColor>::Get())
		{
			*static_cast<FLinearColor*>(ValuePtr) = ReadLinearColor(Obj);
			return true;
		}
		if (Struct == TBaseStructure<FMargin>::Get())
		{
			*static_cast<FMargin*>(ValuePtr) = ReadMargin(Obj);
			return true;
		}
		if (Struct == FAnchors::StaticStruct())
		{
			*static_cast<FAnchors*>(ValuePtr) = ReadAnchors(Obj);
			return true;
		}
		if (Struct == FAnchorData::StaticStruct())
		{
			FAnchorData& Data = *static_cast<FAnchorData*>(ValuePtr);
			const TSharedPtr<FJsonObject>* OffsetsObj = nullptr;
			const TSharedPtr<FJsonObject>* AnchorsObj = nullptr;
			const TSharedPtr<FJsonObject>* AlignmentObj = nullptr;
			if (Obj->TryGetObjectField(TEXT("offsets"), OffsetsObj) && OffsetsObj)
			{
				Data.Offsets = ReadMargin(*OffsetsObj);
			}
			if (Obj->TryGetObjectField(TEXT("anchors"), AnchorsObj) && AnchorsObj)
			{
				Data.Anchors = ReadAnchors(*AnchorsObj);
			}
			if (Obj->TryGetObjectField(TEXT("alignment"), AlignmentObj) && AlignmentObj)
			{
				Data.Alignment = ReadVector2D(*AlignmentObj);
			}
			return true;
		}
		if (Struct == FSlateChildSize::StaticStruct())
		{
			FSlateChildSize& Size = *static_cast<FSlateChildSize*>(ValuePtr);
			double Value = 1.0;
			FString RuleText;
			if (Obj->TryGetNumberField(TEXT("value"), Value))
			{
				Size.Value = static_cast<float>(Value);
			}
			if (Obj->TryGetStringField(TEXT("rule"), RuleText))
			{
				Size.SizeRule = StringToSizeRule(RuleText);
			}
			return true;
		}

		OutError = FString::Printf(
			TEXT("结构体 %s 不支持对象写法，请改用引擎文本格式字符串"),
			Struct ? *Struct->GetName() : TEXT("<null>"));
		return false;
	}

	/**
	 * FAnchorData 的语义校验。
	 *
	 * 引擎里 Offsets 的两组数字含义随锚点变化（SConstraintCanvas::ArrangeLayeredChildren）：
	 *   锚点重合（点锚点）时 Right/Bottom 是宽高，Slate 直接用 Size = (Right, Bottom)；
	 *   锚点拉伸时 Right/Bottom 是相对右/下边的边距。
	 * 所以点锚点下出现负的 Right/Bottom 必然是写错了，这种槽位在 Slate 里会退化成负尺寸矩形
	 * （画不出来、也点不到），最常见的原因是把它当成了"右下边界坐标"。
	 * 拉伸锚点下的负边距是合法的（控件故意超出父容器），不检查。
	 */
	bool ValidateAnchorData(const FAnchorData& Data, FString& OutError)
	{
		const bool bPointAnchoredX = Data.Anchors.Minimum.X == Data.Anchors.Maximum.X;
		const bool bPointAnchoredY = Data.Anchors.Minimum.Y == Data.Anchors.Maximum.Y;

		if (bPointAnchoredX && Data.Offsets.Right < 0.0f)
		{
			OutError = FString::Printf(
				TEXT("LayoutData.offsets.right = %.1f 是负数。锚点重合时 right 表示宽度（Designer 里的 Size X），不是右下边界 X 坐标；")
				TEXT("若这里原本写的是右下边界 %.1f（left = %.1f），应改成 right = %.1f"),
				Data.Offsets.Right,
				Data.Offsets.Right,
				Data.Offsets.Left,
				Data.Offsets.Right - Data.Offsets.Left);
			return false;
		}
		if (bPointAnchoredY && Data.Offsets.Bottom < 0.0f)
		{
			OutError = FString::Printf(
				TEXT("LayoutData.offsets.bottom = %.1f 是负数。锚点重合时 bottom 表示高度（Designer 里的 Size Y），不是右下边界 Y 坐标；")
				TEXT("若这里原本写的是右下边界 %.1f（top = %.1f），应改成 bottom = %.1f"),
				Data.Offsets.Bottom,
				Data.Offsets.Bottom,
				Data.Offsets.Top,
				Data.Offsets.Bottom - Data.Offsets.Top);
			return false;
		}
		return true;
	}

	/** 结构体对象模式的递归深度上限，同时用来兜底自引用结构。 */
	constexpr int32 MaxStructObjectDepth = 16;

	/** 结构体对象模式下允许导出的字段。CPF_EditConst 不排除（FSlateBrush 有这类字段）。 */
	bool IsStructObjectField(const FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			return false;
		}
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_TextExportTransient))
		{
			return false;
		}
		if (Property->IsA<FMulticastDelegateProperty>() || Property->IsA<FDelegateProperty>())
		{
			return false;
		}
		return true;
	}

	/** 给 Schema 递归列出结构体字段，供 AI 查表。VisitingStructs 防自引用。 */
	void AppendStructFieldSchema(const UScriptStruct* Struct, TArray<TSharedPtr<FJsonValue>>& OutFields, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth)
	{
		if (!Struct || VisitingStructs.Contains(Struct) || Depth > MaxStructObjectDepth)
		{
			return;
		}
		VisitingStructs.Add(Struct);

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Field = *It;
			if (!IsStructObjectField(Field))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Field->GetName());

			UEnum* Enum = nullptr;
			if (FByteProperty* ByteProperty = CastField<FByteProperty>(Field))
			{
				Enum = ByteProperty->GetIntPropertyEnum();
			}
			else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Field))
			{
				Enum = EnumProperty->GetEnum();
			}

			FStructProperty* StructField = CastField<FStructProperty>(Field);
			if (Enum)
			{
				Entry->SetStringField(TEXT("type"), Enum->GetName());
				Entry->SetStringField(TEXT("format"), TEXT("枚举名字符串"));
				TArray<TSharedPtr<FJsonValue>> Names;
				const int32 EnumCount = Enum->NumEnums();
				for (int32 Index = 0; Index < EnumCount; ++Index)
				{
					const FString EnumName = ShortEnumName(Enum, Index);
					if (EnumName.EndsWith(TEXT("_MAX")) || EnumName.EndsWith(TEXT("_MASK")))
					{
						continue;
					}
					Names.Add(MakeShared<FJsonValueString>(EnumName));
				}
				Entry->SetArrayField(TEXT("values"), Names);
			}
			else if (StructField)
			{
				Entry->SetStringField(TEXT("type"), StructField->Struct ? StructField->Struct->GetName() : TEXT("struct"));
				Entry->SetStringField(TEXT("format"), TEXT("object"));
				if (IsKnownStruct(StructField->Struct))
				{
					Entry->SetStringField(TEXT("note"), TEXT("专用格式"));
				}
				else
				{
					TArray<TSharedPtr<FJsonValue>> NestedFields;
					AppendStructFieldSchema(StructField->Struct, NestedFields, VisitingStructs, Depth + 1);
					if (NestedFields.Num() > 0)
					{
						Entry->SetArrayField(TEXT("fields"), NestedFields);
					}
				}
			}
			else if (Field->IsA<FObjectPropertyBase>())
			{
				Entry->SetStringField(TEXT("type"), Field->GetCPPType());
				Entry->SetStringField(TEXT("format"), TEXT("资产路径字符串，如 /Game/UI/T_Icon.T_Icon；空值写 None"));
			}
			else if (Field->IsA<FArrayProperty>() || Field->IsA<FMapProperty>() || Field->IsA<FSetProperty>())
			{
				Entry->SetStringField(TEXT("type"), Field->GetCPPType());
				Entry->SetStringField(TEXT("format"), TEXT("string"));
				Entry->SetStringField(TEXT("hint"), TEXT("容器类型在结构体对象模式下回退为引擎文本字符串"));
			}
			else
			{
				Entry->SetStringField(TEXT("type"), Field->GetCPPType());
				Entry->SetStringField(TEXT("format"), TEXT("原生 JSON 值"));
			}

			OutFields.Add(MakeShared<FJsonValueObject>(Entry));
		}

		VisitingStructs.Remove(Struct);
	}
}

const FString FWidgetJsonSerializer::KeySchemaVersion = TEXT("schemaVersion");
const FString FWidgetJsonSerializer::KeyAssetPath = TEXT("assetPath");
const FString FWidgetJsonSerializer::KeyContentHash = TEXT("contentHash");
const FString FWidgetJsonSerializer::KeyRoot = TEXT("root");
const FString FWidgetJsonSerializer::KeyDesignSizeMode = TEXT("designSizeMode");
const FString FWidgetJsonSerializer::KeyClass = TEXT("class");
const FString FWidgetJsonSerializer::KeyClassName = TEXT("className");
const FString FWidgetJsonSerializer::KeyName = TEXT("name");
const FString FWidgetJsonSerializer::KeyIsVariable = TEXT("isVariable");
const FString FWidgetJsonSerializer::KeyProperties = TEXT("properties");
const FString FWidgetJsonSerializer::KeySlot = TEXT("slot");
const FString FWidgetJsonSerializer::KeyChildren = TEXT("children");
const FString FWidgetJsonSerializer::KeyEvents = TEXT("events");
const FString FWidgetJsonSerializer::KeyStructMode = TEXT("structMode");

const int32 FWidgetJsonSerializer::SchemaVersion = 1;

namespace
{
	/**
	 * 顺序必须与 EDesignPreviewSizeMode 完全一致（UserWidget.h，WITH_EDITORONLY_DATA）。
	 * 这个枚举只在编辑器里有，所以用名字数组而不是反射去映射。
	 */
	const TCHAR* const DesignSizeModeNames[] =
	{
		TEXT("FillScreen"),
		TEXT("Custom"),
		TEXT("CustomOnScreen"),
		TEXT("Desired"),
		TEXT("DesiredOnScreen"),
	};

	int32 FindDesignSizeModeIndex(const FString& ModeName)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(DesignSizeModeNames); ++Index)
		{
			if (ModeName.Equals(DesignSizeModeNames[Index], ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}
}

bool FWidgetJsonSerializer::IsValidDesignSizeModeName(const FString& ModeName)
{
	return FindDesignSizeModeIndex(ModeName) != INDEX_NONE;
}

bool FWidgetJsonSerializer::SetDesignSizeMode(UWidgetBlueprint* Blueprint, const FString& ModeName, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("WidgetBlueprint 无效");
		return false;
	}

	const int32 Index = FindDesignSizeModeIndex(ModeName);
	if (Index == INDEX_NONE)
	{
		OutError = FString::Printf(
			TEXT("无效的 designSizeMode '%s'（可选：FillScreen / Custom / CustomOnScreen / Desired / DesiredOnScreen）"),
			*ModeName);
		return false;
	}

	UUserWidget* CDO = Blueprint->GeneratedClass ? Cast<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
	if (!CDO)
	{
		OutError = TEXT("生成类 CDO 不可用（必须在 CompileBlueprint 之后设置 designSizeMode）");
		return false;
	}

	CDO->DesignSizeMode = static_cast<EDesignPreviewSizeMode>(Index);
	// CDO 改了要标脏，否则 SavePackages 可能跳过这个包。
	CDO->MarkPackageDirty();
	return true;
}

FString FWidgetJsonSerializer::GetDesignSizeModeName(UWidgetBlueprint* Blueprint)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return FString();
	}

	const UUserWidget* CDO = Cast<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject());
	if (!CDO)
	{
		return FString();
	}

	const int32 Index = static_cast<int32>(CDO->DesignSizeMode);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(DesignSizeModeNames)) ? FString(DesignSizeModeNames[Index]) : FString();
}


bool FWidgetJsonSerializer::ExportToJson(UWidgetBlueprint* Blueprint, FString& OutJsonText, FString& OutError, bool bStructsAsObjects)
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

	// 事件绑定表（Blueprint->Bindings）是编辑器侧的权威来源。按控件名归组后写进各节点的 events 字段，
	// 这样 JSON 往返不会丢掉绑定，AI 也能看到某个控件已经绑了哪些委托。
	TMap<FName, TArray<TSharedPtr<FJsonValue>>> EventsByWidget;
	for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
	{
		// Bindings 里既有事件绑定（Function）也有属性绑定（Property，例如把 Text 绑到一个函数）。
		// 只有 Function 才是"委托事件"，写进 events 才会被 BindToEventProperty 认。
		if (Binding.Kind != EBindingKind::Function)
		{
			continue;
		}
		const FName WidgetName(*Binding.ObjectName);
		if (WidgetName.IsNone() || Binding.PropertyName.IsNone())
		{
			continue;
		}
		EventsByWidget.FindOrAdd(WidgetName).Add(MakeShared<FJsonValueString>(Binding.PropertyName.ToString()));
	}

	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	RootObj->SetNumberField(KeySchemaVersion, SchemaVersion);
	RootObj->SetStringField(KeyAssetPath, Blueprint->GetOutermost()->GetName());
	// 只有对象模式才写字段：旧 JSON 的字节输出保持不变，缺省即 "string"。
	if (bStructsAsObjects)
	{
		RootObj->SetStringField(KeyStructMode, TEXT("object"));
	}

	const FString DesignSizeModeName = GetDesignSizeModeName(Blueprint);
	if (!DesignSizeModeName.IsEmpty())
	{
		RootObj->SetStringField(KeyDesignSizeMode, DesignSizeModeName);
	}

	RootObj->SetObjectField(KeyRoot, ExportWidgetRecursive(Blueprint->WidgetTree->RootWidget, ParentMap, AllWidgets, &EventsByWidget, bStructsAsObjects));
	RootObj->SetStringField(KeyContentHash, ComputeContentHash(RootObj));

	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJsonText);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);
	return true;
}

bool FWidgetJsonSerializer::ImportFromJson(UWidgetBlueprint* Blueprint, const FString& JsonText, FString& OutError, FString* OutDesignSizeMode)
{
	// OutError 同时充当"构建期错误累积器"，入口必须清空，避免调用方复用同一个字符串时误判。
	OutError.Reset();

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

	// designSizeMode 是 UserWidget 的编辑器预览设置，不在控件树里，导入器只能先校验并回传给调用方，
	// 等编译重建完 CDO 再写进去。校验放在构建树之前，值写错时不会动到资产。
	FString RequestedDesignSizeMode;
	if (RootObj->TryGetStringField(KeyDesignSizeMode, RequestedDesignSizeMode))
	{
		if (!IsValidDesignSizeModeName(RequestedDesignSizeMode))
		{
			OutError = FString::Printf(
				TEXT("无效的 designSizeMode '%s'（可选：FillScreen / Custom / CustomOnScreen / Desired / DesiredOnScreen）"),
				*RequestedDesignSizeMode);
			return false;
		}
	}
	if (OutDesignSizeMode)
	{
		*OutDesignSizeMode = RequestedDesignSizeMode;
	}

	const TSharedPtr<FJsonObject>* RootWidgetObj = nullptr;
	if (!RootObj->TryGetObjectField(KeyRoot, RootWidgetObj))
	{
		OutError = TEXT("缺少 root 节点");
		return false;
	}

	UWidgetTree* Tree = Blueprint->WidgetTree;

	// 先在临时 WidgetTree 上构建完整的新树。构建失败时旧树完全未被触碰，
	// 资产不会停在"树已清空但新树没建起来"的半损坏状态。
	UWidgetTree* StagingTree = NewObject<UWidgetTree>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!StagingTree)
	{
		OutError = TEXT("无法创建 staging WidgetTree");
		return false;
	}

	UWidget* NewRoot = nullptr;
	if (!ImportWidgetRecursive(StagingTree, *RootWidgetObj, nullptr, NewRoot, OutError))
	{
		return false;
	}
	if (!OutError.IsEmpty())
	{
		// 构建阶段收集到属性级错误（未知属性名、值格式不对、容器拒绝子控件）。
		// 此时还没提交，旧树原样保留，错误交给调用方写进报告文件。
		return false;
	}
	if (!NewRoot)
	{
		OutError = TEXT("导入结果没有根控件");
		return false;
	}

	// 新树已完整构建，从这里开始才修改资产。Modify() 必须在任何破坏性操作之前调用，
	// 否则事务记录的是改动后的状态，Ctrl+Z 撤销无效。
	// WidgetTree 是 Blueprint 的独立子对象（CreateDefaultSubobject），Blueprint->Modify() 不会递归到它，
	// 所以必须单独给 WidgetTree 开事务标志，才能让 RootWidget 的替换可撤销。
	Blueprint->Modify();
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// 先收集再 Reset：ForEachWidget 靠 NamedSlotBindings 才能枚举到命名槽内容，
	// 提前 Reset 会让它们既不进 OldWidgets 也不被搬走，退化成挂在 Tree 下的游离对象。
	// GetAllWidgets 自己会处理空 RootWidget，所以不加 if 判断，免得漏掉"无根但有命名槽绑定"的树。
	TArray<UWidget*> OldWidgets;
	Tree->GetAllWidgets(OldWidgets);
	Tree->NamedSlotBindings.Reset();
	Tree->RootWidget = nullptr;
	for (UWidget* Widget : OldWidgets)
	{
		// 保留事务记录（不要 REN_NonTransactional），这样旧控件的外移也能被 Ctrl+Z 撤回。
		Widget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
	}

	// 把新树上所有控件的 Outer 从 staging tree 迁到真实 tree。
	// 旧控件已改名离开真实 tree，正常情况下不会重名；若 Rename 仍被引擎追加了后缀，回读校验并报错。
	StagingTree->RootWidget = NewRoot;
	TArray<UWidget*> NewWidgets;
	StagingTree->GetAllWidgets(NewWidgets);
	TArray<FString> RenamedWidgets;
	for (UWidget* Widget : NewWidgets)
	{
		const FString ExpectedName = Widget->GetFName().ToString();
		Widget->Rename(nullptr, Tree, REN_DontCreateRedirectors | REN_DoNotDirty);
		if (Widget->GetFName().ToString() != ExpectedName)
		{
			RenamedWidgets.Add(FString::Printf(TEXT("%s -> %s"), *ExpectedName, *Widget->GetFName().ToString()));
		}
	}
	StagingTree->RootWidget = nullptr;

	if (RenamedWidgets.Num() > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("WidgetJsonBridge: 迁移到 WidgetTree 时控件被改名（事件绑定/动画会按原名找不到）：%s"),
			*FString::Join(RenamedWidgets, TEXT(", ")));
	}

	// Hierarchy 显示名必须等于对象名。SetDisplayLabel 之后 GetLabelText() 直接返回对象名，
	// 不会再被「类名_」前缀影响（Border_IconFrame 显示成 Border）。DisplayLabel 不写进 JSON，
	// 所以每次重建都从这里补回来。
	// 必须放在上面那次 Rename 之后：迁移时引擎可能给控件追加数字后缀，之前设的显示名会过期。
	for (UWidget* Widget : NewWidgets)
	{
		Widget->SetDisplayLabel(Widget->GetFName().ToString());
	}

	Tree->RootWidget = NewRoot;
	return true;
}

FString FWidgetJsonSerializer::ComputeContentHash(const TSharedPtr<FJsonObject>& RootObject)
{
	FString Canonical;
	AppendCanonical(RootObject, Canonical);

	// 与磁盘 JSON 一样走 UTF-8 字节，避免 HashAnsiString 把中文降级成 '?' 后不同界面算出同一个 hash。
	const FTCHARToUTF8 Utf8(*Canonical);
	return FMD5::HashBytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

bool FWidgetJsonSerializer::ExportClassSchema(UClass* Class, TSharedPtr<FJsonObject>& OutObj)
{
	if (!Class)
	{
		return false;
	}

	OutObj = MakeShared<FJsonObject>();
	OutObj->SetStringField(TEXT("path"), FSoftObjectPath(Class).ToString());

	TArray<FProperty*> Properties;
	CollectExportableProperties(Class, Properties);

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(Properties.Num());
	for (FProperty* Property : Properties)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Property->GetName());

		const FString Category = Property->GetMetaData(TEXT("Category"));
		if (!Category.IsEmpty())
		{
			Entry->SetStringField(TEXT("category"), Category);
		}

		UEnum* Enum = nullptr;
		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			Enum = ByteProperty->GetIntPropertyEnum();
		}
		else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProperty->GetEnum();
		}

		if (Enum)
		{
			Entry->SetStringField(TEXT("type"), Enum->GetName());
			Entry->SetStringField(TEXT("format"), TEXT("枚举名字符串"));
			TArray<TSharedPtr<FJsonValue>> Names;
			const int32 EnumCount = Enum->NumEnums();
			for (int32 Index = 0; Index < EnumCount; ++Index)
			{
				const FString EnumName = ShortEnumName(Enum, Index);
				if (EnumName.EndsWith(TEXT("_MAX")) || EnumName.EndsWith(TEXT("_MASK")))
				{
					continue;
				}
				Names.Add(MakeShared<FJsonValueString>(EnumName));
			}
			Entry->SetArrayField(TEXT("values"), Names);
		}
		else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			Entry->SetStringField(TEXT("type"), StructProperty->Struct ? StructProperty->Struct->GetName() : TEXT("struct"));
			Entry->SetStringField(TEXT("format"), TEXT("object"));
			if (IsKnownStruct(StructProperty->Struct))
			{
				Entry->SetStringField(TEXT("note"), TEXT("专用格式"));
			}
			else
			{
				// 通用对象模式：字段名就是 C++ 属性名，容器字段仍回退为引擎文本字符串。
				Entry->SetStringField(TEXT("legacyFormat"), TEXT("string"));
				Entry->SetStringField(TEXT("hint"), TEXT("必须同时写顶层 \"structMode\": \"object\" 才会按对象导出；字段见下方 fields，容器字段仍写引擎文本字符串"));
				TArray<TSharedPtr<FJsonValue>> StructFields;
				TSet<const UScriptStruct*> VisitingStructs;
				AppendStructFieldSchema(StructProperty->Struct, StructFields, VisitingStructs, 0);
				if (StructFields.Num() > 0)
				{
					Entry->SetArrayField(TEXT("fields"), StructFields);
				}
			}
		}
		else if (Property->IsA<FObjectPropertyBase>())
		{
			Entry->SetStringField(TEXT("type"), Property->GetCPPType());
			Entry->SetStringField(TEXT("format"), TEXT("资产路径字符串，如 /Game/UI/T_Icon.T_Icon；空值写 None"));
		}
		else if (Property->IsA<FArrayProperty>() || Property->IsA<FMapProperty>() || Property->IsA<FSetProperty>())
		{
			Entry->SetStringField(TEXT("type"), Property->GetCPPType());
			Entry->SetStringField(TEXT("format"), TEXT("string"));
			Entry->SetStringField(TEXT("hint"), TEXT("容器类型走引擎文本格式"));
		}
		else
		{
			Entry->SetStringField(TEXT("type"), Property->GetCPPType());
			Entry->SetStringField(TEXT("format"), TEXT("原生 JSON 值"));
		}

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	OutObj->SetArrayField(TEXT("properties"), Entries);
	return true;
}

TSharedPtr<FJsonObject> FWidgetJsonSerializer::ExportWidgetRecursive(UWidget* Widget, const TMap<UWidget*, UWidget*>& ParentMap, const TArray<UWidget*>& AllWidgets, const TMap<FName, TArray<TSharedPtr<FJsonValue>>>* EventsByWidget, bool bStructsAsObjects)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	UClass* WidgetClass = Widget->GetClass();
	Obj->SetStringField(KeyClass, FSoftObjectPath(WidgetClass).ToString());
	Obj->SetStringField(KeyClassName, WidgetClass->GetName());
	Obj->SetStringField(KeyName, Widget->GetFName().ToString());
	Obj->SetBoolField(KeyIsVariable, GetWidgetIsVariable(Widget));

	if (EventsByWidget)
	{
		if (const TArray<TSharedPtr<FJsonValue>>* Events = EventsByWidget->Find(Widget->GetFName()))
		{
			if (Events->Num() > 0)
			{
				Obj->SetArrayField(KeyEvents, *Events);
			}
		}
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<FProperty*> ExportProperties;
	CollectExportableProperties(WidgetClass, ExportProperties);
	const UObject* WidgetDefaults = WidgetClass->GetDefaultObject();
	for (FProperty* Property : ExportProperties)
	{
		// 与类默认值相同的属性不写进 JSON：导入端「未写 = 用默认值」，语义完全等价，
		// 但能让一个 TextBlock 从 36 个字段降到个位数。
		if (WidgetDefaults && Property->Identical_InContainer(Widget, WidgetDefaults))
		{
			continue;
		}
		TSet<const UScriptStruct*> VisitingStructs;
		TSharedPtr<FJsonValue> Value = ExportPropertyValue(Property, Widget, bStructsAsObjects, VisitingStructs, 0);
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
		const UObject* SlotDefaults = Slot->GetClass()->GetDefaultObject();
		for (FProperty* Property : SlotProps)
		{
			const FName PropName = Property->GetFName();
			if (PropName == TEXT("Content") || PropName == TEXT("Parent"))
			{
				continue;
			}
			if (SlotDefaults && Property->Identical_InContainer(Slot, SlotDefaults))
			{
				continue;
			}
			TSet<const UScriptStruct*> VisitingStructs;
			TSharedPtr<FJsonValue> Value = ExportPropertyValue(Property, Slot, bStructsAsObjects, VisitingStructs, 0);
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
			Children.Add(MakeShared<FJsonValueObject>(ExportWidgetRecursive(Candidate, ParentMap, AllWidgets, EventsByWidget, bStructsAsObjects)));
		}
	}
	if (Children.Num() > 0)
	{
		Obj->SetArrayField(KeyChildren, Children);
	}
	return Obj;
}

bool FWidgetJsonSerializer::ImportWidgetRecursive(UWidgetTree* Tree, const TSharedPtr<FJsonObject>& JsonObj, UWidget* Parent, UWidget*& OutWidget, FString& OutError)
{
	if (!Tree)
	{
		OutError = TEXT("目标 WidgetTree 为空");
		return false;
	}

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
	if (WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		OutError = FString::Printf(TEXT("类 '%s' 是抽象或已废弃类，不能作为控件实例"), *ClassPath);
		return false;
	}

	FString WidgetName;
	JsonObj->TryGetStringField(KeyName, WidgetName);

	// MakeUniqueObjectName 的实现保证「返回值一定不等于传入的名字」，会无条件加数字后缀，
	// 所以不能直接用它。这里优先按 JSON 里的原名创建，只有同一 WidgetTree 下真的重名时
	// 才回退到引擎的唯一名生成。否则控件名会在每次重建时漂移，动画绑定和蓝图绑定表都会失效。
	FName WidgetFName = NAME_None;
	if (!WidgetName.IsEmpty())
	{
		WidgetFName = FName(*WidgetName);
		if (StaticFindObjectFast(nullptr, Tree, WidgetFName) != nullptr)
		{
			WidgetFName = MakeUniqueObjectName(Tree, WidgetClass, WidgetFName);
			UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: 控件名 %s 在 %s 下重复，已改名为 %s"),
				*WidgetName, *Tree->GetName(), *WidgetFName.ToString());
		}
	}

	UWidget* Widget = NewObject<UWidget>(Tree, WidgetClass, WidgetFName, RF_Transactional);
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
				const FString Message = FString::Printf(TEXT("属性 %s 不存在于 %s"), *Pair.Key, *WidgetClass->GetName());
				UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: %s"), *Message);
				AppendError(OutError, Message);
				continue;
			}
			FString PropertyError;
			TSet<const UScriptStruct*> VisitingStructs;
			if (!ImportPropertyValue(Property, Widget, Pair.Value, Widget, PropertyError, VisitingStructs, 0))
			{
				const FString Message = FString::Printf(TEXT("属性 %s.%s 导入失败：%s"), *WidgetClass->GetName(), *Pair.Key, *PropertyError);
				UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: %s"), *Message);
				AppendError(OutError, Message);
			}
		}
	}

	bool bIsVariable = false;
	JsonObj->TryGetBoolField(KeyIsVariable, bIsVariable);

	// 事件绑定要求控件暴露成变量（BindToEventProperty 会在 SkeletonGeneratedClass 里按变量名查找），
	// 所以只要节点声明了 events，就自动把它标成变量。
	const TArray<TSharedPtr<FJsonValue>>* DeclaredEvents = nullptr;
	if (JsonObj->TryGetArrayField(KeyEvents, DeclaredEvents) && DeclaredEvents->Num() > 0)
	{
		bIsVariable = true;
	}

	SetWidgetIsVariable(Widget, bIsVariable);

	if (Parent)
	{
		UPanelWidget* PanelParent = Cast<UPanelWidget>(Parent);
		if (!PanelParent)
		{
			OutError = FString::Printf(TEXT("控件 %s 不能作为子控件容器"), *Parent->GetClass()->GetName());
			return false;
		}

		UPanelSlot* CreatedSlot = PanelParent->AddChild(Widget);
		if (!CreatedSlot)
		{
			// 单子容器（UButton / USizeBox / UBorder 等）已有内容时 AddChild 会返回 nullptr，
			// 以前这里静默跳过，导致子树凭空消失且没有任何提示。
			OutError = FString::Printf(TEXT("容器 %s 拒绝接受子控件 %s（单子容器只能有一个子控件）"),
				*Parent->GetClass()->GetName(), *Widget->GetClass()->GetName());
			return false;
		}

		const TSharedPtr<FJsonObject>* SlotObj = nullptr;
		if (JsonObj->TryGetObjectField(KeySlot, SlotObj))
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
						const FString Message = FString::Printf(TEXT("槽位属性 %s 不存在于 %s"), *Pair.Key, *SlotClass->GetName());
						UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: %s"), *Message);
						AppendError(OutError, Message);
						continue;
					}
					const FName PropName = Property->GetFName();
					if (PropName == TEXT("Content") || PropName == TEXT("Parent"))
					{
						continue;
					}
					FString SlotError;
					TSet<const UScriptStruct*> VisitingStructs;
					if (!ImportPropertyValue(Property, CreatedSlot, Pair.Value, CreatedSlot, SlotError, VisitingStructs, 0))
					{
						const FString Message = FString::Printf(TEXT("槽位属性 %s.%s 导入失败：%s"), *SlotClass->GetName(), *Pair.Key, *SlotError);
						UE_LOG(LogTemp, Warning, TEXT("WidgetJsonBridge: %s"), *Message);
						AppendError(OutError, Message);
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
		if (Children->Num() > 1 && Widget->IsA<UContentWidget>())
		{
			OutError = FString::Printf(TEXT("%s 是单子容器，children 最多 1 个（给了 %d 个）"),
				*Widget->GetClass()->GetName(), Children->Num());
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			const TSharedPtr<FJsonObject>* ChildObj = nullptr;
			if (!ChildValue->TryGetObject(ChildObj))
			{
				OutError = TEXT("children 中包含非对象节点");
				return false;
			}
			UWidget* ChildWidget = nullptr;
			if (!ImportWidgetRecursive(Tree, *ChildObj, Widget, ChildWidget, OutError))
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

TSharedPtr<FJsonValue> FWidgetJsonSerializer::ExportPropertyValue(FProperty* Property, const void* Container, bool bStructsAsObjects, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth)
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
				return MakeShared<FJsonValueString>(ShortEnumName(Enum, EnumIndex));
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
			return MakeShared<FJsonValueString>(ShortEnumName(EnumProperty->GetEnum(), EnumIndex));
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

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		const void* StructValue = StructProperty->ContainerPtrToValuePtr<void>(Container);
		TSharedPtr<FJsonValue> StructJson;
		if (TryExportKnownStruct(StructProperty->Struct, StructValue, StructJson))
		{
			return StructJson;
		}
		if (bStructsAsObjects)
		{
			FString StructError;
			if (ExportStructObject(StructProperty->Struct, StructValue, StructJson, StructError, VisitingStructs, Depth + 1))
			{
				return StructJson;
			}
			// 结构体对象化失败（循环引用/超深/字段不可写）时回退到文本，保持导出可用。
			UE_LOG(LogTemp, Warning,
				TEXT("WidgetJsonBridge: 结构体 %s 对象化失败，回退为引擎文本：%s"),
				StructProperty->Struct ? *StructProperty->Struct->GetName() : TEXT("<null>"),
				*StructError);
		}
	}

	FString Text;
	Property->ExportTextItem_InContainer(Text, TNotNull<const void*>(Container), nullptr, nullptr, PPF_None, nullptr);
	return MakeShared<FJsonValueString>(Text);
}

bool FWidgetJsonSerializer::ImportPropertyValue(FProperty* Property, void* Container, const TSharedPtr<FJsonValue>& JsonValue, UObject* OwnerObject, FString& OutError, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth)
{
	if (JsonValue->IsNull())
	{
		return true;
	}

	// 容器字段暂不支持对象写法：落进 ImportText("") 会静默清空，必须明确报错。
	if (Property->IsA<FArrayProperty>() || Property->IsA<FMapProperty>() || Property->IsA<FSetProperty>())
	{
		if (JsonValue->Type != EJson::String)
		{
			OutError = FString::Printf(TEXT("属性 '%s' 是容器类型，暂不支持对象写法，请写引擎文本格式字符串"), *Property->GetName());
			return false;
		}
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
			if (ValueText.IsNumeric())
			{
				FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
				Underlying->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Container), FCString::Atoi64(*ValueText));
				return true;
			}
			OutError = FString::Printf(TEXT("无效枚举值 '%s'"), *ValueText);
			return false;
		}
		FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
		Underlying->SetIntPropertyValue(EnumProperty->ContainerPtrToValuePtr<void>(Container), EnumProperty->GetEnum()->GetValueByIndex(EnumIndex));
		return true;
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (JsonValue->Type == EJson::Object)
		{
			void* StructValue = StructProperty->ContainerPtrToValuePtr<void>(Container);
			if (IsKnownStruct(StructProperty->Struct))
			{
				if (!TryImportKnownStruct(StructProperty->Struct, StructValue, JsonValue, OutError))
				{
					return false;
				}
				// 白名单结构体是直接写内存，不经过 UPROPERTY 的 setter，所以语义校验要在这里补。
				if (StructProperty->Struct == FAnchorData::StaticStruct())
				{
					return ValidateAnchorData(*static_cast<const FAnchorData*>(StructValue), OutError);
				}
				return true;
			}

			// 非白名单结构体：JSON 是对象就按通用对象模式导入，是字符串则继续走下面的 ImportText（旧文件兼容）。
			const TSharedPtr<FJsonObject>* StructObj = nullptr;
			if (!JsonValue->TryGetObject(StructObj) || !StructObj || !(*StructObj).IsValid())
			{
				OutError = TEXT("结构体对象格式无效");
				return false;
			}
			return ImportStructObject(StructProperty->Struct, StructValue, *StructObj, OwnerObject, OutError, VisitingStructs, Depth + 1);
		}
	}

	FString ValueText;
	switch (JsonValue->Type)
	{
	case EJson::Boolean:
		ValueText = JsonValue->AsBool() ? TEXT("true") : TEXT("false");
		break;
	case EJson::Number:
		// SanitizeFloat 默认只有 6 位有效数字，double 属性会丢精度；用 %.17g 保证往返无损。
		ValueText = FString::Printf(TEXT("%.17g"), JsonValue->AsNumber());
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

	// 引擎文本格式的 LayoutData 也走同一条校验：Designer 导出的正常值不会触发，只有手写错坐标才会。
	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (StructProperty->Struct == FAnchorData::StaticStruct())
		{
			return ValidateAnchorData(*static_cast<const FAnchorData*>(StructProperty->ContainerPtrToValuePtr<void>(Container)), OutError);
		}
	}
	return true;
}

bool FWidgetJsonSerializer::ExportStructObject(const UScriptStruct* Struct, const void* ValuePtr, TSharedPtr<FJsonValue>& OutValue, FString& OutError, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth)
{
	if (!Struct || !ValuePtr)
	{
		OutError = TEXT("结构体指针无效");
		return false;
	}
	if (Depth > MaxStructObjectDepth)
	{
		OutError = FString::Printf(TEXT("结构体 %s 嵌套超过 %d 层"), *Struct->GetName(), MaxStructObjectDepth);
		return false;
	}
	if (VisitingStructs.Contains(Struct))
	{
		OutError = FString::Printf(TEXT("结构体 %s 出现循环引用，无法对象化"), *Struct->GetName());
		return false;
	}
	VisitingStructs.Add(Struct);

	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Field = *It;
		if (!IsStructObjectField(Field))
		{
			continue;
		}

		// 对象引用写成资产路径字符串（FSlateBrush.ResourceObject 就属于这一类）。
		if (FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Field))
		{
			const FSoftObjectPtr& SoftPtr = SoftProperty->GetPropertyValue_InContainer(ValuePtr);
			FString PathText = TEXT("None");
			if (!SoftPtr.IsNull())
			{
				const FSoftObjectPath SoftPath = SoftPtr.ToSoftObjectPath();
				if (IsResolvableObjectPath(SoftPath))
				{
					PathText = SoftPath.ToString();
				}
			}
			Obj->SetStringField(Field->GetName(), PathText);
			continue;
		}
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Field))
		{
			UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue_InContainer(ValuePtr);
			FString PathText = TEXT("None");
			if (ReferencedObject && !ReferencedObject->HasAnyFlags(RF_Transient))
			{
				const FSoftObjectPath ObjectPath(ReferencedObject);
				if (IsResolvableObjectPath(ObjectPath))
				{
					PathText = ObjectPath.ToString();
				}
			}
			Obj->SetStringField(Field->GetName(), PathText);
			continue;
		}

		// 容器类型没有对象化，ExportPropertyValue 会回退成引擎文本字符串。
		TSharedPtr<FJsonValue> FieldValue = ExportPropertyValue(Field, ValuePtr, true, VisitingStructs, Depth);
		if (FieldValue.IsValid())
		{
			Obj->SetField(Field->GetName(), FieldValue);
		}
	}

	VisitingStructs.Remove(Struct);
	OutValue = MakeShared<FJsonValueObject>(Obj);
	return true;
}

bool FWidgetJsonSerializer::ImportStructObject(const UScriptStruct* Struct, void* ValuePtr, const TSharedPtr<FJsonObject>& Obj, UObject* OwnerObject, FString& OutError, TSet<const UScriptStruct*>& VisitingStructs, int32 Depth)
{
	if (!Struct || !ValuePtr || !Obj.IsValid())
	{
		OutError = TEXT("结构体对象无效");
		return false;
	}
	if (Depth > MaxStructObjectDepth)
	{
		OutError = FString::Printf(TEXT("结构体 %s 嵌套超过 %d 层"), *Struct->GetName(), MaxStructObjectDepth);
		return false;
	}
	if (VisitingStructs.Contains(Struct))
	{
		OutError = FString::Printf(TEXT("结构体 %s 出现循环引用，无法解析"), *Struct->GetName());
		return false;
	}
	VisitingStructs.Add(Struct);

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->IsNull())
		{
			continue;
		}

		FProperty* Field = Struct->FindPropertyByName(*Pair.Key);
		if (!Field)
		{
			OutError = FString::Printf(TEXT("结构体 %s 没有字段 %s"), *Struct->GetName(), *Pair.Key);
			VisitingStructs.Remove(Struct);
			return false;
		}
		if (!IsStructObjectField(Field))
		{
			OutError = FString::Printf(TEXT("结构体 %s 的字段 %s 不可写"), *Struct->GetName(), *Pair.Key);
			VisitingStructs.Remove(Struct);
			return false;
		}

		// 对象引用字段写路径字符串；空值写 None。
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Field))
		{
			FString PathText;
			if (!Pair.Value->TryGetString(PathText))
			{
				OutError = FString::Printf(TEXT("结构体 %s 的对象引用字段 %s 必须是路径字符串"), *Struct->GetName(), *Pair.Key);
				VisitingStructs.Remove(Struct);
				return false;
			}

			const FString NormalizedPath = NormalizeObjectPathText(PathText);
			if (NormalizedPath.IsEmpty() || NormalizedPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				ObjectProperty->SetObjectPropertyValue_InContainer(ValuePtr, nullptr);
				continue;
			}

			const FSoftObjectPath SoftPath(NormalizedPath);
			UObject* LoadedObject = SoftPath.TryLoad();
			if (!LoadedObject)
			{
				OutError = FString::Printf(TEXT("结构体 %s 的字段 %s 无法加载对象 '%s'"), *Struct->GetName(), *Pair.Key, *NormalizedPath);
				VisitingStructs.Remove(Struct);
				return false;
			}
			ObjectProperty->SetObjectPropertyValue_InContainer(ValuePtr, LoadedObject);
			continue;
		}

		FString FieldError;
		if (!ImportPropertyValue(Field, ValuePtr, Pair.Value, OwnerObject, FieldError, VisitingStructs, Depth))
		{
			OutError = FString::Printf(TEXT("结构体 %s 的字段 %s 导入失败：%s"), *Struct->GetName(), *Pair.Key, *FieldError);
			VisitingStructs.Remove(Struct);
			return false;
		}
	}

	VisitingStructs.Remove(Struct);
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
		if (KeyStr == KeySchemaVersion || KeyStr == KeyAssetPath || KeyStr == KeyContentHash || KeyStr == KeyDesignSizeMode || KeyStr == KeyStructMode)
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

