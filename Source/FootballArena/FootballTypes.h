#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "FootballTypes.generated.h"

class AActor;

// Which movement model the ball is in (doc §12). Who owns the ball is tracked separately (doc §30).
// GoalkeeperControlled (spec §4, §128): a goalkeeper has captured the ball; reached only through Receiving.
UENUM(BlueprintType)
enum class EBallState : uint8 { Free, Controlled, SkillControlled, Passing, Shooting, Receiving, Deflected, Airborne, GoalkeeperControlled };

UENUM(BlueprintType)
enum class EBallOwnerType : uint8 { None, Player, Opponent, Goalkeeper };

// What a contact does to the ball (doc §5). Touch, Strike, Pass, Shot, Header and Deflection are one collision;
// Control, Trap, Redirect and Release are carries (the body pushes the ball along while it is on it).
UENUM(BlueprintType)
enum class EContactType : uint8 { Touch, Control, Redirect, Strike, Pass, Shot, Header, Trap, Deflection, Release };

// How a contact changes the ball (spec §122): each mode has its own impulse calculation, assist limit and state change.
// Phase 2 only stores it on the descriptors; the responses are built in Phase 3.
UENUM(BlueprintType)
enum class EBallResponseMode : uint8 { PhysicalTouch, PhysicalRedirect, PhysicalStrike, EnergyAbsorption, ControlledContact, Capture, Deflection };

// Side of the ball that is touched (doc §3), relative to a basis (skill direction or the toucher's facing).
// The surface point is Center + normalize(sum of the named axes) * Radius; Center = the centre itself.
UENUM(BlueprintType)
enum class EBallRegion : uint8 {
	Center, Front, Back, Left, Right, Top, Bottom,
	FrontLeft, FrontRight, BackLeft, BackRight,
	TopFront, TopBack, TopLeft, TopRight,
	BottomFront, BottomBack, BottomLeft, BottomRight
};

// Body surfaces that can touch the ball (doc §4). Each one is a row of DT_BodyRegions (bone or socket + offset + normal).
// LeftSole/RightSole and LeftHand/RightHand go beyond the doc's recommended list: the sole is needed for
// drag backs, ball rolls and sole stops, the hands for the goalkeeper.
UENUM(BlueprintType)
enum class EBodyRegion : uint8 {
	LeftFootCenter, LeftFootInside, LeftFootOutside, LeftInstep, LeftToe, LeftToeInside, LeftToeOutside, LeftHeel, LeftHeelInside, LeftHeelOutside,
	RightFootCenter, RightFootInside, RightFootOutside, RightInstep, RightToe, RightToeInside, RightToeOutside, RightHeel, RightHeelInside, RightHeelOutside,
	LeftThigh, RightThigh, LeftKnee, RightKnee, LeftShin, RightShin,
	Chest, ChestLeft, ChestRight, Abdomen, Back,
	Head, Forehead, HeadLeft, HeadRight, HeadTop,
	LeftShoulder, RightShoulder,
	LeftSole, RightSole, LeftHand, RightHand
};

// Contact priority (doc §31) and response authority (spec §145): a higher one wins the ball for the frame, and a lower
// one never overrides it. §145's order: Capture > Strike / SkillContact (Skill) > Receive > ControlledDribble (Dribble)
// > Deflection (Foot, Body) > EnvironmentPhysics (Environment). Saved by name, so the values may move.
UENUM(BlueprintType)
enum class EContactPriority : uint8 { None = 0, Environment = 1, Body = 2, Foot = 3, Dribble = 4, Receive = 5, Skill = 6, Capture = 7 };

// Result of the contact checks (doc §7). Anything but Valid names the first test that failed.
UENUM(BlueprintType)
enum class EContactValidation : uint8 { Valid, TooFar, WrongDirection, OutsideWindow, WrongRegion, TooSlow };

// Skill state machine (doc §11). Completed / Cancelled are reported once when a skill ends, then the state is None.
UENUM(BlueprintType)
enum class ESkillState : uint8 { None, Starting, Contacting, Redirecting, Releasing, Recovering, Cancelled, Completed };

// Feet an action may be done with (spec §3 AllowedFoot). An action's Montage is its right-foot version and its
// AlternateMontage the left-foot one.
UENUM(BlueprintType)
enum class EFootSelection : uint8 { Right, Left, Both };

// Skill direction relative to the facing (doc §28), in 45-degree steps clockwise from Forward; Custom = the input
// direction exactly. A skill's AllowedDirections is a bitmask of these (bit = enum value).
UENUM(BlueprintType, meta = (Bitflags))
enum class ESkillDirection : uint8 { Forward, ForwardRight, Right, BackRight, Backward, BackLeft, Left, ForwardLeft, Custom };

// How the player travels during a skill (doc §10 RootMotionMode; named so it doesn't clash with the engine's ERootMotionMode).
// BakedTrack: the capsule follows the clip's root curves (the skeleton has no root bone yet). FromMontage: native
// montage root motion, applied by the engine. InPlace: no travel.
UENUM(BlueprintType)
enum class ESkillRootMotion : uint8 { FromMontage, InPlace, BakedTrack };

// One recorded tick of the ball, read back by the PIE tests.
USTRUCT(BlueprintType)
struct FBallRecordSample {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Football") float Time = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector Position = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector Velocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector AngularVelocityDeg = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") EBallState State = EBallState::Free;
	UPROPERTY(BlueprintReadOnly, Category = "Football") bool bGrounded = true;
};

