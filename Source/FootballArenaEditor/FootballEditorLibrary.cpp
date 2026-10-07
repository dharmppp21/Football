#include "FootballEditorLibrary.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_Slot.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AnimationSettings.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/MirrorDataTable.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BonePose.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/MemStack.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

static FString ToJson(const TSharedRef<FJsonObject>& Obj) {
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

static FString Fail(const FString& Message) {
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), false);
	Obj->SetStringField(TEXT("message"), Message);
	return ToJson(Obj);
}

// First visible pin of a direction, optionally with a given name.
static UEdGraphPin* FindVisiblePin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TCHAR* Name = nullptr) {
	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin && !Pin->bHidden && Pin->Direction == Direction && (!Name || Pin->PinName == FName(Name))) {
			return Pin;
		}
	}
	return nullptr;
}

FString UFootballEditorLibrary::InsertAnimGraphPoseNode(UAnimBlueprint* AnimBP, const FString& NodeKind, const FString& Param) {
	if (!AnimBP) {
		return Fail(TEXT("Invalid AnimBlueprint."));
	}
	if (NodeKind != TEXT("Slot")) {
		return Fail(FString::Printf(TEXT("Unsupported NodeKind '%s' (supported: Slot)."), *NodeKind));
	}

	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs) {
		if (Graph && Graph->GetName() == TEXT("AnimGraph")) {
			AnimGraph = Graph;
			break;
		}
	}
	if (!AnimGraph) {
		return Fail(TEXT("AnimGraph not found."));
	}

	UAnimGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : AnimGraph->Nodes) {
		if ((Root = Cast<UAnimGraphNode_Root>(Node)) != nullptr) {
			break;
		}
	}
	UEdGraphPin* RootIn = Root ? FindVisiblePin(Root, EGPD_Input, TEXT("Result")) : nullptr;
	if (!RootIn) {
		return Fail(TEXT("Output Pose node not found."));
	}

	FGraphNodeCreator<UAnimGraphNode_Slot> Creator(*AnimGraph);
	UAnimGraphNode_Slot* SlotNode = Creator.CreateNode(false);
	SlotNode->Node.SlotName = Param.IsEmpty() ? FName(TEXT("DefaultSlot")) : FName(*Param);
	Creator.Finalize();

	UEdGraphPin* NewIn = FindVisiblePin(SlotNode, EGPD_Input, TEXT("Source"));
	UEdGraphPin* NewOut = FindVisiblePin(SlotNode, EGPD_Output);
	if (!NewIn || !NewOut) {
		AnimGraph->RemoveNode(SlotNode);
		return Fail(TEXT("The new node has no Source/Pose pins."));
	}

	// Take the Output Pose's spot and push the Output Pose to the right.
	SlotNode->NodePosX = Root->NodePosX;
	SlotNode->NodePosY = Root->NodePosY;
	Root->NodePosX += 300;

	UEdGraphPin* Feeder = RootIn->LinkedTo.Num() > 0 ? RootIn->LinkedTo[0] : nullptr;
	RootIn->BreakAllPinLinks();
	if (Feeder) {
		Feeder->MakeLinkTo(NewIn);
	}
	NewOut->MakeLinkTo(RootIn);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetBoolField(TEXT("compiled"), AnimBP->Status != BS_Error);
	Obj->SetStringField(TEXT("node_name"), SlotNode->GetName());
	Obj->SetStringField(TEXT("fed_by"), Feeder ? Feeder->GetOwningNode()->GetName() : FString());
	return ToJson(Obj);
}

bool UFootballEditorLibrary::RemoveUnusedBlueprintVariable(UBlueprint* Blueprint, FName VariableName) {
	if (!Blueprint || FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) == INDEX_NONE) {
		return false;
	}
	if (FBlueprintEditorUtils::IsVariableUsed(Blueprint, VariableName)) {
		return false;
	}
	FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, VariableName);
	return true;
}

FString UFootballEditorLibrary::SetMontageSections(UAnimMontage* Montage, const TArray<FName>& Names, const TArray<float>& StartTimes) {
	if (!Montage) {
		return Fail(TEXT("Invalid montage."));
	}
	if (Names.Num() == 0 || Names.Num() != StartTimes.Num()) {
		return Fail(TEXT("Names and StartTimes need the same, non-zero length."));
	}
	if (!FMath::IsNearlyZero(StartTimes[0])) {
		return Fail(TEXT("The first section has to start at 0."));
	}
	for (int32 i = 0; i < Names.Num(); ++i) {
		if (Names[i].IsNone() || Names.Find(Names[i]) != i) {
			return Fail(FString::Printf(TEXT("Section name %d is empty or repeated."), i));
		}
		if (i > 0 && (StartTimes[i] <= StartTimes[i - 1] || StartTimes[i] > Montage->GetPlayLength())) {
			return Fail(FString::Printf(TEXT("Section %s starts at %.3f s: start times must increase and stay within %.3f s."),
				*Names[i].ToString(), StartTimes[i], Montage->GetPlayLength()));
		}
	}
	Montage->Modify();
	Montage->CompositeSections.Reset();
	for (int32 i = 0; i < Names.Num(); ++i) {
		const int32 Index = Montage->AddAnimCompositeSection(Names[i], StartTimes[i]);
		Montage->CompositeSections[Index].NextSectionName = i + 1 < Names.Num() ? Names[i + 1] : NAME_None;
	}
	Montage->PostEditChange();
	Montage->MarkPackageDirty();

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetNumberField(TEXT("sections"), Montage->CompositeSections.Num());
	return ToJson(Obj);
}

