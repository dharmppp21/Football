#include "FootballEditorLibrary.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_RetargetPoseFromMesh.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimNodes/AnimNode_RetargetPoseFromMesh.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Self.h"
#include "Retargeter/IKRetargeter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AnimationSettings.h"
#include "Animation/BlendSpace.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/MirrorDataTable.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BonePose.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "K2Node_VariableGet.h"
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

FString UFootballEditorLibrary::SetSlotSourceBlendSpace(UAnimBlueprint* AnimBP, UBlendSpace* BlendSpace, FName XVariable, FName YVariable) {
	if (!AnimBP || !BlendSpace) {
		return Fail(TEXT("Invalid AnimBlueprint or BlendSpace."));
	}
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs) {
		if (Graph && Graph->GetName() == TEXT("AnimGraph")) {
			AnimGraph = Graph;
			break;
		}
	}
	UAnimGraphNode_Slot* SlotNode = nullptr;
	if (AnimGraph) {
		for (UEdGraphNode* Node : AnimGraph->Nodes) {
			if ((SlotNode = Cast<UAnimGraphNode_Slot>(Node)) != nullptr) {
				break;
			}
		}
	}
	UEdGraphPin* SlotIn = SlotNode ? FindVisiblePin(SlotNode, EGPD_Input, TEXT("Source")) : nullptr;
	if (!SlotIn) {
		return Fail(TEXT("No Slot node with a Source pin in the AnimGraph."));
	}
	// A scripted sample edit doesn't rebuild the blend space's grid; do it before the node reads it.
	RefreshBlendSpace(BlendSpace);

	FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> Creator(*AnimGraph);
	UAnimGraphNode_BlendSpacePlayer* Player = Creator.CreateNode(false);
	Player->SetAnimationAsset(BlendSpace);
	Creator.Finalize();
	Player->NodePosX = SlotNode->NodePosX - 400;
	Player->NodePosY = SlotNode->NodePosY;
	UEdGraphPin* PlayerOut = FindVisiblePin(Player, EGPD_Output);
	if (!PlayerOut) {
		AnimGraph->RemoveNode(Player);
		return Fail(TEXT("The Blend Space Player has no pose output."));
	}

	// Replace whatever fed the Slot (the old idle Sequence Player).
	FString Replaced;
	if (SlotIn->LinkedTo.Num() > 0) {
		UEdGraphNode* Old = SlotIn->LinkedTo[0]->GetOwningNode();
		Replaced = Old->GetName();
		SlotIn->BreakAllPinLinks();
		AnimGraph->RemoveNode(Old);
	}
	PlayerOut->MakeLinkTo(SlotIn);

	// The coordinates come from the AnimBP's member variables.
	const UEdGraphSchema* Schema = AnimGraph->GetSchema();
	const TPair<FName, const TCHAR*> Inputs[] = { {XVariable, TEXT("X")}, {YVariable, TEXT("Y")} };
	int32 Row = 0;
	for (const TPair<FName, const TCHAR*>& Input : Inputs) {
		FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*AnimGraph);
		UK2Node_VariableGet* Get = GetCreator.CreateNode(false);
		Get->VariableReference.SetSelfMember(Input.Key);
		GetCreator.Finalize();
		Get->NodePosX = Player->NodePosX - 250;
		Get->NodePosY = Player->NodePosY + 80 * Row++;
		UEdGraphPin* Value = Get->FindPin(Input.Key);
		UEdGraphPin* Coord = Player->FindPin(Input.Value);
		if (!Value || !Coord || !Schema->TryCreateConnection(Value, Coord)) {
			return Fail(FString::Printf(TEXT("Could not connect %s to the player's %s pin."), *Input.Key.ToString(), Input.Value));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetBoolField(TEXT("compiled"), AnimBP->Status != BS_Error);
	Obj->SetStringField(TEXT("node_name"), Player->GetName());
	Obj->SetStringField(TEXT("replaced"), Replaced);
	return ToJson(Obj);
}

FString UFootballEditorLibrary::RefreshBlendSpace(UBlendSpace* BlendSpace) {
	if (!BlendSpace) {
		return Fail(TEXT("No blend space."));
	}
	BlendSpace->ValidateSampleData();
	BlendSpace->ResampleData();
	BlendSpace->MarkPackageDirty();
	int32 Valid = 0;
	for (const FBlendSample& Sample : BlendSpace->GetBlendSamples()) {
		Valid += Sample.bIsValid ? 1 : 0;
	}
	// SampleIndexWithMarkers is protected; read it by reflection.
	const FIntProperty* MarkerSample = FindFProperty<FIntProperty>(UBlendSpace::StaticClass(), TEXT("SampleIndexWithMarkers"));
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetNumberField(TEXT("samples"), BlendSpace->GetBlendSamples().Num());
	Obj->SetNumberField(TEXT("valid"), Valid);
	Obj->SetNumberField(TEXT("marker_sample"), MarkerSample ? MarkerSample->GetPropertyValue_InContainer(BlendSpace) : -2);
	return ToJson(Obj);
}