// Local skill space (doc §13): X = Forward, Y = Right (flipped when mirrored), Z = Up.
USTRUCT(BlueprintType)
struct FSkillBasis {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Football") FVector Origin = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Football") FVector Forward = FVector::ForwardVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Football") FVector Right = FVector::RightVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Football") FVector Up = FVector::UpVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Football") bool bMirrored = false;

	FVector TransformVector(const FVector& Local) const {
		return Forward * Local.X + (bMirrored ? -Right : Right) * Local.Y + Up * Local.Z;
	}
};

// One authored contact (spec §13). Action data assets hold these; contact windows point at them by index.
// The renamed properties (AimAssist, DirectionThreshold, LocalDirection, ExitSpeed) have CoreRedirects in DefaultEngine.ini.
USTRUCT(BlueprintType)
struct FBallContactDescriptor {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	EBodyRegion BodyContactRegion = EBodyRegion::RightFootInside;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	EBallRegion BallContactRegion = EBallRegion::Back;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	EContactType ContactType = EContactType::Touch;

	// Which response (§122) this contact gets once Phase 3 builds them.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	EBallResponseMode ResponseMode = EBallResponseMode::PhysicalTouch;

	// A higher priority wins the ball for the frame and a lower one never overrides it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	EContactPriority ContactPriority = EContactPriority::Skill;

	// The planned moment of the touch and the window it may happen in (montage seconds; the bake copies the window from
	// the montage's Ball Contact Window, which is what the runtime follows).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	float ContactTime = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	float WindowStart = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	float WindowEnd = 0.f;

	// Fine-tuning nudge of where the ball centre should be, in skill space.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	FVector LocalBallOffset = FVector::ZeroVector;

	// Spec §71: the ball centre for this contact relative to the capsule's ground point at ContactTime, in skill space,
	// worked out from the clip by the bake (Tools/prep_skill_anims.py). Player alignment (§15) moves the player so the
	// real ball sits there; the ball itself is never moved to it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	FVector ExpectedBallOffset = FVector::ZeroVector;

	// The body region's path through the window, baked from the clip like ExpectedBallOffset: its point and normal in skill
	// space relative to the capsule's ground point, one per clip key from RegionPathStart (montage seconds) every
	// RegionPathStep. The touch sweeps this path at a fine fixed step between frames, so it is found at the same moment at
	// any frame rate (the live pose exists only at frame ends; its straight chord cut the elastico's grazing outside touch
	// early, more at 30 fps). Empty = the live region along its chord.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	TArray<FVector> RegionPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	TArray<FVector> RegionPathNormal;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact")
	float RegionPathStart = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact", meta = (ClampMin = "0.001"))
	float RegionPathStep = 1.f / 30.f;

	// Test 1: ball centre within this of where the contact wants it (cm).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	float ContactTolerance = 6.f;

	// Test 2: the body has to be closing on the ball: dot(relative velocity dir, direction to ball) above this.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	float MinApproachDot = 0.3f;

	// Test 5: minimum body speed (cm/s); 0 = off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation")
	float MinBodySpeed = 0.f;

	// Surface response overrides; below 0 = the body region's own value (DT_BodyRegions Restitution, TangentDamping,
	// SpinTransfer). Read by the Phase 3 response.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response", meta = (ClampMin = "-1", ClampMax = "1"))
	float Restitution = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response", meta = (ClampMin = "-1", ClampMax = "1"))
	float TangentialGrip = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response", meta = (ClampMin = "-1"))
	float SpinTransfer = -1.f;

	// The intended exit, in skill space: an INTENT, never a mandatory trajectory (§13) - what the bounded assist bends the
	// physical result toward after an action's LAST contact. An earlier contact's intent is worked out at the touch: the
	// velocity that rolls the ball to the next contact's expected spot in time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Intent")
	FVector PreferredDirection = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Intent")
	float PreferredSpeed = 300.f;

	// Bounded assistance (§14): the share of the gap between the physical result and the intent that is closed
	// (0.10-0.25 realistic, above 0.40 only for arcade-like actions), and the most it may turn the ball (degrees) or
	// change its speed (cm/s). The action's AssistProfile and the player's skill (UBallAssist::ScaleForSkill) scale them.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0", ClampMax = "1"))
	float AssistStrength = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0"))
	float MaxAssistAngleDegrees = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0"))
	float MaxAssistSpeed = 100.f;

	// An arcade-like action may close more than 0.40 of the gap (§14); without it UBallAssist caps the strength at 0.40.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist")
	bool bArcadeAssist = false;

	// Debounce (§99): the contact counts once per window; the next window re-arms it (after a confirmed separation if
	// the region is still on the ball), and bAllowRecontact lets a confirmed separation re-arm it inside the window.
	// Off for continuous contacts (carry, trap hold, sole roll, §100): they count every tick the region is on the ball.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debounce")
	bool bProcessOnce = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debounce")
	bool bAllowRecontact = false;

	// The ball leaves the action after this contact.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debounce")
	bool bReleaseAfterContact = false;

	// Upward part of the exit velocity, as a fraction of PreferredSpeed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exit")
	float VerticalBias = 0.f;

	// How much of the player's / the ball's own velocity the exit keeps (doc §20).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exit")
	float InheritPlayerVelocity = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Exit")
	float InheritBallVelocity = 0.f;
};

