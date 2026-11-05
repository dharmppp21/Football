#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballEditorLibrary.generated.h"

class UAnimBlueprint;
class UAnimMontage;
class UAnimSequence;
class UBlendSpace;
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

	// Feeds the AnimGraph's first Slot from a Blend Space Player playing BlendSpace with X <- XVariable and Y <- YVariable
	// (member variables of the AnimBP's class), replacing the node that fed the Slot, rebuilds the blend space's sample grid
	// and compiles. Returns JSON {success, compiled, node_name, replaced} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString SetSlotSourceBlendSpace(UAnimBlueprint* AnimBP, UBlendSpace* BlendSpace, FName XVariable, FName YVariable);

	// What the blend space editor does after a sample edit, which a scripted sample_data set skips: validates the samples
	// (this also picks the sample whose sync markers drive marker-based sync) and rebuilds the grid. Save the asset afterwards.
	// Returns JSON {success, samples, valid, marker_sample} (marker_sample -1 = no marker-based sync) or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString RefreshBlendSpace(UBlendSpace* BlendSpace);

	// Makes Blueprint implement Interface (if it doesn't) and gives the interface's function FunctionName one body: a call of
	// LibraryClass::LibraryFunction(self, <its struct output>) feeding the function's struct output - e.g. the Game Animation
	// Sample's BPI_SandboxCharacter_Pawn::Get_PropertiesForAnimation <- UFootballAnimPropertiesLibrary::FillSandboxAnimationProperties.
	// Replaces any earlier body, compiles. Returns JSON {success, compiled} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString ImplementInterfaceFunctionWithCall(UBlueprint* Blueprint, UClass* Interface, FName FunctionName, UClass* LibraryClass, FName LibraryFunction);

	// Feeds the AnimGraph's first Slot from a Two Way Blend: A = Retarget Pose From Mesh (Retargeter, source mesh = the member
	// variable SourceMeshVariable), B = whatever fed the Slot until now (the blend space player), Alpha = AlphaVariable. Compiles.
	// Returns JSON {success, compiled, retarget_pins} or {success:false, message}.
	UFUNCTION(BlueprintCallable, Category = "Football|Editor")
	static FString SetSlotSourceRetargetBlend(UAnimBlueprint* AnimBP, UObject* Retargeter, FName SourceMeshVariable, FName AlphaVariable);
};
