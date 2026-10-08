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

// Plays skill moves and makes their ball touches (the doc's AC_FootballSkill, doc §9–§20, §28). Everything comes from
// data: the skill asset, its montage and the montage's Ball Contact Windows. Each tick while a skill plays:
// - Travel: with BakedTrack the capsule follows the clip's root curves, in skill space.
// - Approach (doc §18): before the first touch the ball is a physics ball, steered to come to rest on the first
//   touch's spot (the trajectory's point at its planned time) by the time its window opens, then held there.
// - Touches (doc §19 "calculated response + physics"): while a window is open its body region is checked against the
//   ball every tick (UBallContactResolver, doc §7), along the region's path this tick so a fast foot can't skip the
//   ball. When the check passes:
//   - a collision (Touch, Strike, Pass, Shot, Header, Deflection) gives the ball the velocity of one real collision with
//     the moving body region (DT_BodyRegions restitution / damping), moved by the contact's AssistStrength toward what the
//     skill needs: rolling to the next touch's spot by its planned time, or the authored exit after the last touch;
//   - a carry (Control, Trap, Redirect, Release) pushes the ball along with the body every tick it pushes (no bounce,
//     full grip); the ball separates by itself when the body stops pushing, and a Release (or the last carry) lets it
//     go at the end of its window.
//   Physics moves the ball between touches, rolling.
// - From the first touch to the last the ball is SkillControlled (the skill owns it; contacts of lower priority can't
//   take it). A window that closes without its touch: the first one leaves the ball, the last one lets it go.
// Skill space is fixed at the start: the facing turned toward the input direction (doc §28); mirrored = left foot.
// fb.Debug.Contact also draws the running skill (doc §33) and the tracked region's sweep (spec §103).
// FROZEN (master plan Phase 1, 2026-10-06): the approach steering, the touch's aim at the baked path and the carry are
// the Legacy* functions below. The master spec forbids them (§15, §138); Phase 6 replaces and deletes them.
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

	// Approach: inside this time before the first window the ball is only held on its spot (steering it to arrive any
	// later would overshoot); also the time constant of that hold.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skill|Approach")
	float MinApproachTime = 0.1f;

	// How fast a free rolling ball slows down (1/s), as the model v(t) = v0 e^(-k t) used to aim a touch so the ball rolls to
	// the next one. Since master plan U.3 the ball rolls and slows almost evenly (~0.83 m/s^2 at these speeds), so this is a
	// fit to the Elastico's touch-to-touch roll (~50 cm in 0.73 s): 1.07 brings contact 1 back to 1-2 cm. Until U.3 it
	// was 0.64, set mostly by the old angular damping (1.5). Re-measure if the ball's physics changes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skill|Touch")
	float BallRollDecay = 1.07f;

	// How long the recorder keeps going after a skill ends (the hand-back to the dribble).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skill|Debug")
	float RecordTail = 0.4f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Skill")
	TArray<TObjectPtr<UFootballSkillData>> LoadedSkills;

	UPROPERTY(BlueprintAssignable, Category = "Skill")
	FOnSkillStateChanged OnSkillStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Skill")
	FOnSkillEnded OnSkillEnded;

	// Starts Skill with Ball. InputDirection (world, e.g. the move input) picks the skill direction; bMirrored plays
	// the left-foot version. Returns false, with the reason in the log, if it can't start.
	UFUNCTION(BlueprintCallable, Category = "Skill")
	bool RequestSkill(UFootballSkillData* Skill, AActor* Ball, FVector InputDirection, bool bMirrored);

	// RequestSkill with the loaded skill whose SkillTag is SkillTag.
	UFUNCTION(BlueprintCallable, Category = "Skill")
	bool RequestSkillByTag(FGameplayTag SkillTag, AActor* Ball, FVector InputDirection, bool bMirrored);

	// Stops the skill now; a ball it holds keeps its current velocity.
	UFUNCTION(BlueprintCallable, Category = "Skill")
	void CancelSkill();

	UFUNCTION(BlueprintPure, Category = "Skill")
	bool IsSkillActive() const { return ActiveSkill != nullptr; }

	// The running skill works with the ball (it has contact windows), so the dribble must leave the ball alone.
	UFUNCTION(BlueprintPure, Category = "Skill")
	bool IsSkillUsingBall() const { return ActiveSkill != nullptr && Windows.Num() > 0; }

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

	// Ground point under the player now.
	FVector GroundRoot() const;

	void UpdateBall(float T, float DeltaTime);
	// Follows the body region of window Index every tick (point, normal, velocity).
	void TrackContactRegion(int32 Index, float DeltaTime);
	void EvaluateWindow(int32 Index, float T, float DeltaTime);
	void Touch(int32 Index, float T);
	void CloseWindow(int32 Index);
	// The skill stops owning the ball (it rolls on, Free / Airborne).
	void LetGo();
	void UpdateSkillState();

	// ---- LEGACY ball-trajectory authority: frozen in master plan Phase 1 (2026-10-06), deleted in Phase 6 ------------
	// The master spec forbids it (§15, §138, §171 #1-#4, #6): it aligns the BALL to the animation instead of the player
	// to the ball, aims a touch (AssistStrength up to 1) at the velocity that rolls it to the next touch's baked spot, and
	// gives a carried ball the foot's velocity. Nothing new may call these; Phase 6 replaces them with player alignment,
	// a raw physical impulse and bounded assist, and deletes this block as one unit.
	//
	// Where the trajectory has the ball at montage time T (now or later). The path is relative to the skill's start;
	// the player's baked travel so far is swapped for where the player really is, so an unblocked player gets exactly
	// the authored path and a blocked one keeps the ball with them.
	FVector LegacyTrajectoryTarget(float T) const;
	// The velocity a freely rolling ball needs now to be on window NextIndex's spot at its planned time.
	FVector LegacyNeededVelocity(int32 NextIndex, float T) const;
	// What a collision touch is moved toward (the next touch's spot in time, or the authored exit after the last touch;
	// written to OutNeeded) and the collision moved toward it by the contact's AssistStrength (returned).
	FVector LegacyAim(int32 Index, float T, const FBallContactDescriptor& D, const FVector& Physical, const FVector& BallVelocity,
		bool bLeaves, FVector& OutNeeded) const;
	// A carry: the ball takes the body region's velocity (no bounce, full grip).
	FVector LegacyCarryVelocity(const FWindow& Window, const FVector& BallVelocity, double Radius) const;
	// Approach (doc §18): steers the ball onto the first touch's spot, then holds it there.
	void LegacyApproach(float T, float DeltaTime);
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

	// Ball flow: approaching -> touched (owned, rolling between touches) -> released; or left (a touch missed, taken away).
	bool bApproaching = false;
	bool bOwned = false;
	bool bEverTouched = false;
	bool bReleased = false;
	// Where the skill wants the ball: the approach spot, or where the last touch aimed it.
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
};
