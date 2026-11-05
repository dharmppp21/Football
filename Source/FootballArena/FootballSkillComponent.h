#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "GameplayTagContainer.h"
#include "FootballTypes.h"
#include "FootballSkillComponent.generated.h"

class AController;
class UAnimInstance;
class UAnimMontage;
class UBallInteractionComponent;
class UBallMovementComponent;
class UBallStateComponent;
class UFootballSkillData;
class UPlayerBallInteractionComponent;
class USkeletalMeshComponent;
struct FAnimMontageInstance;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSkillStateChanged, UFootballSkillData*, Skill, ESkillState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSkillEnded, UFootballSkillData*, Skill, bool, bCancelled);

// Plays skill moves and makes their ball touches (the doc's AC_FootballSkill, doc §9–§20, §28; master spec §31). It is the
// executor of UFootballActionComponent: the action layer validates a skill (PrepareSkill gives it the reach and the
// alignment) and starts it (StartSkill); RequestSkill / RequestSkillByTag go through the action layer. Everything comes
// from data: the skill asset, its montage and the montage's Ball Contact Windows. Each tick while a skill plays:
// - Travel: with BakedTrack the capsule follows the clip's root curves, in skill space; with bCarryEntryVelocity the
//   player also keeps his entry velocity until the first contact, fading after it.
// - Alignment (spec §15): the PLAYER is moved to the real ball, never the ball to the animation. The turn is folded into
//   the start, the capsule move is spread over the warp window on top of the travel, and after each touch the next
//   contact is re-aligned (position only).
// - Touches: while a window is open its body region is checked against the ball every tick (UBallContactResolver), along
//   the region's path this tick so a fast foot can't skip the ball. When the check passes, by the contact's ResponseMode
//   (spec §122): a physical touch / redirect / strike / deflection is one raw collision of the ball with the moving region
//   (UBallPhysicsResponse: the region's restitution and grip x the BallResponseProfile, with the spin the contact point
//   gives), then bounded assist (UBallAssist, spec §14) toward the intent - the next contact's spot in time, or after the
//   last contact its authored exit. A controlled contact / energy absorption is a carry: an inelastic push every tick
//   the body presses, let go when its window ends.
// - Between and after the touches the ball is a physics ball: no trajectory servo (§31).
// - From the first touch to the last the ball is SkillControlled (the skill owns it; contacts of lower priority can't
//   take it). A window that closes without its touch: the first one leaves the ball, the last one lets it go.
// Skill space is fixed at the start: the facing turned toward the input direction (doc §28); mirrored = left foot.
// fb.Debug.Contact also draws the running skill (doc §33) and the tracked region's sweep (spec §103).
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UFootballSkillComponent : public UActorComponent {
	GENERATED_BODY()
public:
	UFootballSkillComponent();

	// Every skill asset in this folder (and below) is loaded at BeginPlay; RequestSkillByTag picks from them.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill", meta = (ContentDir))
	FDirectoryPath SkillDirectory;

	// The AnimBP the mesh is switched to at BeginPlay (the montages play on its slot). Empty = keep the mesh's own.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	TSoftClassPtr<UAnimInstance> AnimClass = TSoftClassPtr<UAnimInstance>(FSoftObjectPath(TEXT("/Game/Football/Animations/ABP_Footballer.ABP_Footballer_C")));

	// Grip of a body surface on the ball (Coulomb friction) for a raw response, where the descriptor sets none.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skill|Touch", meta = (ClampMin = "0"))
	float DefaultFriction = 0.5f;

	// How long the recorder keeps going after a skill ends (the hand-back to the dribble).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skill|Debug")
	float RecordTail = 0.4f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Skill")
	TArray<TObjectPtr<UFootballSkillData>> LoadedSkills;

	UPROPERTY(BlueprintAssignable, Category = "Skill")
	FOnSkillStateChanged OnSkillStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Skill")
	FOnSkillEnded OnSkillEnded;

	// Asks the owner's UFootballActionComponent for Skill with Ball. InputDirection (world, e.g. the move input) picks the
	// skill direction; bMirrored plays the left-foot version. True = it started now; false = refused or waiting in the
	// action layer's input buffer (the reason is in the log).
	UFUNCTION(BlueprintCallable, Category = "Skill")
	bool RequestSkill(UFootballSkillData* Skill, AActor* Ball, FVector InputDirection, bool bMirrored);

	// RequestSkill with the loaded action tagged SkillTag (UFootballActionComponent::RequestAction).
	UFUNCTION(BlueprintCallable, Category = "Skill")
	bool RequestSkillByTag(FGameplayTag SkillTag, AActor* Ball, FVector InputDirection, bool bMirrored);

	// Stops the skill now; a ball it holds keeps its current velocity.
	UFUNCTION(BlueprintCallable, Category = "Skill")
	void CancelSkill();

	UFUNCTION(BlueprintPure, Category = "Skill")
	bool IsSkillActive() const { return ActiveSkill != nullptr; }

	// The running skill works with the ball (it has contact windows), or its recovery still keeps the normal control off it
	// (FActionRecoveryProfile::ResumeControlDelay), so the dribble must leave the ball alone.
	UFUNCTION(BlueprintPure, Category = "Skill")
	bool IsSkillUsingBall() const;

	UFUNCTION(BlueprintPure, Category = "Skill")
	ESkillState GetSkillState() const { return SkillState; }

	UFUNCTION(BlueprintPure, Category = "Skill")
	UFootballSkillData* GetActiveSkill() const { return ActiveSkill; }

	UFUNCTION(BlueprintPure, Category = "Skill")
	FSkillBasis GetSkillBasis() const { return Basis; }

	UFUNCTION(BlueprintPure, Category = "Skill")
	ESkillDirection GetSkillDirection() const { return SkillDirection; }

	// Every tick of the last skill plus RecordTail seconds after it (tests, doc §40).
	UFUNCTION(BlueprintCallable, Category = "Skill|Debug")
	TArray<FSkillDebugSample> GetLastSkillRecord() const { return Record; }

	// ---- the executor side of UFootballActionComponent (master plan Phase 5) ----

	// What the action layer needs before it may start Skill in Basis. False = it can't play here at all (no montage or
	// AnimBP slot, no body regions), with the reason logged. Otherwise OutTravel = the skill's whole move (baked travel,
	// entry momentum, alignment), and for the first contact its alignment target (§120, worked out from the predicted
	// free ball, §72; without bAlignPlayer the limits are zero). bOutReachable = the first touch can be made.
	bool PrepareSkill(const UFootballSkillData* Skill, const FActionContext& Context, const FSkillBasis& Basis, FActionAlignmentTarget& OutAlignment,
		bool& bOutReachable, FVector& OutTravel) const;

	// Plays Skill in Basis (already validated and turned by the alignment): montage, travel, contact windows and, with
	// bAlignPlayer, the alignment warp. False if the montage did not play.
	bool StartSkill(UFootballSkillData* Skill, const FActionContext& Context, const FSkillBasis& Basis, ESkillDirection Direction,
		const FActionAlignmentTarget& InAlignment);

	// The latest recorded tick (the action recorder reads it), or null.
	const FSkillDebugSample* GetLatestSample() const { return Record.Num() > 0 ? &Record.Last() : nullptr; }

	// The skill had the ball and something else took it (spec §65 BallLost).
	bool WasBallTakenAway() const { return bBallTakenAway; }

	const FActionAlignmentTarget& GetAlignment() const { return Alignment; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	enum class EWindowPhase : uint8 { Pending, Open, Done };

	struct FWindow {
		float Start = 0.f;
		float End = 0.f;
		int32 ContactIndex = 0;
		FName Label; // the notify's DebugLabel
		EWindowPhase Phase = EWindowPhase::Pending;
		bool bTouched = false;
		FContactResult Result;
	};

	USkeletalMeshComponent* GetMesh() const;
	UAnimInstance* GetAnimInstance() const;
	FAnimMontageInstance* GetMontageInstance() const;
	float GetPlayRate() const;
	void LoadSkills();
	static TArray<FWindow> CollectWindows(const UFootballSkillData& Skill, const UAnimMontage& InMontage);

	// Capsule travel at montage time T: the BakedTrack curves, cm (forward, right) of the start.
	static FVector2D SampleTrackOf(const UFootballSkillData& Skill, const UAnimMontage& InMontage, float T);
	FVector2D SampleTrack(float T) const;
	// The capsule's travel velocity at T, in the world (the movement component's own velocity is zero during a skill).
	FVector TrackVelocity(float T) const;
	void ApplyTravel(float T);
	void UpdateTurn();
	// Spreads the alignment's capsule move over its warp window (smoothstep in montage time), on top of the travel.
	void ApplyAlignment(float T);
	// After a touch: the alignment for window NextIndex's contact, from where the ball will be then (position only).
	void Realign(int32 NextIndex, float T);
	// The entry momentum's move from montage time FromT to ToT: the entry velocity held until EntryHoldTime, then fading.
	FVector EntryDisplacement(float FromT, float ToT) const;
	void ApplyEntryMomentum(float T);
	// Where the capsule's ground point will be at montage time ContactTime, seen from now (T): the travel still to come,
	// what is left of the alignment warp, but not the entry momentum (EntryDisplacement adds that separately).
	FVector PredictRootAt(float ContactTime, float T) const;
	// Where window Index's contact expects the ball (its ExpectedBallOffset from the predicted root) at its ContactTime.
	FVector PredictContactSpot(int32 Index, float T) const;
	// A non-last contact's intent: the velocity that rolls the ball from From to window NextIndex's spot in time.
	FVector IntentToward(int32 NextIndex, float T, const FVector& From) const;
	// The player's skill (the action's AttributeProfile.AssistAttribute) through the attributes' curve, applied to an assist.
	FBallAssistInput ScaleAssistForPlayer(const FBallAssistInput& Input) const;

	// Ground point under the player now.
	FVector GroundRoot() const;
	// The ground point at montage time At this tick (At <= Now = this tick's T): now, less this skill's moves since (the
	// travel, the alignment warp, the entry momentum). The start turn is taken as done (it ends before any window).
	FVector GroundRootAt(float At, float Now) const;
	// Descriptor D's baked RegionPath at montage time At this tick (world point and normal). False = no baked path.
	bool BakedRegionAt(const FBallContactDescriptor& D, float At, float Now, FVector& OutPoint, FVector& OutNormal) const;

	void UpdateBall(float T, float DeltaTime);
	// Follows the body region of window Index every tick (point, normal, velocity).
	void TrackContactRegion(int32 Index, float DeltaTime);
	void EvaluateWindow(int32 Index, float T, float DeltaTime);
	// BallCenter and BodyVelocity: the ball and the region's velocity where the swept region met it.
	void Touch(int32 Index, float T, const FVector& BallCenter, const FVector& BodyVelocity);
	void CloseWindow(int32 Index);
	// The skill stops owning the ball (it rolls on, Free / Airborne).
	void LetGo();
	void UpdateSkillState();

	void SetSkillState(ESkillState NewState, UFootballSkillData* Skill);
	void Finish(bool bCancelled);
	void OnMontageEnded(UAnimMontage* InMontage, bool bInterrupted, int32 InRequestId);
	void RecordSample(float T);
	void DrawDebug(float T) const;

	UPROPERTY()
	TObjectPtr<UFootballSkillData> ActiveSkill;

	UPROPERTY()
	TObjectPtr<UAnimMontage> ActiveMontage;

	UPROPERTY()
	TObjectPtr<AActor> ActiveBall;

	UPROPERTY()
	TObjectPtr<UBallMovementComponent> BallMovement;

	UPROPERTY()
	TObjectPtr<UBallStateComponent> BallStateComp;

	UPROPERTY()
	TObjectPtr<UBallInteractionComponent> BallContacts;

	UPROPERTY()
	TObjectPtr<UPlayerBallInteractionComponent> Regions;

	TArray<FWindow> Windows;
	ESkillState SkillState = ESkillState::None;
	ESkillDirection SkillDirection = ESkillDirection::Forward;
	FSkillBasis Basis;
	bool bActiveMirrored = false;
	int32 RequestId = 0;
	int32 MontageInstanceId = INDEX_NONE;
	float MontageLength = 1.f;
	float LastMontageTime = 0.f;
	FVector2D PrevTrack = FVector2D::ZeroVector;
	double StartTime = 0.0;
	// The turn from the facing to the skill direction, spread over the montage's blend-in.
	FQuat TurnFrom = FQuat::Identity;
	FQuat TurnTo = FQuat::Identity;
	float TurnDuration = 0.f;
	bool bTurning = false;

	// Ball flow: wanted (before the first touch) -> touched (owned, rolling between touches) -> released; or left (a touch
	// missed, taken away).
	bool bWantsBall = false;
	bool bOwned = false;
	bool bEverTouched = false;
	bool bReleased = false;
	bool bBallTakenAway = false;
	// The dribble stays off the ball until then (the recovery's ResumeControlDelay).
	double HoldBallUntil = 0.0;

	// Player alignment (spec §15): the current target, how far through its warp window the move is, and the total moved.
	FActionAlignmentTarget Alignment;
	bool bAlign = false;
	bool bWarping = false;
	float WarpAlpha = 0.f;
	FVector AlignmentApplied = FVector::ZeroVector;
	// Entry momentum (FActionAlignmentProfile::bCarryEntryVelocity): the velocity, how long it is held (montage seconds,
	// the first contact's ContactTime) and how fast it fades after (s).
	bool bEntryMomentum = false;
	FVector EntryVelocity = FVector::ZeroVector;
	float EntryHoldTime = 0.f;
	float EntryDecayTime = 0.15f;
	// Where the skill expects the ball next: the next contact's predicted ball, or after a touch the spot it aims at.
	FVector LastTarget = FVector::ZeroVector;

	// Body region of the next contact, tracked every tick so its velocity is known when its window opens (spec §10).
	int32 TrackedWindow = INDEX_NONE;
	FContactTracker Tracker;
	// Window evaluated this tick, for the recorder (it may have closed in the same tick).
	int32 LastEvaluated = INDEX_NONE;

	TWeakObjectPtr<AController> IgnoredController;

	TArray<FSkillDebugSample> Record;
	double RecordUntil = 0.0;
	// The last touch, for the recorder (set on its tick).
	bool bTouchedThisTick = false;
	FVector TouchPhysical = FVector::ZeroVector;
	FVector TouchNeeded = FVector::ZeroVector;
	FVector TouchApplied = FVector::ZeroVector;
	FVector TouchImpulse = FVector::ZeroVector;
};
