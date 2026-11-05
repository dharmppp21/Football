// Fill out your copyright notice in the Description page of Project Settings.

using System.IO;
using UnrealBuildTool;

public class FootballArena : ModuleRules
{
	public FootballArena(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "GameplayTags" });

		PrivateDependencyModuleNames.AddRange(new string[] { "AssetRegistry", "Chaos", "PhysicsCore" });

		// MuJoCo, for ARLPhysicsPlayer: from the release zip of the version RL_Train trains on, put bin/mujoco.dll, lib/mujoco.lib
		// and include/mujoco in ThirdParty/mujoco. Only that include folder: the zip's include/math hides UE's Math/ headers on
		// Windows. Delay-loaded, so the game runs without the DLL until one of those actors plays.
		string MuJoCo = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "mujoco");
		PrivateIncludePaths.Add(Path.Combine(MuJoCo, "include"));
		PublicAdditionalLibraries.Add(Path.Combine(MuJoCo, "lib", "mujoco.lib"));
		PublicDelayLoadDLLs.Add("mujoco.dll");
		RuntimeDependencies.Add("$(BinaryOutputDir)/mujoco.dll", Path.Combine(MuJoCo, "bin", "mujoco.dll"));

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