// A player's attributes (spec §62), 0-100. They change assist bounds, timing tolerance, contact quality, action selection
// and physical parameter ranges - never the physics rules themselves.
USTRUCT(BlueprintType)
struct FFootballAttributes {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0", ClampMax = "100")) float BallControl = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0", ClampMax = "100")) float Dribbling = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "0", ClampMax = "100")) float Agility = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "0", ClampMax = "100")) float Balance = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "0", ClampMax = "100")) float Acceleration = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta = (ClampMin = "0", ClampMax = "100")) float SprintSpeed = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passing", meta = (ClampMin = "0", ClampMax = "100")) float Passing = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passing", meta = (ClampMin = "0", ClampMax = "100")) float LongPassing = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Passing", meta = (ClampMin = "0", ClampMax = "100")) float Crossing = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shooting", meta = (ClampMin = "0", ClampMax = "100")) float ShotPower = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shooting", meta = (ClampMin = "0", ClampMax = "100")) float Finishing = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shooting", meta = (ClampMin = "0", ClampMax = "100")) float Curve = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shooting", meta = (ClampMin = "0", ClampMax = "100")) float Volleys = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shooting", meta = (ClampMin = "0", ClampMax = "100")) float Heading = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mental", meta = (ClampMin = "0", ClampMax = "100")) float Reactions = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mental", meta = (ClampMin = "0", ClampMax = "100")) float Composure = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Technique", meta = (ClampMin = "0", ClampMax = "100")) float WeakFoot = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Technique", meta = (ClampMin = "0", ClampMax = "100")) float SkillMoves = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Goalkeeping", meta = (ClampMin = "0", ClampMax = "100")) float GoalkeeperHandling = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Goalkeeping", meta = (ClampMin = "0", ClampMax = "100")) float GoalkeeperDiving = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Goalkeeping", meta = (ClampMin = "0", ClampMax = "100")) float GoalkeeperReflexes = 50.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Goalkeeping", meta = (ClampMin = "0", ClampMax = "100")) float GoalkeeperKicking = 50.f;
};

// Input of UBallAssist::BoundedBlend (spec §14): a physical result and the intent it may be bent toward, strictly bounded.
USTRUCT(BlueprintType)
struct FBallAssistInput {
	GENERATED_BODY()

	// The ball's velocity as physics produced it (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist") FVector RawVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist") FVector IntendedDirection = FVector::ForwardVector;
	// 0 or less = keep the raw speed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist") float IntendedSpeed = 0.f;
	// The share of the gap closed: 0.10-0.25 realistic, 0.25-0.40 strong, above 0.40 only with bArcade.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0", ClampMax = "1")) float Strength = 0.15f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0")) float MaxAngleDegrees = 10.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0")) float MaxSpeedChange = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist") bool bArcade = false;
};

USTRUCT(BlueprintType)
struct FBallAssistResult {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Assist") FVector Velocity = FVector::ZeroVector;
	// Velocity - RawVelocity: what the assist adds, delivered as a velocity-change impulse on top of the physical result.
	UPROPERTY(BlueprintReadOnly, Category = "Assist") FVector VelocityChange = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Assist") float AngleCorrectionDegrees = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Assist") float SpeedCorrection = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Assist") float StrengthUsed = 0.f;
	// The strength asked for was above 0.40 without bArcade and was capped.
	UPROPERTY(BlueprintReadOnly, Category = "Assist") bool bStrengthCapped = false;
};

// Why the touch dribble touched the ball this tick (UPlayerBallInteractionComponent::PlanDribbleTouch). None = no touch.
// FirstTouch takes a loose ball; Planned = the touch interval is up; Start = the ball is at rest and the player sets off;
// Turn = the asked direction left the ball's path; Overrun = the player is running over the ball; Correct = the ball would be
// out of the foot's reach by the next planned touch (the player slowed, sped up or the ball drifts); Stop = the stick was let go.
UENUM(BlueprintType)
enum class EDribbleTouchReason : uint8 { None, FirstTouch, Planned, Start, Turn, Overrun, Correct, Stop };

// Spec §78 FBallControlProfile, the part the touch dribble uses (master plan 7.3 / 7.4, brought forward on 2026-10-10): FIFA
// dribbles with separate touches - "Dribble Touch Intervals ... how close the player keeps the ball" (EA, FC 26 Pitch Notes) -
// and the ball only ever moves from a touch at the feet. Where the foot meets the ball, how far it can reach, where a touch
// sends the ball and how often, and how much one touch may change it.
USTRUCT(BlueprintType)
struct FBallControlProfile {
	GENERATED_BODY()

	// The foot meets the ball this far in front of the player's centre (cm, along the facing).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0")) float TouchDistance = 40.f;
	// A loose ball is taken only within this (cm, ground plane) of the touch point: no visible pull on a pick-up (§70). A held
	// ball can be touched anywhere the legs reach (FDribbleTouchQuery::HeldReach) - the same in every profile.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "1")) float TouchReach = 30.f;
	// Where a touch sends the ball: this far in front of the player when the next touch is due (cm).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0")) float ControlDistance = 55.f;
	// Seconds from one planned touch to the next (§78 TouchFrequency = 1 / TouchInterval).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0.05")) float TouchInterval = 0.4f;
	// The most one touch may change the ball's velocity (cm/s): a sharp turn takes more than one touch, so the ball curves
	// round instead of snapping (§21, §70).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0")) float MaxCorrectionSpeed = 450.f;
	// The fastest a dribble touch sends the ball (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Control", meta = (ClampMin = "0")) float MaxBallSpeed = 900.f;
};

