using UnrealBuildTool;

public class WidgetJsonBridgeEditor : ModuleRules
{
	public WidgetJsonBridgeEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"UnrealEd",
			"UMG",
			"UMGEditor",
			"Json",
			"AssetTools",
			"ToolMenus",
			"Projects",
		});
	}
}
