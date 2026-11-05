#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "FootballTypes.h"
#include "PlayerBallInteractionComponent.generated.h"

class UBallMovementComponent;
class USceneComponent;
class USkeletalMeshComponent;

// Character side of ball interaction (the doc's AC_BallInteraction):
// - body regions (doc §4, §21): DT_BodyRegions gives every region a bone or socket plus an offset and normal;
// - body collisions: the skeletal mesh is set to report per-body overlaps with the ball, which the ball's
//   UBallInteractionComponent turns into deflections;
// - the dribble, called by the Blueprint every tick. The touch dribble (default since 2026-10-10, spec §18-22, §67-70, §78):
//   the ball moves only through touches at the feet - between them it rolls freely and the player has to reach it, as in FIFA
//   (a loose ball is taken by a first touch, a dribble is a series of touches at a speed-dependent interval). Off = the old
//   velocity servo ported 1:1 from BP_FootballPlayer.UpdateDribble (A/B tests). Agile dribbling (RB) = a closer, quicker profile;
// - possession's rules (spec §70 "never visible magnetism", §78 SeparationLimit): the ball is taken only at the feet and let
//   go once it got away. The flags themselves (bHasBall / ControlledBall / bCanCollect) still live in the Blueprint.
UCLASS(ClassGroup = (Football), meta = (BlueprintSpawnableComponent))
class FOOTBALLARENA_API UPlayerBallInteractionComponent : public UActorComponent {
	GENERATED_BODY()
public:
	UPlayerBallInteractionComponent();

	// What this character counts as when it owns the ball (doc §30).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball")
	EBallOwnerType OwnerType = EBallOwnerType::Player;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Regions")
	TSoftObjectPtr<UDataTable> BodyRegionTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Football/Data/DT_BodyRegions.DT_BodyRegions")));

	// Collision channel of the ball; the mesh is set to overlap it so body touches are reported.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ball|Regions")
	TEnumAsByte<ECollisionChannel> BallChannel = ECC_GameTraceChannel1;

	// Name of the owner's scene component the dribbled ball is pulled toward.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	FName GuidePointName = TEXT("BallGuidePoint");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	float DribbleStrength = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	float MaxDribbleVelocity = 900.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	float DribbleBlend = 0.4f;

	// Agile dribbling (FIFA 22: hold RB, spec §69 "a closer control zone"): the ball is kept at most this far in front of
	// the player (cm, along the facing) instead of at the guide point. The guide point's height and sideways offset are
	// kept. 30 since master plan Phase 6 (was 40): the elastico's outside touch meets the ball beside the right foot.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	float AgileGuideDistance = 30.f;

	// While agile dribbling the player walks at most this fast (cm/s, the character's MaxWalkSpeed); 0 = leave the speed
	// alone. The user's choice (2026-10-10): ~2.5 m/s, the pace the elastico clip was captured for.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	float AgileMaxSpeed = 250.f;

	// Possession is taken only with the ball at the feet: within CaptureRadius (cm) of where the agile dribble holds it, and
	// at most CaptureMaxSpeed (cm/s) faster or slower than the player - so taking it needs no visible pull. (The old rule took
	// any ball under 8 m/s anywhere in a 60 cm zone 110 cm ahead and reeled it in from up to 1.7 m.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Possession")
	float CaptureRadius = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Possession")
	float CaptureMaxSpeed = 800.f;

	// It is let go once the ball is further than this (cm) from the dribble's guide point (the touch dribble: from where the foot
	// meets the ball): a ball that got away is never pulled back.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Possession")
	float SeparationLimit = 200.f;

	// The touch dribble (see the class comment). Off = the old velocity servo, which pulls the ball toward the guide point every
	// tick from anywhere within SeparationLimit - kept for A/B tests.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	bool bTouchDribble = true;