// Input of UPlayerBallInteractionComponent::PlanDribbleTouch: one tick of the touch dribble, everything it needs so it runs
// without a world (the self-test). Positions and velocities in cm and cm/s; only their ground-plane parts are used.
USTRUCT(BlueprintType)
struct FDribbleTouchQuery {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FVector PlayerLocation = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FVector PlayerVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FVector PlayerForward = FVector::ForwardVector;
	// What the stick asks for: its direction times the speed it asks (zero = the stick is let go).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FVector DesiredVelocity = FVector::ZeroVector;
	// How fast the player's velocity changes toward the asked one (CharacterMovement's acceleration and braking, cm/s^2).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float PlayerAcceleration = 1800.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float PlayerBraking = 2200.f;
	// The ball now (its velocity including this frame's queued response) and whether it is on the ground.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FVector BallLocation = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FVector BallVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") bool bBallGrounded = true;
	// The ball's rolling model (UBallMovementComponent::GetRollModel): where a rolled ball goes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float RollDecel = 81.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float RollDragK = 1.425e-4f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") FBallControlProfile Profile;
	// Seconds since the last touch; seconds past the planned next one (negative = not due yet); the last touch's reason.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float TimeSinceTouch = 1.0e6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float TimeToPlannedTouch = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") EDribbleTouchReason LastReason = EDribbleTouchReason::None;
	// The touch that takes a loose ball.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") bool bFirstTouch = false;
	// The feet: how far each one sits to the right of the player's centre line (cm, - = left; the body regions at the feet),
	// and the foot he prefers when the ball is in the middle. Left at 0 the touch point stays on the centre line.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float LeftFootSide = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float RightFootSide = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") EFootSelection PreferredFoot = EFootSelection::Right;
	// Where the legs reach a held ball: within this of the player's centre (cm) and not behind it (see the planner). Nothing
	// farther is ever touched - a ball out of reach is never pulled (§70). The same in every profile: RB doesn't shorten legs.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float HeldReach = 75.f;
	// No two touches closer than MinTouchInterval (s); a turn touch once the ball's path is TurnTouchAngle (deg) off the asked
	// direction; with the stick let go, a ball faster than StopTouchSpeed (cm/s) gets one stop touch.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float MinTouchInterval = 0.12f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float TurnTouchAngle = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float StopTouchSpeed = 40.f;
	// Touches in step with the feet (UPlayerBallInteractionComponent::bFootSyncDribble): a planned touch waits for the touching
	// foot's window - the run loops' "Dribble Foot Window" notify, late in the swing with the toe low and still driving forward -
	// at most once a stride, or is made a stride (at least MaxWindowWait, s) late without it; every touch sends the ball to where that foot will be at
	// its next window a planned touch may use. bLeftWindow / bRightWindow: that foot's window came this frame; the ages are the
	// seconds since each foot's last window; StridePeriod = one stride (s, a foot's window to its next one).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") bool bSyncToFeet = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") bool bLeftWindow = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") bool bRightWindow = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float LeftWindowAge = 1.0e6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float RightWindowAge = 1.0e6f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float StridePeriod = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float MaxWindowWait = 0.25f;
	// A turn or correction touch waits up to this (s) for either foot's window and is made by that foot (0 = at once).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble") float TurnWindowWait = 0.f;
};

// Output of UPlayerBallInteractionComponent::PlanDribbleTouch.
USTRUCT(BlueprintType)
struct FDribbleTouchResult {
	GENERATED_BODY()

	// Where the foot meets the ball now, the ball's distance from that point (cm, ground plane), and whether the foot can reach
	// it (on the ground and within the reach of FBallControlProfile::TouchReach).
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") FVector TouchSpot = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") float Distance = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") bool bInReach = false;
	// The foot that meets the ball: the one on the ball's side of the body, the preferred one with the ball in the middle.
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") EFootSelection Foot = EFootSelection::Right;
	// The touch to make now (None = no touch): the ball's new velocity (its vertical velocity kept), the point it is sent to and
	// in how long, the velocity change and whether MaxCorrectionSpeed capped it.
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") EDribbleTouchReason Reason = EDribbleTouchReason::None;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") FVector Velocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") FVector Target = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") float Interval = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") float Correction = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") bool bCapped = false;
	// World seconds of the touch (set by the component when it is made).
	UPROPERTY(BlueprintReadOnly, Category = "Dribble") float Time = 0.f;
};

// UPlayerBallInteractionComponent::ChooseKickFoot: the foot a kick is struck with, and what kicking with the weak foot costs.
USTRUCT(BlueprintType)
struct FKickFootChoice {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Kick") EFootSelection Foot = EFootSelection::Right;
	UPROPERTY(BlueprintReadOnly, Category = "Kick") bool bWeakFoot = false;
	// The kick's speed is multiplied by SpeedScale, and its direction may be up to ErrorDeg off (a random share of it).
	UPROPERTY(BlueprintReadOnly, Category = "Kick") float SpeedScale = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Kick") float ErrorDeg = 0.f;
};

// Input of UBallContactResolver::ResolveContact (doc §6).
USTRUCT(BlueprintType)
struct FContactQuery {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FVector BallCenter = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FVector BallVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") float BallRadius = 11.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FVector BodyPoint = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FVector BodyNormal = FVector::ForwardVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FVector BodyVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FVector PlayerVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FBallContactDescriptor Descriptor;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") FSkillBasis Basis;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Contact") bool bWindowOpen = true;
};