FString UFootballEditorLibrary::ImplementInterfaceFunctionWithCall(UBlueprint* Blueprint, UClass* Interface, FName FunctionName, UClass* LibraryClass, FName LibraryFunction) {
	if (!Blueprint || !Interface || !LibraryClass) {
		return Fail(TEXT("Invalid Blueprint, interface or library class."));
	}
	UFunction* Function = LibraryClass->FindFunctionByName(LibraryFunction);
	if (!Function) {
		return Fail(FString::Printf(TEXT("%s has no function %s."), *LibraryClass->GetName(), *LibraryFunction.ToString()));
	}
	if (!FBlueprintEditorUtils::ImplementsInterface(Blueprint, false, Interface)
		&& !FBlueprintEditorUtils::ImplementNewInterface(Blueprint, Interface->GetClassPathName())) {
		return Fail(TEXT("Could not add the interface."));
	}
	UEdGraph* Graph = nullptr;
	for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces) {
		if (Description.Interface == Interface) {
			for (UEdGraph* Candidate : Description.Graphs) {
				if (Candidate && Candidate->GetFName() == FunctionName) {
					Graph = Candidate;
				}
			}
		}
	}
	if (!Graph) {
		return Fail(FString::Printf(TEXT("The interface has no function graph %s."), *FunctionName.ToString()));
	}
	UK2Node_FunctionEntry* Entry = nullptr;
	UK2Node_FunctionResult* Result = nullptr;
	TArray<UEdGraphNode*> Others;
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node)) {
			Entry = AsEntry;
		} else if (UK2Node_FunctionResult* AsResult = Cast<UK2Node_FunctionResult>(Node)) {
			Result = AsResult;
		} else if (Node) {
			Others.Add(Node);
		}
	}
	if (!Entry) {
		return Fail(TEXT("The function graph has no entry node."));
	}
	for (UEdGraphNode* Node : Others) { // an earlier body
		Graph->RemoveNode(Node);
	}
	if (!Result) {
		FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Graph);
		Result = ResultCreator.CreateNode();
		Result->FunctionReference = Entry->FunctionReference;
		ResultCreator.Finalize();
		Result->ReconstructNode();
	}
	Result->NodePosX = Entry->NodePosX + 700;
	Result->NodePosY = Entry->NodePosY;

	FGraphNodeCreator<UK2Node_CallFunction> CallCreator(*Graph);
	UK2Node_CallFunction* Call = CallCreator.CreateNode(false);
	Call->SetFromFunction(Function);
	CallCreator.Finalize();
	Call->NodePosX = Entry->NodePosX + 320;
	Call->NodePosY = Entry->NodePosY;
	FGraphNodeCreator<UK2Node_Self> SelfCreator(*Graph);
	UK2Node_Self* Self = SelfCreator.CreateNode(false);
	SelfCreator.Finalize();
	Self->NodePosX = Call->NodePosX - 180;
	Self->NodePosY = Call->NodePosY + 140;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* Output = nullptr; // the function's struct output
	for (UEdGraphPin* Pin : Result->Pins) {
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) {
			Output = Pin;
		}
	}
	UEdGraphPin* PawnPin = nullptr;
	UEdGraphPin* StructPin = nullptr; // the call's wildcard struct output
	for (UEdGraphPin* Pin : Call->Pins) {
		if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->PinName == UEdGraphSchema_K2::PN_Self) {
			continue;
		}
		if (Pin->Direction == EGPD_Input && !PawnPin) {
			PawnPin = Pin;
		} else if (Pin->Direction == EGPD_Output && !StructPin) {
			StructPin = Pin;
		}
	}
	UEdGraphPin* EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* SelfPin = Self->FindPin(UEdGraphSchema_K2::PN_Self);
	if (!Output || !StructPin || !PawnPin || !EntryThen || !SelfPin) {
		return Fail(TEXT("Missing a pin (the function's struct output, the call's pawn or struct pin, the entry's exec, self)."));
	}
	StructPin->PinType = Output->PinType; // the wildcard takes the interface's struct
	StructPin->PinType.bIsReference = false;
	const bool bWired = Schema->TryCreateConnection(EntryThen, Call->GetExecPin())
		&& Schema->TryCreateConnection(Call->GetThenPin(), Result->GetExecPin())
		&& Schema->TryCreateConnection(SelfPin, PawnPin)
		&& Schema->TryCreateConnection(StructPin, Output);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (!bWired) {
		return Fail(TEXT("Could not wire the call."));
	}
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	return ToJson(Obj);
}