	// Touch profiles: walking, sprinting (blended by the player's speed between WalkControlSpeed and SprintControlSpeed) and the
	// agile dribble (RB held). Sprinting knocks the ball further and touches it less often (spec §20); agile keeps it closest and
	// touches it most often, and turns it most sharply (§69).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	FBallControlProfile WalkControl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	FBallControlProfile SprintControl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	FBallControlProfile AgileControl;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	float WalkControlSpeed = 430.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	float SprintControlSpeed = 660.f;

	// Where the legs reach a held ball (cm from the player's centre, any profile): a turn, a slow-down or a pressed RB can still
	// touch it there. A loose ball must be at the touch point (the profile's TouchReach) to be taken.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch", meta = (ClampMin = "1"))
	float HeldTouchReach = 75.f;

	// No two touches closer than this (s): touching every tick would be the old servo again.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch", meta = (ClampMin = "0"))
	float MinTouchInterval = 0.12f;

	// A turn touch once the ball's path is this far (deg) off the direction the stick asks.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch", meta = (ClampMin = "0", ClampMax = "180"))
	float TurnTouchAngle = 20.f;

	// With the stick let go, a ball faster than this (cm/s) gets one touch that stops it in front of the player.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch", meta = (ClampMin = "0"))
	float StopTouchSpeed = 40.f;

	// Touches in step with the run: a planned touch waits for the touching foot's swing - the run loops' "Dribble Foot Window"
	// notifies (UAnimNotify_DribbleFoot) - so the foot meets the ball, and sends it to where that foot will be a stride later.
	// Turn, start, correction and stop touches stay immediate. Off, while agile dribbling, or with no windows coming (standing,
	// a skill playing), planned touches keep the profile's TouchInterval. Off by default since 2026-10-24: the dribble is back on
	// the player's own mocap loops, which have no windows yet (the user's new ball clips will get them).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch")
	bool bFootSyncDribble = false;

	// A planned touch overdue by a stride, and at least this (s), is made without its foot's window: the ball is never lost
	// waiting for a foot.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch", meta = (ClampMin = "0"))
	float MaxFootWindowWait = 0.25f;

	// In step with the feet, a turn or correction touch waits up to this (s) for either foot's window and is made by that foot
	// (in a turn the ball changes sides); with no foot coming that soon it is made at once. 0 = always at once.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Touch", meta = (ClampMin = "0"))
	float TurnFootWindowWait = 0.1f;

	// A run loop's "Dribble Foot Window" notify: Foot swings through the ball's place now (late swing, toe low, still driving forward).
	void OnDribbleFootWindow(EFootSelection Foot);

	// One stride of the run as the foot windows measure it (s; 0 = none yet) - tests, debugging.
	UFUNCTION(BlueprintPure, Category = "Dribble|Touch")
	float GetStridePeriod() const { return StridePeriod; }

	// Carry the ball (FIFA: the player is moved to the ball, the ball is never pulled). If running where the stick says would
	// leave the ball out of the legs' reach ReleaseLookAhead seconds from now, the player goes to the ball first - at its pace,
	// closing to CollectGap (cm) at CollectGain (1/s) - so the turn touches can bring it round; then he runs where the stick says.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Steer")
	bool bSteerDribble = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Steer", meta = (ClampMin = "0"))
	float ReleaseLookAhead = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Steer", meta = (ClampMin = "0"))
	float CollectGap = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble|Steer", meta = (ClampMin = "0"))
	float CollectGain = 4.f;

	// Call every tick: dribbles Ball while bHasBall, otherwise lets go of a ball this character was dribbling (and, with the
	// touch dribble, takes a loose ball at the feet with a first touch). Does nothing while the owner's skill works with the ball.
	UFUNCTION(BlueprintCallable, Category = "Dribble")
	void UpdateDribble(bool bHasBall, AActor* Ball);

	// Agile dribbling on/off (the player's BP calls it from the RB button). On: the walk speed drops to AgileMaxSpeed. Off:
	// it goes back to what it was, unless something else (the sprint) has set a speed meanwhile.
	UFUNCTION(BlueprintCallable, Category = "Dribble")
	void SetAgileDribble(bool bAgile);