// Output of UBallContactResolver::ResolveContact (doc §6).
USTRUCT(BlueprintType)
struct FContactResult {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Contact") EContactValidation Validation = EContactValidation::Valid;
	// The touched point on the ball (descriptor's ball region) and the normal from the body toward the ball.
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector SurfacePoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector ContactNormal = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector BodyPoint = FVector::ZeroVector;
	// Where the ball centre has to be for this contact.
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector TargetBallCenter = FVector::ZeroVector;
	// Exit velocity for launching contact types, zero otherwise.
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector ResponseVelocity = FVector::ZeroVector;
	// Test 1 distance: ball centre to TargetBallCenter (cm).
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float Distance = 0.f;
	// Spec §7: body velocity - ball velocity, split along ContactNormal (NormalSpeed > 0 = closing).
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector RelativeVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float NormalSpeed = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector TangentVelocity = FVector::ZeroVector;
	// Spec §11 (UBallContactResolver::SweepRegionToBall): the body region's path this tick against the ball. The closest
	// approach between the two surfaces (cm, <= 0 = touching), where on the path the touch starts (0 = a tick ago,
	// 1 = now; the closest point when they don't touch) and the touched point on the ball.
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float ClosestApproach = UE_BIG_NUMBER;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float SweepAlpha = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector ContactPoint = FVector::ZeroVector;
};

// One row of DT_BodyRegions; the row name is the EBodyRegion name. The point and normal are given in the
// socket's space (if Socket is set) or the bone's space.
USTRUCT(BlueprintType)
struct FBodyRegionRow : public FTableRowBase {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region") FName Socket;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region") FName Bone;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region") FVector LocalOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region") FVector LocalNormal = FVector(1.f, 0.f, 0.f);
	// Size of the surface patch (debug drawing, nearest-region search).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region") float Radius = 5.f;
	// What the surface is (spec §10), e.g. Boot, Shin, Torso, Head, Hand - a label for responses and effects.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region") FName SurfaceType;
	// Deflection response of this surface (doc §19): bounce of the normal part, loss of the sliding part,
	// share of the sliding speed turned into spin.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deflection") float Restitution = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deflection") float TangentDamping = 0.2f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Deflection") float SpinTransfer = 0.f;
};

// A body region followed through its contact windows (spec §10, §99, §100): where it is and how fast it moves,
// whether the window's contact has been processed, and whether it is on the ball. Any action keeps one per region it
// uses and updates it with UBallContactResolver's tracker functions.
USTRUCT(BlueprintType)
struct FContactTracker {
	GENERATED_BODY()

	// §10: tracked only while the region may take part in a contact; the velocity needs the same region on two ticks.
	UPROPERTY(BlueprintReadOnly, Category = "Contact") EBodyRegion Region = EBodyRegion::RightFootInside;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") bool bTracking = false;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector Point = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector PrevPoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector Normal = FVector::ForwardVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector Velocity = FVector::ZeroVector;
	// §99: the window's contact has been processed. bWaitSeparation: the window opened with the region still on the ball
	// after a processed contact, so it re-arms only after a confirmed separation.
	UPROPERTY(BlueprintReadOnly, Category = "Contact") bool bProcessed = false;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") bool bWaitSeparation = false;
	// §100: on the ball now; touched it in this window; seconds off it; touches after a confirmed separation in this
	// window; impulse given in this window (the Phase 3 response caps it).
	UPROPERTY(BlueprintReadOnly, Category = "Contact") bool bTouching = false;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") bool bTouchedInWindow = false;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float SeparatedFor = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") int32 Recontacts = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float ImpulseUsed = 0.f;
};

// Input of UBallPhysicsResponse::ComputeResponse (spec §7, §91, §122): the ball, the body surface it meets and that
// surface's response parameters. Velocities in cm/s, spin in rad/s, mass in kg.
USTRUCT(BlueprintType)
struct FBallResponseInput {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") EBallResponseMode Mode = EBallResponseMode::PhysicalTouch;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector BallCenter = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector BallVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector BallAngularVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float BallRadius = 11.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float BallMass = 0.43f;
	// Moment of inertia = InertiaFactor * mass * radius^2: 0.667 = a thin shell, the real ball, which BP_Football's body has
	// matched since master plan U.2 (inertia scale 1.667 on the solid sphere's 0.4).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float InertiaFactor = 0.667f;
	// Where the body touches the ball, the normal from the body toward the ball, and the body's velocity there.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector ContactPoint = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector ContactNormal = FVector::ForwardVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector BodyVelocity = FVector::ZeroVector;
	// Share of the approach speed (along the normal) given back.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float Restitution = 0.5f;
	// Coulomb friction: the sideways impulse is at most Friction x the normal impulse (else the surface grips fully).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float Friction = 0.5f;
	// Effective contact strength: the body region's effective mass M as M / (M + ball mass); 1 = an immovable body.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float ContactStrength = 1.f;
};

// Output of UBallPhysicsResponse::ComputeResponse: the raw physical impulse, before any assist (spec §14 adds that).
USTRUCT(BlueprintType)
struct FBallResponse {
	GENERATED_BODY()

	// False when the ball is not closing on the body (no impulse), or for ControlledContact / Capture (not impulses).
	UPROPERTY(BlueprintReadOnly, Category = "Response") bool bContact = false;
	// Applied at the contact point (kg*cm/s); AngularImpulse = (contact point - centre) x LinearImpulse (spec §91).
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector LinearImpulse = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector AngularImpulse = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector VelocityAfter = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector AngularVelocityAfter = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Response") float NormalImpulse = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector TangentImpulse = FVector::ZeroVector;
	// The friction cap was reached: the ball slides on the surface instead of gripping.
	UPROPERTY(BlueprintReadOnly, Category = "Response") bool bSliding = false;
};

// One requested change to the ball (spec §146). Systems hand these to UBallInteractionComponent::SubmitResponse; once
// per frame, just before physics, the highest Priority wins (§145, §147) and the rest are dropped and logged.
USTRUCT(BlueprintType)
struct FBallResponseCommand {
	GENERATED_BODY()

