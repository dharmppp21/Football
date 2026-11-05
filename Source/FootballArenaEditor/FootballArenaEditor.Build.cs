using UnrealBuildTool;

// Editor-only tooling for the football project (called from Python / the MCP bridge).
public class FootballArenaEditor : ModuleRules
{
	public FootballArenaEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });

		PrivateDependencyModuleNames.AddRange(new string[] {
			"UnrealEd", "Kismet", "BlueprintGraph", "AnimGraph", "AnimGraphRuntime", "Json", "AssetTools", "AssetRegistry", "IKRig", "IKRigDeveloper"
		});
	}
}