	UFUNCTION(BlueprintPure, Category = "Dribble")
	bool IsAgileDribble() const { return bAgileDribble; }

	// The touch profile in use now: agile while RB is held, else walk -> sprint by the player's (or the asked) speed.
	UFUNCTION(BlueprintPure, Category = "Dribble|Touch")
	FBallControlProfile GetActiveControlProfile() const;

	// Touches made so far, and the last one (tests, debugging).
	UFUNCTION(BlueprintPure, Category = "Dribble|Touch")
	int32 GetDribbleTouchCount() const { return DribbleTouchCount; }

	UFUNCTION(BlueprintPure, Category = "Dribble|Touch")
	FDribbleTouchResult GetLastDribbleTouch() const { return LastDribbleTouch; }

	// The movement input while dribbling (UFootballCharacterMovement calls it every move): the stick's own input while the
	// ball will stay at the feet, otherwise a run to the ball at its pace. MaxSpeed = the movement's top speed now (cm/s).
	FVector SteerDribbleInput(const FVector& Input, float MaxSpeed) const;
	// True while this player carries a ball with the touch dribble and no skill has it: the body then faces where the stick
	// asks (UFootballCharacterMovement), whatever way the steering runs.
	bool IsCarryingBall() const;
	// True while this player holds a ball with the touch dribble - also while a skill works with it.
	bool HasDribbledBall() const { return bTouchDribble && DribbledBall.IsValid(); }

	// A kick of this player's (UBallMovementComponent::Launch calls it): picks the foot with ChooseKickFoot (his preferred
	// foot and WeakFoot attribute) and returns VelocityChange with the weak foot's loss of speed and accuracy applied.
	FVector ApplyKickFoot(const FVector& VelocityChange, const FVector& BallLocation);

	// The last kick's foot (tests, debugging).
	UFUNCTION(BlueprintPure, Category = "Football|Kick")
	FKickFootChoice GetLastKickFoot() const { return LastKickFoot; }

	// ---- the touch dribble's rules: stateless, so the self-test runs them without a world ----

	// One tick of the touch dribble: is the ball within the foot's reach, should it be touched now, and with what velocity.
	// A touch sends the ball to where it should be when the next touch is due - ControlDistance in front of where the player
	// will be then - rolled by the ball's own model, and changes its velocity by at most MaxCorrectionSpeed. A ball out of
	// reach is never touched: no pull from a distance.
	UFUNCTION(BlueprintPure, Category = "Football|Dribble")
	static FDribbleTouchResult PlanDribbleTouch(const FDribbleTouchQuery& Query);

	// Where the player will be (relative to now) after Time seconds, his velocity moving from Velocity to DesiredVelocity at
	// Acceleration (speeding up) or Braking (slowing down) cm/s^2. Ground plane only.
	UFUNCTION(BlueprintPure, Category = "Football|Dribble")
	static FVector PredictPlayerDisplacement(FVector Velocity, FVector DesiredVelocity, float Acceleration, float Braking, float Time);

	// A + (B - A) * Alpha for every field.
	UFUNCTION(BlueprintPure, Category = "Football|Dribble")
	static FBallControlProfile BlendControlProfiles(const FBallControlProfile& A, const FBallControlProfile& B, float Alpha);
	// Which foot strikes a kick toward KickDirection with the ball at BallLocation (spec §61; FIFA documents no rule, this is
	// ours): each foot costs how far the ball sits across the body from it and how far the kick goes out past that foot's own
	// side, and the weak foot costs (100 - WeakFoot) / 100 more; the cheaper foot kicks, the preferred one on a tie. The weak
	// foot then loses up to 15% speed and aims up to 8 deg off (EA, FC Mobile: "a decrease in accuracy and ball speed"),
	// scaled the same way.
	UFUNCTION(BlueprintPure, Category = "Football|Kick")
	static FKickFootChoice ChooseKickFoot(FVector PlayerLocation, FVector PlayerForward, FVector BallLocation, FVector KickDirection,
		EFootSelection PreferredFoot, float WeakFoot);