	// Who asks (a debug name such as Dribble, Kick, Skill, Deflection) and the actor behind it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FName Source;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") TObjectPtr<AActor> Instigator = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") EBallResponseMode ResponseType = EBallResponseMode::PhysicalTouch;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") EContactPriority Priority = EContactPriority::Body;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector ContactPoint = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector ContactNormal = FVector::ZeroVector;
	// An impulse (kg*cm/s), or with bVelocityChange a velocity change (cm/s). At ContactPoint with bImpulseAtContactPoint
	// (the engine then adds the spin from the offset), else at the centre.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector LinearImpulse = FVector::ZeroVector;
	// An angular impulse in radians, or with bVelocityChange an angular velocity change (rad/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector AngularImpulse = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") bool bVelocityChange = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") bool bImpulseAtContactPoint = false;
	// Bounded velocity support (the dribble, §6): after the impulses the velocity moves toward DesiredVelocity by at most
	// MaxAllowedCorrection (cm/s; 0 or less = all the way).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") bool bUseDesiredVelocity = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector DesiredVelocity = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") float MaxAllowedCorrection = 0.f;
	// A spin set outright (deg/s): a skill touch's physical response (velocity and spin worked out at once, applied as one SET).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") bool bSetAngularVelocity = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") FVector AngularVelocityDeg = FVector::ZeroVector;
	// The state and owner after the response (§146); bSetStateAfter off leaves the state alone.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") bool bSetStateAfter = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") EBallState BallStateAfter = EBallState::Free;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response") TObjectPtr<AActor> OwnerAfter = nullptr;
};

// One arbitrated command (debug and tests): applied, or dropped and why.
USTRUCT(BlueprintType)
struct FBallCommandRecord {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Response") float Time = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FName Source;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FString Instigator;
	UPROPERTY(BlueprintReadOnly, Category = "Response") EBallResponseMode ResponseType = EBallResponseMode::PhysicalTouch;
	UPROPERTY(BlueprintReadOnly, Category = "Response") EContactPriority Priority = EContactPriority::None;
	UPROPERTY(BlueprintReadOnly, Category = "Response") bool bApplied = false;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FString Note;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector LinearImpulse = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector DesiredVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Response") FVector VelocityBefore = FVector::ZeroVector;
};

// The last body contact the ball handled (debug and tests).
USTRUCT(BlueprintType)
struct FBallContactInfo {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Contact") float Time = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FString Instigator;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") EBodyRegion Region = EBodyRegion::Chest;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") EContactPriority Priority = EContactPriority::None;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") bool bApplied = false;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FString Note;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector BodyPoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector Normal = FVector::ZeroVector;
	// The touched point on the ball: centre - Normal * radius (doc §33 "calculated ball surface point").
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector BallSurfacePoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") float ContactRadius = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector VelocityIn = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") FVector VelocityOut = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Contact") EBallState BallStateAfter = EBallState::Free;
};

// One point of a baked ball path (doc §13–§14): the ball centre relative to the ground point where the skill started,
// in skill space (X forward, Y right, Z height above the ground), at normalized skill time T (0..1 over the montage).
USTRUCT(BlueprintType)
struct FBallTrajectorySample {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trajectory") float T = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Trajectory") FVector LocalOffset = FVector::ZeroVector;
};

// One recorded tick of a skill (UFootballSkillComponent::GetLastSkillRecord), read back by the PIE tests (doc §40).
USTRUCT(BlueprintType)
struct FSkillDebugSample {
	GENERATED_BODY()

	// Seconds since the skill was requested, and the montage position.
	UPROPERTY(BlueprintReadOnly, Category = "Football") float Time = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Football") float MontageTime = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Football") ESkillState SkillState = ESkillState::None;
	UPROPERTY(BlueprintReadOnly, Category = "Football") EBallState BallState = EBallState::Free;
	// True while the skill moves the ball (approach or control).
	UPROPERTY(BlueprintReadOnly, Category = "Football") bool bBallDriven = false;
	// The open contact window's descriptor index (-1 = none) and its latest check (doc §7).
	UPROPERTY(BlueprintReadOnly, Category = "Football") int32 ContactIndex = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Football") EContactValidation Validation = EContactValidation::Valid;
	UPROPERTY(BlueprintReadOnly, Category = "Football") float ContactDistance = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector BodyPoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector SurfacePoint = FVector::ZeroVector;
	// The tracked body region's normal (toward the ball) and velocity this tick.
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector ContactNormal = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector BodyVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector BallPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector BallVel = FVector::ZeroVector;
	// Where the skill wanted the ball this tick (approach spot, or where the last touch aimed).
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TargetPos = FVector::ZeroVector;
	// Set on the tick of a touch: the collision's velocity, what the skill needed, and what was applied (doc §19–§20).
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchPhysical = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchNeeded = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchApplied = FVector::ZeroVector;
	// The touch as an impulse: ball mass x (applied - velocity before), kg*cm/s (spec §104 "final impulse").
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchImpulse = FVector::ZeroVector;
	// How far player alignment (spec §15) has moved the capsule so far, on top of the baked travel.
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector AlignmentApplied = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector ActorPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") float ActorYaw = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector HipsPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector LeftFootPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector RightFootPos = FVector::ZeroVector;
};

// ---- The action layer (master spec §3, §15-16, §63-65, §71-73, §116-121; master plan Phase 5) --------------------------

