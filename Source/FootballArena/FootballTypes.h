#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
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

// Feet a skill may be done with (doc §10 AllowedFeet). A skill's Montage is its right-foot version and its
// MirroredMontage the left-foot one.
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

	// The intended exit, in skill space: an INTENT, never a mandatory trajectory (§13). A release's authored exit
	// (with the Exit fields below).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Intent")
	FVector PreferredDirection = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Intent")
	float PreferredSpeed = 300.f;

	// Bounded assistance (§14): the share of the gap between the physical result and the intent that is closed
	// (0.10-0.25 realistic, above 0.40 only for arcade-like actions), and the most it may turn the ball (degrees) or
	// change its speed (cm/s); Phase 4 tunes the bounds. The frozen Elastico still uses AssistStrength the old way
	// (1 = exactly what its baked path needs) until Phase 6.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0", ClampMax = "1"))
	float AssistStrength = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0"))
	float MaxAssistAngleDegrees = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist", meta = (ClampMin = "0"))
	float MaxAssistSpeed = 100.f;

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
	// A spin set outright (deg/s): the legacy skill touch, until master plan Phase 6.
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
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector BallPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector BallVel = FVector::ZeroVector;
	// Where the skill wanted the ball this tick (approach spot, or where the last touch aimed).
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TargetPos = FVector::ZeroVector;
	// Set on the tick of a touch: the collision's velocity, what the skill needed, and what was applied (doc §19–§20).
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchPhysical = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchNeeded = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector TouchApplied = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector ActorPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") float ActorYaw = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector HipsPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector LeftFootPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Football") FVector RightFootPos = FVector::ZeroVector;
};
