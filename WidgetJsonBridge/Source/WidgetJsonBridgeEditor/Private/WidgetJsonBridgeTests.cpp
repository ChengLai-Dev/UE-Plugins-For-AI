#include "Misc/AutomationTest.h"
#include "WidgetJsonSerializer.h"
#include "WidgetJsonSync.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "UObject/UnrealType.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

static UPanelSlot* GetWidgetSlotForTest(UWidget* Widget)
{
	FProperty* SlotProperty = Widget->GetClass()->FindPropertyByName(TEXT("Slot"));
	if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(SlotProperty))
	{
		return Cast<UPanelSlot>(ObjectProperty->GetObjectPropertyValue(ObjectProperty->ContainerPtrToValuePtr<void>(Widget)));
	}
	return nullptr;
}

/** 在导出的控件树 JSON 里按名字找节点，用于断言结构体字段的 JSON 类型。 */
static TSharedPtr<FJsonObject> FindWidgetNodeForTest(const TSharedPtr<FJsonObject>& Node, const FString& WidgetName)
{
	if (!Node.IsValid())
	{
		return nullptr;
	}

	FString NodeName;
	Node->TryGetStringField(TEXT("name"), NodeName);
	if (NodeName == WidgetName)
	{
		return Node;
	}

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node->TryGetArrayField(TEXT("children"), Children) && Children)
	{
		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			const TSharedPtr<FJsonObject>* ChildObj = nullptr;
			if (ChildValue.IsValid() && ChildValue->TryGetObject(ChildObj) && ChildObj)
			{
				TSharedPtr<FJsonObject> FoundNode = FindWidgetNodeForTest(*ChildObj, WidgetName);
				if (FoundNode.IsValid())
				{
					return FoundNode;
				}
			}
		}
	}
	return nullptr;
}

