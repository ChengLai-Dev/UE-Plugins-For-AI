#include "Misc/AutomationTest.h"
#include "WidgetJsonSerializer.h"
#include "WidgetJsonSync.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "UObject/UnrealType.h"
#include "Misc/FileHelper.h"

static UPanelSlot* GetWidgetSlotForTest(UWidget* Widget)
{
	FProperty* SlotProperty = Widget->GetClass()->FindPropertyByName(TEXT("Slot"));
	if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(SlotProperty))
	{
		return Cast<UPanelSlot>(ObjectProperty->GetObjectPropertyValue(ObjectProperty->ContainerPtrToValuePtr<void>(Widget)));
	}
	return nullptr;
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
	if (ImportedText)
	{
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeExportTest, "WidgetJsonBridge.ExportToFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetJsonBridgeExportTest::RunTest(const FString& Parameters)
{
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
	TestTrue(TEXT("导出文本与磁盘一致"), FWidgetJsonSerializer::ExportToJson(Blueprint, ExportText, ExportError));
	TestEqual(TEXT("磁盘内容与导出内容一致"), DiskText, ExportText);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetJsonBridgeMappingTest, "WidgetJsonBridge.MappingAndHash", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)bool FWidgetJsonBridgeMappingTest::RunTest(const FString& Parameters)
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