// Generic action state (spec §116). A skill's finer state (Redirecting...) stays on UFootballSkillComponent.
UENUM(BlueprintType)
enum class EActionState : uint8 { None, Requested, Validating, Starting, Aligning, Playing, Contacting, Releasing, Recovering, Completed, Cancelled, Interrupted };

// Why an action ended early (spec §65); Superseded = a higher-priority action took over (§64). An action's
// InterruptionProfile.AllowedReasons is a bitmask of these (bit = enum value).
UENUM(BlueprintType, meta = (Bitflags))
enum class EActionInterruptReason : uint8 { Cancelled, InterruptedByTackle, InterruptedByCollision, BallLost, PlayerStunned, InvalidState, Superseded };

// The first validator (spec §119) that refused a request. Valid = it may start.
UENUM(BlueprintType)
enum class EActionValidation : uint8 {
	Valid, UnknownAction, CannotPlay, InvalidState, Busy, Cooldown, WrongFoot, NoBall, BallState, NotOwner, TooFar, WrongDirection,
	WrongSpeed, Unreachable, NoSpace
};

// How an action picks its direction (spec §3 DirectionMode): the input relative to the facing, snapped to the action's
// AllowedDirections (skills, doc §28); the input itself; toward the target; along the camera.
UENUM(BlueprintType)
enum class EActionDirectionMode : uint8 { FacingSnapped, Input, Target, Camera };

// What an action request carries (spec §117) and the situation it is judged in (§118). The caller fills the Request part;
// UFootballActionComponent::RequestAction fills the Situation part when it takes the request (and again on every retry
// from the input buffer, so a buffered request is judged by the situation it starts in).
USTRUCT(BlueprintType)
struct FActionContext {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") FVector InputDirection = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") FVector TargetDirection = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") FVector TargetLocation = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") TObjectPtr<AActor> TargetActor = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") TObjectPtr<AActor> BallActor = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") float Power = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") float Spin = 0.f;
	// The left-foot (mirrored) version.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Request") bool bMirrored = false;

	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector BallLocation = FVector::ZeroVector;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector BallVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector PlayerLocation = FVector::ZeroVector;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector PlayerVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector PlayerForward = FVector::ForwardVector;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector PlayerRight = FVector::RightVector;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FVector CameraDirection = FVector::ForwardVector;
	// 0-1. Always 0 until there are opponents (master plan Phases 14-15).
	UPROPERTY(BlueprintReadWrite, Category = "Situation") float OpponentPressure = 0.f;
	// The player's strong foot (weak-foot rules, spec §61) and attributes (§62), from their UPlayerAttributesData.
	UPROPERTY(BlueprintReadWrite, Category = "Situation") EFootSelection PreferredFoot = EFootSelection::Right;
	UPROPERTY(BlueprintReadWrite, Category = "Situation") FFootballAttributes AttributeSnapshot;
	// World seconds when the request was made (the input buffer's clock, §63).
	UPROPERTY(BlueprintReadWrite, Category = "Situation") float RequestTime = 0.f;
};

// How far an action may move the player to the real ball (spec §15, §16, §120). The interim until there is root motion:
// a bounded capsule move spread over a warp window on top of the baked travel, with targets shaped like Motion Warping's.
USTRUCT(BlueprintType)
struct FActionAlignmentProfile {
	GENERATED_BODY()

	// Move the PLAYER so each contact's expected ball offset (§71) lands on the real ball. Off = no correction at all: the
	// action only starts when the ball is already where its first contact expects it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") bool bAlignPlayer = false;
	// Per contact: the most the capsule may be moved (cm) and turned at the start (degrees), and how fast (cm/s) - "do not
	// over-warp". Beyond them the action falls back (§120).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment", meta = (ClampMin = "0")) float MaxPositionCorrection = 30.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment", meta = (ClampMin = "0", ClampMax = "90")) float MaxRotationCorrection = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment", meta = (ClampMin = "1")) float MaxCorrectionSpeed = 250.f;
	// Montage seconds the first warp window opens; it closes at the first contact's ContactTime.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment", meta = (ClampMin = "0")) float WarpStartTime = 0.f;
	// Multi-contact actions re-align after each contact for the next one (position only: the turn is set at the start).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") bool bRealignBeforeEachContact = true;
	// Entry momentum: the player keeps the velocity he had at the request (the dribble's) until the first contact, then it
	// fades with EntryDecayTime (s), on top of the baked travel - a move done on the run instead of from a dead stop.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") bool bCarryEntryVelocity = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment", meta = (ClampMin = "0.01")) float EntryDecayTime = 0.15f;
};

// The result of UFootballActionComponent::CalculateAlignmentTarget (spec §120), named and shaped like a Motion Warping
// target (§16) so the warp can take over from the interim capsule move later.
USTRUCT(BlueprintType)
struct FActionAlignmentTarget {
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Alignment") FName WarpTargetName;
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") int32 ContactIndex = INDEX_NONE;
	// The clamped correction puts the ball within the contact's tolerance.
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") bool bReachable = false;
	// Where the capsule's ground point should be, and the facing, at the contact (world).
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") FVector Location = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") FRotator Rotation = FRotator::ZeroRotator;
	// The warp window (montage seconds).
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") float WindowStart = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") float WindowEnd = 0.f;
	// The capsule move over the window (world, cm) and the turn folded into the start (yaw degrees, + = clockwise).
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") FVector PositionCorrection = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") float RotationCorrection = 0.f;
	// The read-only prediction (§72) it worked from: the ball at the contact, and how long until then (s).
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") FVector PredictedBall = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") float TimeToContact = 0.f;
	// Ball to expected spot before any correction, and what is left after the clamped one (cm).
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") float RequiredCorrection = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Alignment") float Residual = 0.f;
};