/** 取一个控件节点 properties 里的值；找不到返回空指针。 */
static TSharedPtr<FJsonValue> FindWidgetPropertyForTest(const TSharedPtr<FJsonObject>& RootNode, const FString& WidgetName, const FString& PropertyName)
{
	const TSharedPtr<FJsonObject> WidgetNode = FindWidgetNodeForTest(RootNode, WidgetName);
	if (!WidgetNode.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!WidgetNode->TryGetObjectField(TEXT("properties"), PropertiesObj) || !PropertiesObj)
	{
		return nullptr;
	}
	return (*PropertiesObj)->TryGetField(PropertyName);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeRoundTripTest, "WidgetJsonBridge.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeRoundTripTest::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* Blueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UWidgetTree* Tree = Blueprint->WidgetTree;
	TestNotNull(TEXT("WidgetTree 存在"), Tree);

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Tree, UCanvasPanel::StaticClass(), TEXT("CanvasPanel_0"), RF_Transactional);
	Tree->RootWidget = RootPanel;

	UTextBlock* TextWidget = NewObject<UTextBlock>(Tree, UTextBlock::StaticClass(), TEXT("Text_Test"), RF_Transactional);
	TextWidget->SetText(FText::FromString(TEXT("Hello Sync")));
	UPanelSlot* PanelSlot = RootPanel->AddChild(TextWidget);
	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(PanelSlot);
	TestNotNull(TEXT("CanvasPanelSlot 创建"), CanvasSlot);
	if (CanvasSlot)
	{
		CanvasSlot->SetPosition(FVector2D(100.0f, 50.0f));
	}

	FString JsonText;
	FString Error;
	TestTrue(TEXT("导出成功"), FWidgetJsonSerializer::ExportToJson(Blueprint, JsonText, Error));
	TestTrue(TEXT("JSON 含控件名"), JsonText.Contains(TEXT("Text_Test")));
	TestTrue(TEXT("JSON 含文本"), JsonText.Contains(TEXT("Hello Sync")));
	TestTrue(TEXT("JSON 含 slot 位置"), JsonText.Contains(TEXT("100")));
	TestTrue(TEXT("JSON 含 className"), JsonText.Contains(TEXT("CanvasPanel")));
	TestTrue(TEXT("JSON 含 isVariable"), JsonText.Contains(TEXT("isVariable")));
	TestTrue(TEXT("JSON 含 contentHash"), JsonText.Contains(TEXT("contentHash")));

	JsonText = JsonText.Replace(TEXT("Hello Sync"), TEXT("World Sync"));

	TestTrue(TEXT("导入成功"), FWidgetJsonSerializer::ImportFromJson(Blueprint, JsonText, Error));

	TArray<UWidget*> AllWidgets;
	Tree->GetAllWidgets(AllWidgets);
	UTextBlock* ImportedText = nullptr;
	UCanvasPanel* ImportedRoot = nullptr;
	for (UWidget* Widget : AllWidgets)
	{
		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			ImportedText = Text;
		}
		if (UCanvasPanel* Panel = Cast<UCanvasPanel>(Widget))
		{
			ImportedRoot = Panel;
		}
	}
	TestNotNull(TEXT("导入后根控件存在"), ImportedRoot);
	TestNotNull(TEXT("导入后 TextBlock 存在"), ImportedText);
	if (ImportedRoot)
	{
		TestEqual(TEXT("根控件名保持 JSON 原名"), ImportedRoot->GetFName().ToString(), TEXT("CanvasPanel_0"));
	}
	if (ImportedText)
	{
		TestEqual(TEXT("子控件名保持 JSON 原名"), ImportedText->GetFName().ToString(), TEXT("Text_Test"));
		TestEqual(TEXT("文本已更新"), ImportedText->GetText().ToString(), TEXT("World Sync"));
	}
	if (ImportedRoot)
	{
		TArray<UWidget*> Children = ImportedRoot->GetAllChildren();
		TestEqual(TEXT("子控件数量"), Children.Num(), 1);
		if (Children.Num() == 1)
		{
			UCanvasPanelSlot* ImportedSlot = Cast<UCanvasPanelSlot>(GetWidgetSlotForTest(Children[0]));
			TestNotNull(TEXT("导入后 slot 存在"), ImportedSlot);
			if (ImportedSlot)
			{
				TestEqual(TEXT("slot 位置 X"), ImportedSlot->GetPosition().X, 100.0);
				TestEqual(TEXT("slot 位置 Y"), ImportedSlot->GetPosition().Y, 50.0);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeCanvasSlotTest, "WidgetJsonBridge.CanvasSlotLayout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeCanvasSlotTest::RunTest(const FString& Parameters)
{
	// 契约：点锚点（min == max）时 Offsets 是"位置 + 宽高"，right/bottom 就是宽和高。
	// 依据 SConstraintCanvas::ArrangeLayeredChildren（Size = (Offset.Right, Offset.Bottom)）
	// 和 UCanvasPanelSlot::SetSize（LayoutData.Offsets.Right = Size.X），
	// 也就是 Designer Details 面板里显示的 Position X/Y + Size X/Y。导入必须原样落进 LayoutData。
	const FString GoodJson = TEXT(R"({
		"schemaVersion": 1,
		"root": {
			"class": "/Script/UMG.CanvasPanel",
			"name": "Root_Canvas",
			"children": [
				{
					"class": "/Script/UMG.TextBlock",
					"name": "Text_Layout",
					"properties": { "Text": "Layout" },
					"slot": {
						"class": "/Script/UMG.CanvasPanelSlot",
						"properties": {
							"LayoutData": {
								"offsets": { "left": -100, "top": -40, "right": 200, "bottom": 80 },
								"anchors": { "min": { "x": 0.5, "y": 0.5 }, "max": { "x": 0.5, "y": 0.5 } },
								"alignment": { "x": 0.5, "y": 0.5 }
							}
						}
					}
				}
			]
		}
	})");

	UWidgetBlueprint* Blueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	FString Error;
	TestTrue(TEXT("点锚点 LayoutData 导入成功"), FWidgetJsonSerializer::ImportFromJson(Blueprint, GoodJson, Error));
	TestTrue(TEXT("导入无错误"), Error.IsEmpty());

	UWidget* LayoutWidget = Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(TEXT("Text_Layout")) : nullptr;
	TestNotNull(TEXT("带 LayoutData 的控件存在"), LayoutWidget);
	if (LayoutWidget)
	{
		UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(GetWidgetSlotForTest(LayoutWidget));
		TestNotNull(TEXT("槽对象是 CanvasPanelSlot"), CanvasSlot);
		if (CanvasSlot)
		{
			TestEqual(TEXT("位置 X 原样保留"), CanvasSlot->GetPosition().X, -100.0);
			TestEqual(TEXT("位置 Y 原样保留"), CanvasSlot->GetPosition().Y, -40.0);
			TestEqual(TEXT("Size X 就是 offsets.right"), CanvasSlot->GetSize().X, 200.0);
			TestEqual(TEXT("Size Y 就是 offsets.bottom"), CanvasSlot->GetSize().Y, 80.0);
			TestEqual(TEXT("锚点最小 X"), CanvasSlot->GetAnchors().Minimum.X, 0.5);
			TestEqual(TEXT("锚点最大 Y"), CanvasSlot->GetAnchors().Maximum.Y, 0.5);
			TestEqual(TEXT("Alignment Y"), CanvasSlot->GetAlignment().Y, 0.5);
		}
	}

	// 把 right/bottom 当成"右下边界坐标"（点锚点下 right < left，等于负宽度）必须报错。
	// 这种槽位在 Slate 里是负尺寸矩形，画不出来也点不到；静默导入会让错误一路传到 Designer。
	// 用 96x96 的格子当旧树，验证报错后旧树没被破坏。
	UWidgetBlueprint* BadBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UWidgetTree* BadTree = BadBlueprint->WidgetTree;
	TestNotNull(TEXT("坏用例的 WidgetTree 存在"), BadTree);
	if (!BadTree)
	{
		return false;
	}
	UCanvasPanel* KeptRoot = NewObject<UCanvasPanel>(BadTree, UCanvasPanel::StaticClass(), TEXT("CanvasPanel_Keep"), RF_Transactional);
	BadTree->RootWidget = KeptRoot;

	const FString CornerStyleJson = TEXT(R"({
		"schemaVersion": 1,
		"root": {
			"class": "/Script/UMG.CanvasPanel",
			"name": "Root_Canvas",
			"children": [
				{
					"class": "/Script/UMG.TextBlock",
					"name": "Text_Corner",
					"slot": {
						"class": "/Script/UMG.CanvasPanelSlot",
						"properties": {
							"LayoutData": {
								"offsets": { "left": -480, "top": -260, "right": -360, "bottom": -96 },
								"anchors": { "min": { "x": 0.5, "y": 0.5 }, "max": { "x": 0.5, "y": 0.5 } }
							}
						}
					}
				}
			]
		}
	})");

	FString CornerError;
	TestFalse(TEXT("写成右下边界坐标应导入失败"), FWidgetJsonSerializer::ImportFromJson(BadBlueprint, CornerStyleJson, CornerError));
	TestTrue(TEXT("错误信息点名 right"), CornerError.Contains(TEXT("right")));
	TestTrue(TEXT("错误信息说明 right 是宽度"), CornerError.Contains(TEXT("宽度")));
	TestTrue(TEXT("失败后旧根控件保留"), BadTree->RootWidget == KeptRoot);

	// 拉伸锚点下 right/bottom 是相对右/下边的边距，负值合法，不能被上面的校验误判。
	const FString StretchedJson = TEXT(R"({
		"schemaVersion": 1,
		"root": {
			"class": "/Script/UMG.CanvasPanel",
			"name": "Root_Canvas",
			"children": [
				{
					"class": "/Script/UMG.TextBlock",
					"name": "Text_Stretch",
					"slot": {
						"class": "/Script/UMG.CanvasPanelSlot",
						"properties": {
							"LayoutData": {
								"offsets": { "left": 8, "top": 8, "right": -8, "bottom": -8 },
								"anchors": { "min": { "x": 0.0, "y": 0.0 }, "max": { "x": 1.0, "y": 1.0 } }
							}
						}
					}
				}
			]
		}
	})");

	UWidgetBlueprint* StretchBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	FString StretchError;
	TestTrue(TEXT("拉伸锚点的负边距允许"), FWidgetJsonSerializer::ImportFromJson(StretchBlueprint, StretchedJson, StretchError));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeExportTest, "WidgetJsonBridge.ExportToFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeExportTest::RunTest(const FString& Parameters)
{
	// 这个用例依赖 UIWidgets/Test/SyncTest.json 作为输入。全新项目里没有这个夹具，
	// 以前只能靠别的工程带过来，导致这条测试在任何"干净"项目上必然失败。缺文件就自己补一份最小界面。
	FString SyncJsonPath;
	if (FWidgetJsonSync::AssetPathToJsonPath(TEXT("/Game/Test/SyncTest"), SyncJsonPath)
		&& !IFileManager::Get().FileExists(*SyncJsonPath))
	{
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(SyncJsonPath));
		static const FString FixtureJson = TEXT(R"({
	"schemaVersion": 1,
	"root": {
		"class": "/Script/UMG.CanvasPanel",
		"name": "CanvasPanel_0",
		"children": [
			{
				"class": "/Script/UMG.TextBlock",
				"name": "Text_Sync",
				"properties": { "Text": "Sync Test" }
			}
		]
	}
})");
		FFileHelper::SaveStringToFile(FixtureJson, *SyncJsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FWidgetJsonSync::Get().Tick();

	UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, TEXT("/Game/Test/SyncTest"));
	TestNotNull(TEXT("SyncTest 资产由 JSON 导入创建"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	TArray<UWidget*> AllWidgets;
	Blueprint->WidgetTree->GetAllWidgets(AllWidgets);
	UTextBlock* TextWidget = nullptr;
	for (UWidget* Widget : AllWidgets)
	{
		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			TextWidget = Text;
		}
	}
	TestNotNull(TEXT("资产内 TextBlock 存在"), TextWidget);
	if (!TextWidget)
	{
		return false;
	}

	TextWidget->SetText(FText::FromString(TEXT("Editor Edited")));
	Blueprint->MarkPackageDirty();

	FString Error;
	TestTrue(TEXT("导出到文件成功"), FWidgetJsonSync::Get().ExportAsset(Blueprint, Error));

	FString JsonPath;
	FWidgetJsonSync::AssetPathToJsonPath(Blueprint->GetOutermost()->GetName(), JsonPath);
	FString DiskText;
	TestTrue(TEXT("磁盘 JSON 可读"), FFileHelper::LoadFileToString(DiskText, *JsonPath));
	TestTrue(TEXT("磁盘 JSON 含编辑器修改的文本"), DiskText.Contains(TEXT("Editor Edited")));
	TestTrue(TEXT("磁盘 JSON 含 contentHash"), DiskText.Contains(TEXT("contentHash")));

	FString ExportText;
	FString ExportError;
	// 与自动导出走同一套模式读取：磁盘 JSON 若带 structMode: object，比较文本也必须按对象模式生成。
	TestTrue(TEXT("导出文本与磁盘一致"), FWidgetJsonSync::ExportToJsonForPath(Blueprint, JsonPath, ExportText, ExportError));
	TestEqual(TEXT("磁盘内容与导出内容一致"), DiskText, ExportText);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeMappingTest, "WidgetJsonBridge.MappingAndHash", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeMappingTest::RunTest(const FString& Parameters)
{
	FString JsonPath;
	TestTrue(TEXT("资产路径映射"), FWidgetJsonSync::AssetPathToJsonPath(TEXT("/Game/HUD/MainMenu"), JsonPath));
	TestTrue(TEXT("JSON 路径在 UIWidgets 下"), JsonPath.Contains(TEXT("UIWidgets")));
	TestTrue(TEXT("JSON 扩展名"), JsonPath.EndsWith(TEXT(".json")));

	FString AssetPath;
	TestTrue(TEXT("JSON 路径逆映射"), FWidgetJsonSync::JsonPathToAssetPath(JsonPath, AssetPath));
	TestEqual(TEXT("逆映射一致"), AssetPath, TEXT("/Game/HUD/MainMenu"));

	TestFalse(TEXT("非 Game 资产拒绝"), FWidgetJsonSync::AssetPathToJsonPath(TEXT("/Engine/UI/Widget"), JsonPath));
	TestFalse(TEXT("目录外 JSON 拒绝"), FWidgetJsonSync::JsonPathToAssetPath(TEXT("D:/Other/Widget.json"), AssetPath));
	TestFalse(TEXT("同前缀兄弟目录拒绝"), FWidgetJsonSync::JsonPathToAssetPath(JsonPath.Replace(TEXT("UIWidgets"), TEXT("UIWidgets_backup")), AssetPath));

	const FString SampleJson = TEXT("{\"schemaVersion\":1,\"assetPath\":\"/Game/Test/WBP_A\",\"contentHash\":\"\",\"root\":{\"class\":\"/Script/UMG.CanvasPanel\",\"name\":\"Root\"}}");
	TSharedPtr<FJsonObject> Parsed;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(SampleJson);
	FJsonSerializer::Deserialize(Reader, Parsed);
	const FString HashA = FWidgetJsonSerializer::ComputeContentHash(Parsed);
	TestTrue(TEXT("hash 非空"), !HashA.IsEmpty());
	const FString HashB = FWidgetJsonSerializer::ComputeContentHash(Parsed);
	TestEqual(TEXT("hash 稳定"), HashA, HashB);

	TSharedPtr<FJsonObject> Parsed2;
	TSharedRef<TJsonReader<TCHAR>> Reader2 = TJsonReaderFactory<TCHAR>::Create(SampleJson.Replace(TEXT("Root"), TEXT("RootRenamed")));
	FJsonSerializer::Deserialize(Reader2, Parsed2);
	const FString HashC = FWidgetJsonSerializer::ComputeContentHash(Parsed2);
	TestNotEqual(TEXT("内容变化 hash 变化"), HashA, HashC);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeImportFailureTest, "WidgetJsonBridge.ImportFailureKeepsTree", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeImportFailureTest::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* Blueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UWidgetTree* Tree = Blueprint->WidgetTree;
	TestNotNull(TEXT("WidgetTree 存在"), Tree);
	if (!Tree)
	{
		return false;
	}

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Tree, UCanvasPanel::StaticClass(), TEXT("CanvasPanel_Keep"), RF_Transactional);
	Tree->RootWidget = RootPanel;

	UTextBlock* TextWidget = NewObject<UTextBlock>(Tree, UTextBlock::StaticClass(), TEXT("Text_Keep"), RF_Transactional);
	TextWidget->SetText(FText::FromString(TEXT("Keep Me")));
	RootPanel->AddChild(TextWidget);

	// 子节点缺 class 字段，必然在导入中途失败。
	const FString BadJson = TEXT("{\"schemaVersion\":1,\"root\":{\"class\":\"/Script/UMG.CanvasPanel\",\"name\":\"Root\",\"children\":[{\"name\":\"BrokenChild\"}]}}");

	FString Error;
	TestFalse(TEXT("坏 JSON 导入应失败"), FWidgetJsonSerializer::ImportFromJson(Blueprint, BadJson, Error));
	TestTrue(TEXT("失败时返回错误信息"), !Error.IsEmpty());

	TestTrue(TEXT("失败后旧根控件仍在"), Tree->RootWidget == RootPanel);

	TArray<UWidget*> AllWidgets;
	Tree->GetAllWidgets(AllWidgets);
	UTextBlock* Survivor = nullptr;
	for (UWidget* Widget : AllWidgets)
	{
		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			Survivor = Text;
		}
	}
	TestNotNull(TEXT("失败后旧 TextBlock 仍在"), Survivor);
	if (Survivor)
	{
		TestEqual(TEXT("失败后旧文本未被破坏"), Survivor->GetText().ToString(), TEXT("Keep Me"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeSchemaTest, "WidgetJsonBridge.ExportSchema", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeSchemaTest::RunTest(const FString& Parameters)
{
	// 确保 UMG 的常用类已经加载，否则 GetDerivedClasses 收不到它们。
	LoadObject<UClass>(nullptr, TEXT("/Script/UMG.TextBlock"));
	LoadObject<UClass>(nullptr, TEXT("/Script/UMG.CanvasPanelSlot"));

	FString OutPath;
	FString Error;
	TestTrue(TEXT("Schema 导出成功"), FWidgetJsonSync::ExportPropertySchemaToFile(OutPath, Error));
	TestTrue(TEXT("导出路径非空"), !OutPath.IsEmpty());

	FString IndexText;
	TestTrue(TEXT("Schema 索引可读"), FFileHelper::LoadFileToString(IndexText, *OutPath));
	TestTrue(TEXT("索引包含 TextBlock"), IndexText.Contains(TEXT("TextBlock")));
	TestTrue(TEXT("索引包含 CanvasPanelSlot"), IndexText.Contains(TEXT("CanvasPanelSlot")));

	const FString SchemaDir = FPaths::GetPath(OutPath);
	FString TextBlockSchema;
	TestTrue(TEXT("TextBlock Schema 可读"),
		FFileHelper::LoadFileToString(TextBlockSchema, *FPaths::Combine(SchemaDir, TEXT("widgets"), TEXT("TextBlock.json"))));
	TestTrue(TEXT("TextBlock 含 Text 属性"), TextBlockSchema.Contains(TEXT("\"Text\"")));
	TestTrue(TEXT("TextBlock 含原生 JSON 格式标记"), TextBlockSchema.Contains(TEXT("原生 JSON 值")));

	FString CanvasSlotSchema;
	TestTrue(TEXT("CanvasPanelSlot Schema 可读"),
		FFileHelper::LoadFileToString(CanvasSlotSchema, *FPaths::Combine(SchemaDir, TEXT("slots"), TEXT("CanvasPanelSlot.json"))));
	TestTrue(TEXT("CanvasPanelSlot 含 LayoutData"), CanvasSlotSchema.Contains(TEXT("LayoutData")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeEventBindingTest, "WidgetJsonBridge.EventBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeEventBindingTest::RunTest(const FString& Parameters)
{
	// 1. JSON 里声明 events 的控件应自动变成变量（BindToEventProperty 要求它是变量）
	const FString EventJson = TEXT("{\"schemaVersion\":1,\"root\":{\"class\":\"/Script/UMG.CanvasPanel\",\"name\":\"Root_Canvas\",\"children\":[{\"class\":\"/Script/UMG.Button\",\"name\":\"Button_A\",\"isVariable\":false,\"events\":[\"OnClicked\"]}]}}");

	UWidgetBlueprint* DeclaredBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	FString DeclaredError;
	TestTrue(TEXT("带 events 的 JSON 导入成功"), FWidgetJsonSerializer::ImportFromJson(DeclaredBlueprint, EventJson, DeclaredError));

	UWidget* ButtonA = DeclaredBlueprint->WidgetTree ? DeclaredBlueprint->WidgetTree->FindWidget(TEXT("Button_A")) : nullptr;
	TestNotNull(TEXT("Button_A 存在"), ButtonA);
	if (ButtonA)
	{
		TestTrue(TEXT("声明 events 的控件自动成为变量"), ButtonA->bIsVariable);
	}

	// 2. 已有绑定表应被导出成 events，且导入时不被清空
	UWidgetBlueprint* Blueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UWidgetTree* Tree = Blueprint->WidgetTree;
	TestNotNull(TEXT("WidgetTree 存在"), Tree);
	if (!Tree)
	{
		return false;
	}

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Tree, UCanvasPanel::StaticClass(), TEXT("CanvasPanel_0"), RF_Transactional);
	Tree->RootWidget = RootPanel;
	UButton* ButtonWidget = NewObject<UButton>(Tree, UButton::StaticClass(), TEXT("Button_Submit"), RF_Transactional);
	ButtonWidget->bIsVariable = true;
	RootPanel->AddChild(ButtonWidget);

	FDelegateEditorBinding ManualBinding;
	ManualBinding.ObjectName = TEXT("Button_Submit");
	ManualBinding.PropertyName = TEXT("OnClicked");
	ManualBinding.FunctionName = TEXT("OnSubmitClicked");
	ManualBinding.Kind = EBindingKind::Function;
	Blueprint->Bindings.Add(ManualBinding);

	FString JsonText;
	FString Error;
	TestTrue(TEXT("导出成功"), FWidgetJsonSerializer::ExportToJson(Blueprint, JsonText, Error));
	TestTrue(TEXT("导出结果含 events 字段"), JsonText.Contains(TEXT("\"events\"")));
	TestTrue(TEXT("导出结果含 OnClicked"), JsonText.Contains(TEXT("OnClicked")));
	TestTrue(TEXT("导出结果含控件变量标记"), JsonText.Contains(TEXT("\"isVariable\": true")));

	TestTrue(TEXT("重新导入成功"), FWidgetJsonSerializer::ImportFromJson(Blueprint, JsonText, Error));
	TestEqual(TEXT("导入后绑定表未清空"), Blueprint->Bindings.Num(), 1);
	if (Blueprint->Bindings.Num() == 1)
	{
		TestEqual(TEXT("绑定控件名保留"), Blueprint->Bindings[0].ObjectName, FString(TEXT("Button_Submit")));
		TestEqual(TEXT("绑定委托名保留"), Blueprint->Bindings[0].PropertyName.ToString(), FString(TEXT("OnClicked")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeUtf8Test, "WidgetJsonBridge.Utf8RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeUtf8Test::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* Blueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UWidgetTree* Tree = Blueprint->WidgetTree;
	TestNotNull(TEXT("WidgetTree 存在"), Tree);
	if (!Tree)
	{
		return false;
	}

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Tree, UCanvasPanel::StaticClass(), TEXT("CanvasPanel_0"), RF_Transactional);
	Tree->RootWidget = RootPanel;
	UTextBlock* TextWidget = NewObject<UTextBlock>(Tree, UTextBlock::StaticClass(), TEXT("Text_Utf8"), RF_Transactional);
	TextWidget->SetText(FText::FromString(TEXT("开始游戏 · 设置")));
	RootPanel->AddChild(TextWidget);

	FString JsonText;
	FString Error;
	TestTrue(TEXT("导出成功"), FWidgetJsonSerializer::ExportToJson(Blueprint, JsonText, Error));
	TestTrue(TEXT("JSON 保留中文"), JsonText.Contains(TEXT("开始游戏")));

	const FString EditedJson = JsonText.Replace(TEXT("开始游戏"), TEXT("继续冒险"));
	TestTrue(TEXT("导入成功"), FWidgetJsonSerializer::ImportFromJson(Blueprint, EditedJson, Error));

	UTextBlock* Imported = nullptr;
	TArray<UWidget*> AllWidgets;
	Tree->GetAllWidgets(AllWidgets);
	for (UWidget* Widget : AllWidgets)
	{
		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			Imported = Text;
		}
	}
	TestNotNull(TEXT("导入后 TextBlock 存在"), Imported);
	if (Imported)
	{
		TestEqual(TEXT("中文往返正确"), Imported->GetText().ToString(), FString(TEXT("继续冒险 · 设置")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeStructObjectTest, "WidgetJsonBridge.StructObjectMode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeStructObjectTest::RunTest(const FString& Parameters)
{
	UWidgetBlueprint* Blueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	UWidgetTree* Tree = Blueprint->WidgetTree;
	TestNotNull(TEXT("WidgetTree 存在"), Tree);
	if (!Tree)
	{
		return false;
	}

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Tree, UCanvasPanel::StaticClass(), TEXT("CanvasPanel_Struct"), RF_Transactional);
	Tree->RootWidget = RootPanel;

	UProgressBar* ProgressBarWidget = NewObject<UProgressBar>(Tree, UProgressBar::StaticClass(), TEXT("Progress_Style"), RF_Transactional);
	TestNotNull(TEXT("ProgressBar 创建"), ProgressBarWidget);
	if (!ProgressBarWidget)
	{
		return false;
	}

	ProgressBarWidget->SetPercent(0.42f);
	const FLinearColor FillColor(0.1f, 0.2f, 0.3f, 1.0f);
	ProgressBarWidget->SetFillColorAndOpacity(FillColor);

	// 改出一组非默认的 Brush 字段，确保 WidgetStyle 与 CDO 不同，导出不会被差异过滤掉。
	const FLinearColor FillTint(1.0f, 0.25f, 0.5f, 1.0f);
	const FVector4 CornerRadii(1.0, 2.0, 3.0, 4.0);
	const FMargin BrushMargin(1.0f, 2.0f, 3.0f, 4.0f);
	FProgressBarStyle Style = ProgressBarWidget->GetWidgetStyle();
	Style.FillImage.TintColor = FSlateColor(FillTint);
	Style.FillImage.DrawAs = ESlateBrushDrawType::Box;
	Style.FillImage.Tiling = ESlateBrushTileType::Both;
	Style.FillImage.Mirroring = ESlateBrushMirrorType::Horizontal;
	Style.FillImage.ImageType = ESlateBrushImageType::Linear;
	Style.FillImage.ImageSize.X = 24.0f;
	Style.FillImage.ImageSize.Y = 16.0f;
	Style.FillImage.Margin = BrushMargin;
	Style.FillImage.OutlineSettings.CornerRadii = CornerRadii;
	// ResourceName 是 protected UPROPERTY，测试用反射写入（插件本身也是通过反射读写它）。
	FNameProperty* ResourceNameProperty = CastField<FNameProperty>(FSlateBrush::StaticStruct()->FindPropertyByName(TEXT("ResourceName")));
	TestNotNull(TEXT("FSlateBrush.ResourceName 可反射写入"), ResourceNameProperty);
	if (ResourceNameProperty)
	{
		ResourceNameProperty->SetPropertyValue_InContainer(&Style.BackgroundImage, FName(TEXT("T_TestBrush")));
	}
	Style.BackgroundImage.TintColor = FSlateColor::UseForeground();
	Style.EnableFillAnimation = true;
	ProgressBarWidget->SetWidgetStyle(Style);
	RootPanel->AddChild(ProgressBarWidget);

	// 1. 对象模式导出：WidgetStyle 必须是 JSON 对象，含 FillImage。
	FString ObjectJson;
	FString Error;
	TestTrue(TEXT("对象模式导出成功"), FWidgetJsonSerializer::ExportToJson(Blueprint, ObjectJson, Error, true));
	TestTrue(TEXT("对象模式 JSON 含 structMode"), ObjectJson.Contains(TEXT("\"structMode\"")));
	TestTrue(TEXT("对象模式 JSON 含 object"), ObjectJson.Contains(TEXT("\"object\"")));

	TSharedPtr<FJsonObject> ObjectRoot;
	const TSharedRef<TJsonReader<TCHAR>> ObjectReader = TJsonReaderFactory<TCHAR>::Create(ObjectJson);
	TestTrue(TEXT("对象模式 JSON 可解析"), FJsonSerializer::Deserialize(ObjectReader, ObjectRoot) && ObjectRoot.IsValid());

	const TSharedPtr<FJsonObject>* ObjectRootNode = nullptr;
	if (ObjectRoot.IsValid() && ObjectRoot->TryGetObjectField(TEXT("root"), ObjectRootNode) && ObjectRootNode)
	{
		const TSharedPtr<FJsonValue> StyleValue = FindWidgetPropertyForTest(*ObjectRootNode, TEXT("Progress_Style"), TEXT("WidgetStyle"));
		TestTrue(TEXT("WidgetStyle 是 JSON 对象"), StyleValue.IsValid() && StyleValue->Type == EJson::Object);
		const TSharedPtr<FJsonObject>* StyleObj = nullptr;
		if (StyleValue.IsValid() && StyleValue->TryGetObject(StyleObj) && StyleObj)
		{
			TestTrue(TEXT("对象模式含 FillImage"), (*StyleObj)->HasField(TEXT("FillImage")));

			bool bEnableFillAnimation = false;
			TestTrue(TEXT("对象模式含 EnableFillAnimation"),
				(*StyleObj)->TryGetBoolField(TEXT("EnableFillAnimation"), bEnableFillAnimation));
			TestTrue(TEXT("EnableFillAnimation 值正确"), bEnableFillAnimation);

			const TSharedPtr<FJsonObject>* FillImageObj = nullptr;
			TestTrue(TEXT("FillImage 是对象"),
				(*StyleObj)->TryGetObjectField(TEXT("FillImage"), FillImageObj) && FillImageObj);
			if (FillImageObj)
			{
				FString DrawAsText;
				TestTrue(TEXT("FillImage.DrawAs 是字符串"),
					(*FillImageObj)->TryGetStringField(TEXT("DrawAs"), DrawAsText));
				TestEqual(TEXT("FillImage.DrawAs 值正确"), DrawAsText, TEXT("Box"));

				FString TilingText;
				TestTrue(TEXT("FillImage.Tiling 是字符串"),
					(*FillImageObj)->TryGetStringField(TEXT("Tiling"), TilingText));
				TestEqual(TEXT("FillImage.Tiling 值正确"), TilingText, TEXT("Both"));

				FString MirroringText;
				TestTrue(TEXT("FillImage.Mirroring 是字符串"),
					(*FillImageObj)->TryGetStringField(TEXT("Mirroring"), MirroringText));
				TestEqual(TEXT("FillImage.Mirroring 值正确"), MirroringText, TEXT("Horizontal"));

				FString ImageTypeText;
				TestTrue(TEXT("FillImage.ImageType 是字符串"),
					(*FillImageObj)->TryGetStringField(TEXT("ImageType"), ImageTypeText));
				TestEqual(TEXT("FillImage.ImageType 值正确"), ImageTypeText, TEXT("Linear"));

				const TSharedPtr<FJsonObject>* ImageSizeObj = nullptr;
				TestTrue(TEXT("FillImage.ImageSize 是对象"),
					(*FillImageObj)->TryGetObjectField(TEXT("ImageSize"), ImageSizeObj) && ImageSizeObj);
				if (ImageSizeObj)
				{
					double SizeX = 0.0;
					double SizeY = 0.0;
					TestTrue(TEXT("ImageSize.X 是数字"), (*ImageSizeObj)->TryGetNumberField(TEXT("X"), SizeX));
					TestTrue(TEXT("ImageSize.Y 是数字"), (*ImageSizeObj)->TryGetNumberField(TEXT("Y"), SizeY));
					TestEqual(TEXT("ImageSize.X 值正确"), SizeX, 24.0);
					TestEqual(TEXT("ImageSize.Y 值正确"), SizeY, 16.0);
				}

				const TSharedPtr<FJsonObject>* MarginObj = nullptr;
				TestTrue(TEXT("FillImage.Margin 是对象"),
					(*FillImageObj)->TryGetObjectField(TEXT("Margin"), MarginObj) && MarginObj);
				if (MarginObj)
				{
					double Left = 0.0;
					double Top = 0.0;
					double Right = 0.0;
					double Bottom = 0.0;
					(*MarginObj)->TryGetNumberField(TEXT("left"), Left);
					(*MarginObj)->TryGetNumberField(TEXT("top"), Top);
					(*MarginObj)->TryGetNumberField(TEXT("right"), Right);
					(*MarginObj)->TryGetNumberField(TEXT("bottom"), Bottom);
					TestEqual(TEXT("Margin.left 值正确"), Left, 1.0);
					TestEqual(TEXT("Margin.top 值正确"), Top, 2.0);
					TestEqual(TEXT("Margin.right 值正确"), Right, 3.0);
					TestEqual(TEXT("Margin.bottom 值正确"), Bottom, 4.0);
				}

				const TSharedPtr<FJsonObject>* OutlineObj = nullptr;
				TestTrue(TEXT("FillImage.OutlineSettings 是对象"),
					(*FillImageObj)->TryGetObjectField(TEXT("OutlineSettings"), OutlineObj) && OutlineObj);
				if (OutlineObj)
				{
					const TSharedPtr<FJsonObject>* CornerRadiiObj = nullptr;
					TestTrue(TEXT("OutlineSettings.CornerRadii 是对象"),
						(*OutlineObj)->TryGetObjectField(TEXT("CornerRadii"), CornerRadiiObj) && CornerRadiiObj);
					if (CornerRadiiObj)
					{
						double CornerX = 0.0;
						double CornerY = 0.0;
						double CornerZ = 0.0;
						double CornerW = 0.0;
						(*CornerRadiiObj)->TryGetNumberField(TEXT("X"), CornerX);
						(*CornerRadiiObj)->TryGetNumberField(TEXT("Y"), CornerY);
						(*CornerRadiiObj)->TryGetNumberField(TEXT("Z"), CornerZ);
						(*CornerRadiiObj)->TryGetNumberField(TEXT("W"), CornerW);
						TestEqual(TEXT("CornerRadii.X 值正确"), CornerX, 1.0);
						TestEqual(TEXT("CornerRadii.Y 值正确"), CornerY, 2.0);
						TestEqual(TEXT("CornerRadii.Z 值正确"), CornerZ, 3.0);
						TestEqual(TEXT("CornerRadii.W 值正确"), CornerW, 4.0);
					}
				}
			}

			const TSharedPtr<FJsonObject>* BackgroundImageObj = nullptr;
			TestTrue(TEXT("BackgroundImage 是对象"),
				(*StyleObj)->TryGetObjectField(TEXT("BackgroundImage"), BackgroundImageObj) && BackgroundImageObj);
			if (BackgroundImageObj)
			{
				FString ResourceObjectText;
				TestTrue(TEXT("BackgroundImage.ResourceObject 是字符串"),
					(*BackgroundImageObj)->TryGetStringField(TEXT("ResourceObject"), ResourceObjectText));
				TestEqual(TEXT("BackgroundImage.ResourceObject 为 None"), ResourceObjectText, TEXT("None"));

				FString ResourceNameText;
				TestTrue(TEXT("BackgroundImage.ResourceName 是字符串"),
					(*BackgroundImageObj)->TryGetStringField(TEXT("ResourceName"), ResourceNameText));
				TestEqual(TEXT("BackgroundImage.ResourceName 值正确"), ResourceNameText, TEXT("T_TestBrush"));
			}
		}
	}
	else
	{
		TestTrue(TEXT("对象模式 JSON 含 root"), false);
	}

	// 2. 对象模式导入：Percent 与嵌套 TintColor 必须保持。
	Error.Reset();
	TestTrue(TEXT("对象模式导入成功"), FWidgetJsonSerializer::ImportFromJson(Blueprint, ObjectJson, Error));
	UProgressBar* ImportedBar = Cast<UProgressBar>(Tree->FindWidget(TEXT("Progress_Style")));
	TestNotNull(TEXT("导入后 ProgressBar 存在"), ImportedBar);
	if (ImportedBar)
	{
		const FProgressBarStyle& ImportedStyle = ImportedBar->GetWidgetStyle();
		TestEqual(TEXT("Percent 往返"), ImportedBar->GetPercent(), 0.42f);
		TestTrue(TEXT("FillImage.TintColor 往返"),
			ImportedStyle.FillImage.TintColor.GetSpecifiedColor().Equals(FillTint));
		TestTrue(TEXT("BackgroundImage.TintColor/ColorUseRule 往返"),
			ImportedStyle.BackgroundImage.TintColor == FSlateColor::UseForeground());

		TestTrue(TEXT("FillImage.DrawAs 往返"),
			ImportedStyle.FillImage.DrawAs == ESlateBrushDrawType::Box);
		TestTrue(TEXT("FillImage.Tiling 往返"),
			ImportedStyle.FillImage.Tiling == ESlateBrushTileType::Both);
		TestTrue(TEXT("FillImage.Mirroring 往返"),
			ImportedStyle.FillImage.Mirroring == ESlateBrushMirrorType::Horizontal);
		TestTrue(TEXT("FillImage.ImageType 往返"),
			ImportedStyle.FillImage.ImageType == ESlateBrushImageType::Linear);
		TestEqual(TEXT("FillImage.ImageSize.X 往返"), ImportedStyle.FillImage.ImageSize.X, 24.0f);
		TestEqual(TEXT("FillImage.ImageSize.Y 往返"), ImportedStyle.FillImage.ImageSize.Y, 16.0f);
		TestEqual(TEXT("FillImage.Margin.Left 往返"), ImportedStyle.FillImage.Margin.Left, BrushMargin.Left);
		TestEqual(TEXT("FillImage.Margin.Top 往返"), ImportedStyle.FillImage.Margin.Top, BrushMargin.Top);
		TestEqual(TEXT("FillImage.Margin.Right 往返"), ImportedStyle.FillImage.Margin.Right, BrushMargin.Right);
		TestEqual(TEXT("FillImage.Margin.Bottom 往返"), ImportedStyle.FillImage.Margin.Bottom, BrushMargin.Bottom);

		TestEqual(TEXT("BackgroundImage.ResourceName 往返"),
			ImportedStyle.BackgroundImage.GetResourceName(), FName(TEXT("T_TestBrush")));
		TestTrue(TEXT("BackgroundImage.ResourceObject 为空"),
			ImportedStyle.BackgroundImage.GetResourceObject() == nullptr);

		TestEqual(TEXT("OutlineSettings.CornerRadii.X 往返"),
			ImportedStyle.FillImage.OutlineSettings.CornerRadii.X, CornerRadii.X);
		TestEqual(TEXT("OutlineSettings.CornerRadii.Y 往返"),
			ImportedStyle.FillImage.OutlineSettings.CornerRadii.Y, CornerRadii.Y);
		TestEqual(TEXT("OutlineSettings.CornerRadii.Z 往返"),
			ImportedStyle.FillImage.OutlineSettings.CornerRadii.Z, CornerRadii.Z);
		TestEqual(TEXT("OutlineSettings.CornerRadii.W 往返"),
			ImportedStyle.FillImage.OutlineSettings.CornerRadii.W, CornerRadii.W);
		TestTrue(TEXT("EnableFillAnimation 往返"), ImportedStyle.EnableFillAnimation);
	}

	// 2b. 顶层没有 structMode，但结构体值是对象：导入不依赖该字段，仍必须成功。
	TSharedPtr<FJsonObject> NoModeRoot;
	const TSharedRef<TJsonReader<TCHAR>> NoModeReader = TJsonReaderFactory<TCHAR>::Create(ObjectJson);
	if (FJsonSerializer::Deserialize(NoModeReader, NoModeRoot) && NoModeRoot.IsValid())
	{
		NoModeRoot->RemoveField(TEXT("structMode"));
	}
	FString NoModeJson;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> NoModeWriter =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&NoModeJson);
	TestTrue(TEXT("生成无 structMode 的对象 JSON"), NoModeRoot.IsValid());
	if (NoModeRoot.IsValid())
	{
		FJsonSerializer::Serialize(NoModeRoot.ToSharedRef(), NoModeWriter);
		FString NoModeError;
		TestTrue(TEXT("无 structMode 的对象 JSON 导入成功"),
			FWidgetJsonSerializer::ImportFromJson(Blueprint, NoModeJson, NoModeError));
		UProgressBar* NoModeBar = Cast<UProgressBar>(Tree->FindWidget(TEXT("Progress_Style")));
		TestNotNull(TEXT("无 structMode 导入后 ProgressBar 存在"), NoModeBar);
		if (NoModeBar)
		{
			TestEqual(TEXT("无 structMode 时 ImageSize.X 仍按对象导入"),
				NoModeBar->GetWidgetStyle().FillImage.ImageSize.X, 24.0f);
		}
	}

	// 2c. 导出入口按目标 JSON 的 structMode 选模式（覆盖 ExportAssetInternal 的读文件逻辑）。
	const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WidgetJsonBridge"), TEXT("Tests"));
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*TempDir);

	const FString ObjectModePath = FPaths::Combine(TempDir, TEXT("StructModeObject.json"));
	const FString ObjectModeFixture = TEXT("{\"schemaVersion\":1,\"structMode\":\"object\",\"root\":{}}");
	TestTrue(TEXT("写入 structMode=object 测试 JSON"),
		FFileHelper::SaveStringToFile(ObjectModeFixture, *ObjectModePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FString PathExportJson;
	FString PathExportError;
	TestTrue(TEXT("按文件 structMode=object 导出成功"),
		FWidgetJsonSync::ExportToJsonForPath(Blueprint, ObjectModePath, PathExportJson, PathExportError));

	TSharedPtr<FJsonObject> PathExportRoot;
	const TSharedRef<TJsonReader<TCHAR>> PathExportReader = TJsonReaderFactory<TCHAR>::Create(PathExportJson);
	TestTrue(TEXT("按文件模式导出的 JSON 可解析"),
		FJsonSerializer::Deserialize(PathExportReader, PathExportRoot) && PathExportRoot.IsValid());
	if (PathExportRoot.IsValid())
	{
		FString ModeText;
		TestTrue(TEXT("导出结果含 structMode 字符串"), PathExportRoot->TryGetStringField(TEXT("structMode"), ModeText));
		TestEqual(TEXT("导出结果为 object 模式"), ModeText, TEXT("object"));

		const TSharedPtr<FJsonObject>* PathRootNode = nullptr;
		if (PathExportRoot->TryGetObjectField(TEXT("root"), PathRootNode) && PathRootNode)
		{
			const TSharedPtr<FJsonValue> StyleValue = FindWidgetPropertyForTest(*PathRootNode, TEXT("Progress_Style"), TEXT("WidgetStyle"));
			TestTrue(TEXT("按文件模式导出时 WidgetStyle 是对象"),
				StyleValue.IsValid() && StyleValue->Type == EJson::Object);
		}
	}
	IFileManager::Get().Delete(*ObjectModePath, false, true, true);

	// 非法 structMode（这里是 bool）应打 Warning 并退回 string 模式，不能把 bool 写回去。
	const FString InvalidModePath = FPaths::Combine(TempDir, TEXT("StructModeInvalid.json"));
	const FString InvalidModeFixture = TEXT("{\"schemaVersion\":1,\"structMode\":true,\"root\":{}}");
	TestTrue(TEXT("写入非法 structMode 测试 JSON"),
		FFileHelper::SaveStringToFile(InvalidModeFixture, *InvalidModePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FString InvalidExportJson;
	FString InvalidExportError;
	TestTrue(TEXT("按非法 structMode 仍能导出"),
		FWidgetJsonSync::ExportToJsonForPath(Blueprint, InvalidModePath, InvalidExportJson, InvalidExportError));
	TestFalse(TEXT("非法 structMode 不会写回 JSON"), InvalidExportJson.Contains(TEXT("\"structMode\"")));

	TSharedPtr<FJsonObject> InvalidExportRoot;
	const TSharedRef<TJsonReader<TCHAR>> InvalidExportReader = TJsonReaderFactory<TCHAR>::Create(InvalidExportJson);
	if (FJsonSerializer::Deserialize(InvalidExportReader, InvalidExportRoot) && InvalidExportRoot.IsValid())
	{
		const TSharedPtr<FJsonObject>* InvalidRootNode = nullptr;
		if (InvalidExportRoot->TryGetObjectField(TEXT("root"), InvalidRootNode) && InvalidRootNode)
		{
			const TSharedPtr<FJsonValue> StyleValue = FindWidgetPropertyForTest(*InvalidRootNode, TEXT("Progress_Style"), TEXT("WidgetStyle"));
			TestTrue(TEXT("非法 structMode 时 WidgetStyle 退回字符串"),
				StyleValue.IsValid() && StyleValue->Type == EJson::String);
		}
	}
	IFileManager::Get().Delete(*InvalidModePath, false, true, true);

	// 3. 旧模式导出：不写 structMode，WidgetStyle 仍是引擎文本字符串；再导入不崩。
	FString StringJson;
	TestTrue(TEXT("字符串模式导出成功"), FWidgetJsonSerializer::ExportToJson(Blueprint, StringJson, Error, false));
	TestFalse(TEXT("字符串模式不写 structMode"), StringJson.Contains(TEXT("\"structMode\"")));

	TSharedPtr<FJsonObject> StringRoot;
	const TSharedRef<TJsonReader<TCHAR>> StringReader = TJsonReaderFactory<TCHAR>::Create(StringJson);
	TestTrue(TEXT("字符串模式 JSON 可解析"), FJsonSerializer::Deserialize(StringReader, StringRoot) && StringRoot.IsValid());
	const TSharedPtr<FJsonObject>* StringRootNode = nullptr;
	if (StringRoot.IsValid() && StringRoot->TryGetObjectField(TEXT("root"), StringRootNode) && StringRootNode)
	{
		const TSharedPtr<FJsonValue> StyleValue = FindWidgetPropertyForTest(*StringRootNode, TEXT("Progress_Style"), TEXT("WidgetStyle"));
		TestTrue(TEXT("字符串模式 WidgetStyle 是字符串"), StyleValue.IsValid() && StyleValue->Type == EJson::String);
	}

	Error.Reset();
	TestTrue(TEXT("字符串模式导入成功"), FWidgetJsonSerializer::ImportFromJson(Blueprint, StringJson, Error));
	UProgressBar* LegacyBar = Cast<UProgressBar>(Tree->FindWidget(TEXT("Progress_Style")));
	TestNotNull(TEXT("字符串模式导入后 ProgressBar 存在"), LegacyBar);
	if (LegacyBar)
	{
		TestEqual(TEXT("字符串模式 Percent 往返"), LegacyBar->GetPercent(), 0.42f);
	}

	// 4. 结构体对象里的非法字段名必须报错，不能静默忽略。
	const FString BadJson = TEXT("{\"schemaVersion\":1,\"root\":{\"class\":\"/Script/UMG.CanvasPanel\",\"name\":\"Root_Bad\",\"children\":[{\"class\":\"/Script/UMG.ProgressBar\",\"name\":\"Progress_Bad\",\"properties\":{\"WidgetStyle\":{\"NotAField\":1}}}]}}");
	UWidgetBlueprint* BadBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
	FString BadError;
	TestFalse(TEXT("非法结构体字段名导入失败"), FWidgetJsonSerializer::ImportFromJson(BadBlueprint, BadJson, BadError));
	TestTrue(TEXT("错误信息点名 NotAField"), BadError.Contains(TEXT("NotAField")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeHashLoopTest, "WidgetJsonBridge.HashNoFalseLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeHashLoopTest::RunTest(const FString& Parameters)
{
	const FString JsonPath = FPaths::Combine(FWidgetJsonSync::GetSyncRootDir(), TEXT("Test"), TEXT("HashLoop.json"));
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(JsonPath));

	const FString JsonText = TEXT("{\"schemaVersion\":1,\"root\":{\"class\":\"/Script/UMG.CanvasPanel\",\"name\":\"CanvasPanel_0\",\"children\":[{\"class\":\"/Script/UMG.TextBlock\",\"name\":\"Text_CN\",\"properties\":{\"Text\":\"开始游戏\"}}]}}");
	TestTrue(TEXT("写入测试 JSON"), FFileHelper::SaveStringToFile(JsonText, *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FWidgetJsonSync& Sync = FWidgetJsonSync::Get();
	Sync.ImportAll();

	UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, TEXT("/Game/Test/HashLoop"));
	TestNotNull(TEXT("资产已由 JSON 创建"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	const int32 ImportsBeforeExport = Sync.GetImportCount();

	// 走编辑器 Ctrl+S 的导出路径（bAllowConflictFile = true），
	// 这样才能真正覆盖「导出后不误判冲突 / 不误判文件改动」这两条。
	Sync.OnAssetSaved(Blueprint);

	const FString ConflictPath = FPaths::Combine(FPaths::GetPath(JsonPath), TEXT("HashLoop_editor_conflict.json"));
	TestFalse(TEXT("没有产生冲突文件"), IFileManager::Get().FileExists(*ConflictPath));

	// 关键断言：导出侧与 ticker 侧的 hash 若不在同一编码域（例如误用 HashAnsiString），
	// 这里会重新导入一遍。中文内容才能暴露这个问题，纯 ASCII 测不出来。
	Sync.Tick();
	Sync.Tick();
	TestEqual(TEXT("导出后没有发生假回环重导入"), Sync.GetImportCount(), ImportsBeforeExport);

	IFileManager::Get().Delete(*ConflictPath, false, true, true);
	return true;
}