	// World point and outward normal of a body region. False if the region has no usable row.
	UFUNCTION(BlueprintCallable, Category = "Ball|Regions")
	bool GetRegionPoint(EBodyRegion Region, FVector& OutPoint, FVector& OutNormal) const;

	// The bone (or socket's bone) a region hangs on.
	UFUNCTION(BlueprintCallable, Category = "Ball|Regions")
	FName GetRegionBone(EBodyRegion Region) const;

	// Number of regions loaded from the table (tests).
	UFUNCTION(BlueprintPure, Category = "Ball|Regions")
	int32 GetLoadedRegionCount() const;

	// The region nearest WorldPoint among those on Bone or its child bones (any bone if Bone is None).
	bool FindContactRegion(FName Bone, const FVector& WorldPoint, EBodyRegion& OutRegion, FVector& OutPoint, FVector& OutNormal, const FBodyRegionRow*& OutRow) const;

	const FBodyRegionRow* GetRegionRow(EBodyRegion Region) const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;

private:
	USkeletalMeshComponent* GetMesh() const;
	// The guide point, pulled in along the facing to at most MaxAhead in front of the player.
	FVector GuideWithin(double MaxAhead) const;
	// The old velocity servo (bTouchDribble off), unchanged.
	void UpdateServoDribble(bool bHasBall, AActor* Ball);
	// Servo dribble, not holding the ball: takes it if it is at the feet (CaptureRadius, CaptureMaxSpeed); re-arms bCanCollect after a kick.
	void TryCapture();
	// The touch dribble (bTouchDribble on).
	void UpdateTouchDribble(bool bHasBall, AActor* Ball);
	// What the stick asks for now: its direction times the speed it asks (ground plane).
	FVector GetDesiredVelocity() const;
	FDribbleTouchQuery MakeTouchQuery(const AActor* Ball, const UBallMovementComponent* Movement) const;
	// Makes the planned touch (claims the ball at Dribble priority; the ball leaves rolling) and records it.
	void MakeDribbleTouch(AActor* Ball, UBallMovementComponent* Movement, const FDribbleTouchResult& Touch);
	void DrawDribbleDebug(const FDribbleTouchResult& Plan, const AActor* Ball) const;
	// the first ball in the world; keep a list when there is more than one.
	AActor* FindBall();

	TWeakObjectPtr<AActor> KnownBall;
	// The touch dribble's clock: the last touch, when the next one is planned, and why the last one was made.
	double LastTouchTime = -1.0e9;
	double NextTouchTime = 0.0;
	EDribbleTouchReason LastTouchReason = EDribbleTouchReason::None;
	int32 DribbleTouchCount = 0;
	FDribbleTouchResult LastDribbleTouch;
	FKickFootChoice LastKickFoot;
	// The foot windows (index 0 = right, 1 = left): when each came last, whether one came since the dribble last looked, and
	// the stride they measure (s, eased).
	double FootWindowTime[2] = {-1.0e9, -1.0e9};
	bool bFootWindowPending[2] = {false, false};
	float StridePeriod = 0.f;

	UPROPERTY()
	TObjectPtr<USceneComponent> GuidePoint;

	UPROPERTY()
	TObjectPtr<UDataTable> LoadedRegionTable;

	// Indexed by EBodyRegion; points into LoadedRegionTable.
	TArray<const FBodyRegionRow*> RegionRows;

	TWeakObjectPtr<UBallMovementComponent> DribbledBall;
	bool bAgileDribble = false;
	// The walk speed before agile dribbling slowed it, while it is slowed.
	float SpeedBeforeAgile = 0.f;
	bool bAgileSpeedApplied = false;
};
