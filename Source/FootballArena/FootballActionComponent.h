#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "GameplayTagContainer.h"
#include "FootballTypes.h"
#include "FootballActionComponent.generated.h"

class UFootballActionData;
class UFootballSkillComponent;
class UFootballSkillData;
class UPlayerAttributesData;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnActionStateChanged, FGameplayTag, ActionTag, EActionState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnActionRejected, FGameplayTag, ActionTag, EActionValidation, Reason);

// The action layer (master spec §116-121; master plan Phase 5): every gameplay action is asked for here, the same way.
//   request (§117) -> context (§118) -> validate (§119, with prediction + reachability §72-73) -> align (§120) -> start
// - The player is moved to the real ball (§15): CalculateAlignmentTarget works out a bounded turn at the start and a
//   bounded capsule move over a warp window, shaped like a Motion Warping target (§16). Actions without bAlignPlayer get
//   no correction (the frozen Elastico still steers the ball until master plan Phase 6).
// - A request that can't start yet waits in a one-slot input buffer (§63) for its InputBufferTime; a newer request replaces
//   it. If the ball can't be reached when it runs out, the action's fallback runs (§121; "keep dribbling" needs no asset).
// - Priority is data (§64): a higher-priority request takes over a running action that allows it.
// - Interruptions (§65) release the ball's control, restore movement and let the ball carry on physically.
// - Skill-category actions are played by UFootballSkillComponent (the executor); other categories come with their phases.
// fb.Debug.Action draws the running action, its state and its alignment; GetLastActionRecord is the §104 recorder.
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UFootballActionComponent : public UActorComponent {
	GENERATED_BODY()
public:
	UFootballActionComponent();

	// Every action asset in this folder (and below) is loaded at BeginPlay; RequestAction finds them by tag.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action", meta = (ContentDir))
	FDirectoryPath ActionDirectory;

	// The player's attributes (§62) and strong foot, copied into every request's context.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Action")
	TSoftObjectPtr<UPlayerAttributesData> AttributesAsset = TSoftObjectPtr<UPlayerAttributesData>(FSoftObjectPath(TEXT("/Game/Football/Data/DA_PlayerAttributes_Default.DA_PlayerAttributes_Default")));

	// §104, for the relevant player only: the recorder runs for a locally controlled player, or for any with this on.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action|Debug")
	bool bAlwaysRecord = false;

	// How long the recorder keeps going after an action ends.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Action|Debug")
	float RecordTail = 0.4f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Action")
	TArray<TObjectPtr<UFootballActionData>> LoadedActions;

	UPROPERTY(BlueprintAssignable, Category = "Action")
	FOnActionStateChanged OnActionStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Action")
	FOnActionRejected OnActionRejected;

	// Spec §117: asks for the loaded action tagged ActionTag. True = it started now. False = it was refused (the reason is
	// logged and broadcast); a request that may still become valid waits in the input buffer.
	UFUNCTION(BlueprintCallable, Category = "Action")
	bool RequestAction(FGameplayTag ActionTag, FActionContext Context);

	// RequestAction with the action asset itself (tests, data-driven callers).
	UFUNCTION(BlueprintCallable, Category = "Action")
	bool RequestActionData(UFootballActionData* Action, FActionContext Context);

	// RequestAction for an input gesture: the context from the ball, the input direction and the foot (mirrored = left).
	UFUNCTION(BlueprintCallable, Category = "Action")
	bool RequestActionWithInput(FGameplayTag ActionTag, AActor* Ball, FVector InputDirection, bool bMirrored);

	// Spec §65: ends the running action early if its InterruptionProfile allows Reason (Cancelled and InvalidState always
	// end it). True = it ended.
	UFUNCTION(BlueprintCallable, Category = "Action")
	bool InterruptAction(EActionInterruptReason Reason);

	UFUNCTION(BlueprintPure, Category = "Action")
	bool IsActionActive() const { return ActiveAction != nullptr; }

	UFUNCTION(BlueprintPure, Category = "Action")
	UFootballActionData* GetActiveAction() const { return ActiveAction; }

	UFUNCTION(BlueprintPure, Category = "Action")
	EActionState GetActionState() const { return ActionState; }

	// The running action's current alignment target (the first contact's, or the next one's after a re-alignment).
	UFUNCTION(BlueprintPure, Category = "Action")
	FActionAlignmentTarget GetAlignmentTarget() const;

	UFUNCTION(BlueprintPure, Category = "Action")
	bool HasPendingRequest() const { return bPending; }

	// The last refusal and the last interruption (tests, debugging).
	UFUNCTION(BlueprintPure, Category = "Action")
	EActionValidation GetLastRejection() const { return LastRejection; }

	UFUNCTION(BlueprintPure, Category = "Action")
	EActionInterruptReason GetLastInterruptReason() const { return LastInterruptReason; }

	// Every tick of the last action plus RecordTail seconds after it (§104).
	UFUNCTION(BlueprintCallable, Category = "Action|Debug")
	TArray<FActionRecordSample> GetLastActionRecord() const { return Record; }

	// The player's attributes asset (§62): its attributes and the skill -> assist curve (§105), for the executors.
	UFUNCTION(BlueprintPure, Category = "Action")
	UPlayerAttributesData* GetAttributes() const { return LoadedAttributes; }

	// ---- the rules: stateless, so the self-test runs them without a world ----

	// §119: the first validator that refuses, in order: state, cooldown, foot, ball, ownership, distance, direction, speed,
	// reachability, collision space. Ball checks only for an action with contacts.
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static EActionValidation ValidateAction(const UFootballActionData* Action, const FActionContext& Context, const FActionValidationInput& Input);

	// §120: a bounded turn at the start (swinging the whole move about the pivot so the expected ball spot points at the
	// ball), then a bounded move for the rest, both clamped by the profile; reachable when what is left is within the
	// contact's tolerance.
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static FActionAlignmentTarget CalculateAlignmentTarget(const FAlignmentQuery& Query);

	// Skill space from the action's DirectionMode. bOutHasDirection false = the mode found no direction (no input, target...).
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static FSkillBasis MakeActionBasis(const UFootballActionData* Action, const FActionContext& Context, ESkillDirection& OutDirection, bool& bOutHasDirection);

	// §63: a refused request keeps waiting while it is younger than BufferTime and its reason may still clear.
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static bool ShouldBuffer(EActionValidation Reason, float Age, float BufferTime);

	// §121: the reasons a fallback runs for (the ball can't be reached).
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static bool WantsFallback(EActionValidation Reason);

	// §65: Reason ends an action with this profile.
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static bool AllowsInterrupt(const FActionInterruptionProfile& Profile, EActionInterruptReason Reason);

	// §64: a request of IncomingPriority takes over a running action of ActivePriority.
	UFUNCTION(BlueprintPure, Category = "Football|Action")
	static bool Supersedes(int32 IncomingPriority, int32 ActivePriority, bool bActiveProtected);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	UFootballActionData* FindAction(FGameplayTag ActionTag) const;
	void FillContext(FActionContext& Context) const;
	// Validates Action in Context and starts it. False = refused (OutReason).
	bool TryStart(UFootballActionData* Action, FActionContext& Context, EActionValidation& OutReason, bool bLog);
	// ValidateCollisionSpace: the capsule can travel Travel (the action's own travel + the alignment) without a blocking hit.
	bool IsSpaceFree(const FVector& Travel, const AActor* Ball) const;
	void RunFallback(UFootballActionData* Action, const FActionContext& Context, EActionValidation Reason);
	// The running action is over (its executor ended it, or nothing was playing any more): cooldown, recovery, final state.
	void EndAction(bool bCancelled);
	void SetActionState(EActionState NewState, FGameplayTag Tag);
	void RecordSample();
	void DrawDebug() const;
	float CooldownLeft(const UFootballActionData* Action) const;

	UFUNCTION()
	void OnSkillStateChanged(UFootballSkillData* Skill, ESkillState NewState);

	UFUNCTION()
	void OnSkillEnded(UFootballSkillData* Skill, bool bCancelled);

	UPROPERTY()
	TObjectPtr<UFootballActionData> ActiveAction;

	UPROPERTY()
	TObjectPtr<UFootballSkillComponent> Skills;

	UPROPERTY()
	TObjectPtr<UPlayerAttributesData> LoadedAttributes;

	// The input buffer's one slot (§63).
	UPROPERTY()
	TObjectPtr<UFootballActionData> PendingAction;

	UPROPERTY()
	FActionContext PendingContext;

	UPROPERTY()
	FActionContext ActiveContext;

	bool bPending = false;
	EActionValidation PendingReason = EActionValidation::Valid;
	EActionState ActionState = EActionState::None;
	EActionValidation LastRejection = EActionValidation::Valid;
	EActionInterruptReason LastInterruptReason = EActionInterruptReason::Cancelled;
	bool bInterrupted = false;
	// The skill lost the ball and its profile let it play on: reported once.
	bool bBallLossHandled = false;
	double StartTime = 0.0;
	// Recovery (FActionRecoveryProfile): until when, and at what priority, a new request waits.
	double RecoverUntil = 0.0;
	int32 RecoverPriority = -1;
	TMap<FGameplayTag, double> LastEndTime;

	TArray<FActionRecordSample> Record;
	FGameplayTag RecordTag;
	double RecordUntil = 0.0;
	FVector LastPlayerLocation = FVector::ZeroVector;
};
