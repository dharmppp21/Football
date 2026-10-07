#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballEditorLibrary.generated.h"

class UAnimBlueprint;
class UAnimMontage;
class UAnimSequence;
class UBlueprint;
class UMirrorDataTable;
class USkeleton;

// Editor tools for building the football framework's assets from Python:
// unreal.FootballEditorLibrary.<snake_case_name>(...)
UCLASS()
class UFootballEditorLibrary : public UBlueprintFunctionLibrary {
	GENERATED_BODY()
public:
	// Inserts a pose node directly in front of the AnimGraph's Output Pose and compiles.
	// NodeKind "Slot": Param = slot name (empty = DefaultSlot). Returns JSON {success, compiled, node_name, fed_by} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString InsertAnimGraphPoseNode(UAnimBlueprint* AnimBP, const FString& NodeKind, const FString& Param);

	// Removes a Blueprint member variable, but only if no node uses it any more. Returns true if it was removed.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static bool RemoveUnusedBlueprintVariable(UBlueprint* Blueprint, FName VariableName);

	// Replaces a montage's sections (doc §9): Names[i] starts at StartTimes[i] seconds and plays into the next one;
	// the first starts at 0. Returns JSON {success, sections} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString SetMontageSections(UAnimMontage* Montage, const TArray<FName>& Names, const TArray<float>& StartTimes);

	// Bakes the left/right mirror of Source into a new sequence at DestAssetPath (e.g. "/Game/.../AS_elastico_Mirrored"):
	// a copy of Source, curves and notifies included, whose bone keys are Source's poses mirrored with MirrorTable.
	// Never overwrites (fails if DestAssetPath exists); save the new asset afterwards.
	// Returns JSON {success, path, keys, bones} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString BakeMirroredSequence(UAnimSequence* Source, UMirrorDataTable* MirrorTable, const FString& DestAssetPath);

	// Creates a mirror data table for Skeleton at AssetPath (e.g. "/Game/Football/Animations/MDT_Footballer"), filled from
	// the project's mirror name rules (Left* <-> Right* ...), mirroring across X (the mesh's left/right axis). It is what the
	// editor's Mirror Data Table factory does, without its skeleton dialog. Never overwrites; save the asset afterwards.
	// Returns JSON {success, path, rows, pairs} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString CreateMirrorDataTable(USkeleton* Skeleton, const FString& AssetPath);
};