// Input of UFootballActionComponent::CalculateAlignmentTarget: everything it needs, so it runs without a world.
USTRUCT(BlueprintType)
struct FAlignmentQuery {
	GENERATED_BODY()

	// The ball at the contact (predicted, §72) and the capsule's ground point then without any correction, from the baked
	// travel; EntryOffset is the entry momentum's share of that move, which keeps its real direction (a start turn doesn't
	// swing it).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FVector PredictedBall = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FVector PredictedRoot = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FVector EntryOffset = FVector::ZeroVector;
	// The capsule's ground point now: a start turn swings the whole move about it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FVector Pivot = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FSkillBasis Basis;
	// The contact's ExpectedBallOffset (§71) and ContactTolerance.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FVector ExpectedBallOffset = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") float Tolerance = 6.f;
	// Real seconds until the contact (the warp window's length), and the window itself in montage seconds.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") float TimeAvailable = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") float WindowStart = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") float WindowEnd = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") int32 ContactIndex = 0;
	// Off for a re-alignment in the middle of an action (the turn is only set at its start).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") bool bAllowRotation = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Alignment") FActionAlignmentProfile Profile;
};

// Spec §65: which interruptions end this action early (Cancelled and InvalidState always do). With the Superseded bit a
// higher-priority request may take over (§64).
USTRUCT(BlueprintType)
struct FActionInterruptionProfile {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interruption", meta = (Bitmask, BitmaskEnum = "/Script/FootballArena.EActionInterruptReason"))
	int32 AllowedReasons = 0x7F;
};

// After the action: for RecoveryTime seconds a new request of the same or lower priority waits (buffered), and the normal
// control (the dribble) takes the ball back after ResumeControlDelay seconds (0 = at once, as before master plan Phase 5).
USTRUCT(BlueprintType)
struct FActionRecoveryProfile {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recovery", meta = (ClampMin = "0")) float RecoveryTime = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recovery", meta = (ClampMin = "0")) float ResumeControlDelay = 0.f;
};

// Spec §14 / §105: scales on top of each contact's own assist values, and whether the player's skill scales them through
// UBallAssist::ScaleForSkill. Read from master plan Phase 6 on (the converted Elastico is the first user).
USTRUCT(BlueprintType)
struct FActionAssistProfile {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0")) float StrengthScale = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0")) float BoundsScale = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist") bool bScaleWithPlayerSkill = true;
};

// Spec §62: which of the player's attributes (FFootballAttributes property names) drive this action's assist and its
// contact quality. Read from master plan Phase 6 on.
USTRUCT(BlueprintType)
struct FActionAttributeProfile {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attributes") FName AssistAttribute = TEXT("SkillMoves");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attributes") FName QualityAttribute = TEXT("BallControl");
};

// Spec §122: scales on top of the body region's surface values (DT_BodyRegions) for this action's raw responses, and the
// body's effective contact strength (FBallResponseInput). Read from master plan Phase 6 on (the raw touches).
USTRUCT(BlueprintType)
struct FActionBallResponseProfile {
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response", meta = (ClampMin = "0")) float RestitutionScale = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response", meta = (ClampMin = "0")) float FrictionScale = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Response", meta = (ClampMin = "0", ClampMax = "1")) float ContactStrength = 1.f;
};

// What the validators (spec §119) look at, gathered by UFootballActionComponent so the rules themselves (ValidateAction)
// run without a world in the self-test.
USTRUCT(BlueprintType)
struct FActionValidationInput {
	GENERATED_BODY()

	// ValidateState: the player can act (on the ground, not stunned); the running action's priority (-1 = none) and whether
	// it is protected from being taken over (no Superseded bit); still recovering from an action of this priority (-1 = no).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bPlayerCanAct = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") int32 ActivePriority = -1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bActiveProtected = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") int32 RecoveringPriority = -1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") float CooldownLeft = 0.f;
	// ValidateBall / Ownership / Distance: a simulated ball with BallMovement + BallState; its state machine allows the
	// action's ball state; another character controls or receives it; its ground distance to the player (cm).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bBallValid = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bBallStateAllows = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bBallOwnedByOther = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") float BallDistance = 0.f;
	// ValidateDirection: the direction mode found a direction. ValidateSpeed: the player's speed (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bHasDirection = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") float PlayerSpeed = 0.f;
	// ValidateReachability (§72-73) and ValidateCollisionSpace.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bReachable = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Validation") bool bSpaceFree = true;
};

// One recorded tick of an action (spec §104), read back by tests and debugging.
USTRUCT(BlueprintType)
struct FActionRecordSample {
	GENERATED_BODY()

	// Seconds since the action started.
	UPROPERTY(BlueprintReadOnly, Category = "Action") float Time = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FGameplayTag ActionTag;
	UPROPERTY(BlueprintReadOnly, Category = "Action") EActionState State = EActionState::None;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FTransform PlayerTransform;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector PlayerVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FTransform BallTransform;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector BallVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector BallAngularVelocity = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") int32 ContactIndex = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Action") EContactValidation ContactValidity = EContactValidation::Valid;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector ContactPoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector ContactNormal = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector BodyVelocity = FVector::ZeroVector;
	// On a touch's tick: the raw physical result, the assisted one, and the impulse finally given (kg*cm/s).
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector RawResponse = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector AssistedResponse = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector FinalImpulse = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Action") FVector AlignmentCorrection = FVector::ZeroVector;
};