FString UFootballEditorLibrary::SetSlotSourceRetargetBlend(UAnimBlueprint* AnimBP, UObject* Retargeter, FName SourceMeshVariable, FName AlphaVariable) {
	UIKRetargeter* RetargetAsset = Cast<UIKRetargeter>(Retargeter);
	if (!AnimBP || !RetargetAsset) {
		return Fail(TEXT("Invalid AnimBlueprint or IK Retargeter."));
	}
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs) {
		if (Graph && Graph->GetName() == TEXT("AnimGraph")) {
			AnimGraph = Graph;
			break;
		}
	}
	UAnimGraphNode_Slot* SlotNode = nullptr;
	if (AnimGraph) {
		for (UEdGraphNode* Node : AnimGraph->Nodes) {
			if ((SlotNode = Cast<UAnimGraphNode_Slot>(Node)) != nullptr) {
				break;
			}
		}
	}
	UEdGraphPin* SlotIn = SlotNode ? FindVisiblePin(SlotNode, EGPD_Input, TEXT("Source")) : nullptr;
	if (!SlotIn || SlotIn->LinkedTo.Num() == 0) {
		return Fail(TEXT("No Slot node fed by a pose in the AnimGraph."));
	}
	UEdGraphPin* BallPose = SlotIn->LinkedTo[0];
	if (Cast<UAnimGraphNode_TwoWayBlend>(BallPose->GetOwningNode())) {
		return Fail(TEXT("The Slot is already fed by a Two Way Blend (done before)."));
	}
	const UEdGraphSchema* Schema = AnimGraph->GetSchema();

	FGraphNodeCreator<UAnimGraphNode_RetargetPoseFromMesh> RetargetCreator(*AnimGraph);
	UAnimGraphNode_RetargetPoseFromMesh* Retarget = RetargetCreator.CreateNode(false);
	// The node's settings are a private member: set them by reflection.
	if (const FStructProperty* NodeProperty = FindFProperty<FStructProperty>(UAnimGraphNode_RetargetPoseFromMesh::StaticClass(), TEXT("Node"))) {
		FAnimNode_RetargetPoseFromMesh* Settings = NodeProperty->ContainerPtrToValuePtr<FAnimNode_RetargetPoseFromMesh>(Retarget);
		Settings->RetargetFrom = ERetargetSourceMode::CustomSkeletalMeshComponent;
		Settings->IKRetargeterAsset = RetargetAsset;
	}
	RetargetCreator.Finalize();
	Retarget->ReconstructNode();
	Retarget->NodePosX = SlotNode->NodePosX - 700;
	Retarget->NodePosY = SlotNode->NodePosY - 220;

	FGraphNodeCreator<UAnimGraphNode_TwoWayBlend> BlendCreator(*AnimGraph);
	UAnimGraphNode_TwoWayBlend* Blend = BlendCreator.CreateNode(false);
	BlendCreator.Finalize();
	Blend->NodePosX = SlotNode->NodePosX - 300;
	Blend->NodePosY = SlotNode->NodePosY;

	FString RetargetPins;
	for (UEdGraphPin* Pin : Retarget->Pins) {
		if (Pin && !Pin->bHidden) {
			RetargetPins += Pin->PinName.ToString() + TEXT(" ");
		}
	}
	UEdGraphPin* RetargetOut = FindVisiblePin(Retarget, EGPD_Output);
	UEdGraphPin* SourceMeshPin = Retarget->FindPin(TEXT("SourceMeshComponent"));
	UEdGraphPin* A = Blend->FindPin(TEXT("A"));
	UEdGraphPin* B = Blend->FindPin(TEXT("B"));
	UEdGraphPin* AlphaPin = Blend->FindPin(TEXT("Alpha"));
	UEdGraphPin* BlendOut = FindVisiblePin(Blend, EGPD_Output);
	if (!RetargetOut || !SourceMeshPin || !A || !B || !AlphaPin || !BlendOut) {
		return Fail(FString::Printf(TEXT("Missing a pin (retarget pins: %s)."), *RetargetPins));
	}
	auto MakeGet = [&](FName Variable, int32 X, int32 Y) {
		FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*AnimGraph);
		UK2Node_VariableGet* Get = GetCreator.CreateNode(false);
		Get->VariableReference.SetSelfMember(Variable);
		GetCreator.Finalize();
		Get->NodePosX = X;
		Get->NodePosY = Y;
		return Get->FindPin(Variable);
	};
	UEdGraphPin* SourceMesh = MakeGet(SourceMeshVariable, Retarget->NodePosX - 300, Retarget->NodePosY + 60);
	UEdGraphPin* Alpha = MakeGet(AlphaVariable, Blend->NodePosX - 250, Blend->NodePosY + 160);
	SlotIn->BreakAllPinLinks();
	const bool bWired = SourceMesh && Alpha && Schema->TryCreateConnection(SourceMesh, SourceMeshPin) && Schema->TryCreateConnection(RetargetOut, A)
		&& Schema->TryCreateConnection(BallPose, B) && Schema->TryCreateConnection(Alpha, AlphaPin) && Schema->TryCreateConnection(BlendOut, SlotIn);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	FKismetEditorUtilities::CompileBlueprint(AnimBP);
	if (!bWired) {
		return Fail(FString::Printf(TEXT("Could not wire the nodes (retarget pins: %s)."), *RetargetPins));
	}
	const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("success"), true);
	Obj->SetBoolField(TEXT("compiled"), AnimBP->Status != BS_Error);
	Obj->SetStringField(TEXT("retarget_pins"), RetargetPins);
	return ToJson(Obj);
}