FString UFootballEditorLibrary::BakeMirroredSequence(UAnimSequence* Source, UMirrorDataTable* MirrorTable, const FString& DestAssetPath) {
	if (!Source || !MirrorTable) {
		return Fail(TEXT("Invalid source sequence or mirror table."));
	}
	USkeleton* Skeleton = Source->GetSkeleton();
	if (!Skeleton || MirrorTable->Skeleton != Skeleton) {
		return Fail(TEXT("The mirror table has to be made for the sequence's skeleton."));
	}
	if (!FPackageName::IsValidLongPackageName(DestAssetPath) || FPackageName::DoesPackageExist(DestAssetPath) || FindPackage(nullptr, *DestAssetPath)) {
		return Fail(FString::Printf(TEXT("%s is not a free asset path."), *DestAssetPath));
	}

	// Every key of Source, mirrored (FAnimationRuntime::MirrorPose, as the AnimGraph's Mirror node does).
	const IAnimationDataModel* Model = Source->GetDataModel();
	const int32 NumKeys = Model->GetNumberOfKeys();
	const FFrameRate Rate = Model->GetFrameRate();
	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();
	TArray<FBoneIndexType> BoneIndices;
	for (int32 i = 0; i < NumBones; ++i) {
		BoneIndices.Add(FBoneIndexType(i));
	}
	FBoneContainer BoneContainer(BoneIndices, UE::Anim::FCurveFilterSettings(), *Skeleton);
	TArray<TArray<FVector>> Positions, Scales;
	TArray<TArray<FQuat>> Rotations;
	Positions.SetNum(NumBones);
	Rotations.SetNum(NumBones);
	Scales.SetNum(NumBones);
	for (int32 Key = 0; Key < NumKeys; ++Key) {
		FMemMark Mark(FMemStack::Get());
		FCompactPose Pose;
		Pose.SetBoneContainer(&BoneContainer);
		FBlendedCurve Curve;
		Curve.InitFrom(BoneContainer);
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData PoseData(Pose, Curve, Attributes);
		const double Time = FMath::Min(Rate.AsSeconds(FFrameTime(Key)), double(Source->GetPlayLength()));
		Source->GetAnimationPose(PoseData, FAnimExtractContext(Time));
		FAnimationRuntime::MirrorPose(Pose, *MirrorTable);
		for (const FCompactPoseBoneIndex Index : Pose.ForEachBoneIndex()) {
			const int32 Bone = BoneContainer.GetSkeletonIndex(Index);
			Positions[Bone].Add(Pose[Index].GetLocation());
			Rotations[Bone].Add(Pose[Index].GetRotation().GetNormalized());
			Scales[Bone].Add(Pose[Index].GetScale3D());
		}
	}

	// The copy keeps Source's curves (the root travel), notifies and settings; only the bone keys change.
	UAnimSequence* Dest = Cast<UAnimSequence>(FAssetToolsModule::GetModule().Get().DuplicateAsset(
		FPackageName::GetLongPackageAssetName(DestAssetPath), FPackageName::GetLongPackagePath(DestAssetPath), Source));
	if (!Dest) {
		return Fail(TEXT("Could not duplicate the source sequence."));
	}
	IAnimationDataController& Controller = Dest->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Bake mirrored sequence")));
	int32 Written = 0;
	for (int32 Bone = 0; Bone < NumBones; ++Bone) {
		if (Positions[Bone].Num() != NumKeys) {
			continue;
		}
		const FName Name = RefSkeleton.GetBoneName(Bone);
		if (!Dest->GetDataModel()->IsValidBoneTrackName(Name)) {
			Controller.AddBoneCurve(Name);
		}
		Written += Controller.SetBoneTrackKeys(Name, Positions[Bone], Rotations[Bone], Scales[Bone]) ? 1 : 0;
	}
	Controller.CloseBracket();
	Dest->MarkPackageDirty();

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetStringField(TEXT("path"), Dest->GetPathName());
	Obj->SetNumberField(TEXT("keys"), NumKeys);
	Obj->SetNumberField(TEXT("bones"), Written);
	return ToJson(Obj);
}

FString UFootballEditorLibrary::CreateMirrorDataTable(USkeleton* Skeleton, const FString& AssetPath) {
	if (!Skeleton) {
		return Fail(TEXT("Invalid skeleton."));
	}
	if (!FPackageName::IsValidLongPackageName(AssetPath) || FPackageName::DoesPackageExist(AssetPath) || FindPackage(nullptr, *AssetPath)) {
		return Fail(FString::Printf(TEXT("%s is not a free asset path."), *AssetPath));
	}
	UPackage* Package = CreatePackage(*AssetPath);
	UMirrorDataTable* Table = NewObject<UMirrorDataTable>(Package, FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
		RF_Public | RF_Standalone | RF_Transactional);
	// As UMirrorDataTableFactory::FactoryCreateNew, with the project's default name rules.
	Table->RowStruct = FMirrorTableRow::StaticStruct();
	Table->Skeleton = Skeleton;
	Table->MirrorFindReplaceExpressions = GetDefault<UAnimationSettings>()->MirrorFindReplaceExpressions;
	Table->UpdateFromFindReplaceExpressions(UMirrorDataTable::FFindReplaceOptions::Sync());
	FAssetRegistryModule::AssetCreated(Table);
	Table->MarkPackageDirty();

	int32 Pairs = 0;
	Table->ForeachRow<FMirrorTableRow>(TEXT("CreateMirrorDataTable"), [&Pairs](const FName&, const FMirrorTableRow& Row) {
		Pairs += Row.Name != Row.MirroredName ? 1 : 0;
	});
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetStringField(TEXT("path"), Table->GetPathName());
	Obj->SetNumberField(TEXT("rows"), Table->GetRowMap().Num());
	Obj->SetNumberField(TEXT("pairs"), Pairs);
	return ToJson(Obj);
}
