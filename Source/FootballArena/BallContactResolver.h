#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballTypes.h"
#include "BallContactResolver.generated.h"

// The reusable contact resolver (doc §6, §7). Stateless: everything comes in through the arguments, so it is
// the same for skills, receiving, deflections and the goalkeeper, and Python can test it without PIE.
UCLASS()
class FOOTBALLARENA_API UBallContactResolver : public UBlueprintFunctionLibrary {
	GENERATED_BODY()
public:
	// Skill space at Origin facing Facing (flattened); Right = Up x Forward.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FSkillBasis MakeSkillBasis(FVector Origin, FVector Facing, bool bMirrored);

	// Skill space from the facing and an input direction (doc §28): the input, relative to the facing, snaps to the
	// nearest direction in AllowedDirections (bits of ESkillDirection); with the Custom bit it is used exactly.
	// An input shorter than 0.3, or no allowed direction, gives Forward. OutDirection = the direction used.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FSkillBasis MakeSkillBasisFromInput(FVector Origin, FVector Facing, FVector InputDirection, int32 AllowedDirections, bool bMirrored, ESkillDirection& OutDirection);

	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FVector SkillToWorld(const FSkillBasis& Basis, FVector Local);

	// Unit direction of a ball region in the basis (zero for Center). Mirroring swaps left and right.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FVector RegionDirection(EBallRegion Region, const FSkillBasis& Basis);

	// Center + RegionDirection * Radius (doc §3).
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FVector BallSurfacePoint(FVector BallCenter, float BallRadius, EBallRegion Region, const FSkillBasis& Basis);

	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static EBallRegion MirrorBallRegion(EBallRegion Region);

	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static EBodyRegion MirrorBodyRegion(EBodyRegion Region);

	// Foot, toe, heel and sole regions (Foot contact priority); everything else is Body.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static bool IsFootRegion(EBodyRegion Region);

	// Runs the doc's five tests on one requested contact and, if valid, works out the response:
	// T3 window open, T1 distance, T4 the ball is on the region's side, T2 the body is closing on the ball,
	// T5 minimum body speed. Always fills the spec §7 relative velocity and its normal / tangent split.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FContactResult ResolveContact(const FContactQuery& Query);

	// Spec §11: the body region's path this tick against the ball, both moving (each one went back by its velocity x
	// DeltaTime): the segment relative to the ball against a sphere of BallRadius + BodyRadius. OutClosestApproach =
	// the smallest gap between the surfaces (cm, <= 0 = touching); OutSweepAlpha = where on the path the touch starts
	// (0 = a tick ago, 1 = now), or the closest point when there is no touch; OutContactPoint = the touched point on
	// the ball at that moment. DeltaTime 0 = a point test at the current positions.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static void SweepRegionToBall(FVector BodyPoint, FVector BodyVelocity, float BodyRadius, FVector BallCenter, FVector BallVelocity, float BallRadius,
		float DeltaTime, float& OutClosestApproach, float& OutSweepAlpha, FVector& OutContactPoint);

	// ---- contact tracker (FContactTracker, spec §10, §99, §100) ----

	// Follows the region: its velocity is (point - last point) / DeltaTime when the same region was tracked last tick,
	// else zero (and PrevPoint = Point).
	UFUNCTION(BlueprintCallable, Category = "Football|Contact")
	static void TrackRegion(UPARAM(ref) FContactTracker& Tracker, EBodyRegion Region, FVector Point, FVector Normal, float DeltaTime);

	// The region stops taking part (no region point, the action ended); the next TrackRegion starts at zero velocity.
	UFUNCTION(BlueprintCallable, Category = "Football|Contact")
	static void StopTracking(UPARAM(ref) FContactTracker& Tracker);

	// A new contact window (§99): its contact can be processed again - after a confirmed separation if the region is
	// still on the ball from a processed contact - and the window's touches and impulse start from zero.
	UFUNCTION(BlueprintCallable, Category = "Football|Contact")
	static void OpenContactWindow(UPARAM(ref) FContactTracker& Tracker);

	// §100: the contact state after this tick's sweep (touching = ClosestApproach <= 0). Off the ball for at least
	// MinSeparationTime is a confirmed separation: it re-arms a window that waits for one, and touching again after it
	// is a recontact, which re-arms a processed contact when the descriptor allows recontact.
	UFUNCTION(BlueprintCallable, Category = "Football|Contact")
	static void UpdateContactState(UPARAM(ref) FContactTracker& Tracker, float ClosestApproach, float DeltaTime, const FBallContactDescriptor& Descriptor,
		float MinSeparationTime = 0.05f);

	// §99 / §100: a one-shot contact can be processed until it has been; a continuous one while the region is on the ball.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static bool CanProcessContact(const FContactTracker& Tracker, const FBallContactDescriptor& Descriptor);

	// After a valid contact (§99).
	UFUNCTION(BlueprintCallable, Category = "Football|Contact")
	static void MarkContactProcessed(UPARAM(ref) FContactTracker& Tracker);

	// Exit velocity (doc §20): skill direction * PreferredSpeed + up * PreferredSpeed * VerticalBias
	// + player velocity * InheritPlayerVelocity + ball velocity * InheritBallVelocity.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FVector ComputeExitVelocity(const FBallContactDescriptor& Descriptor, const FSkillBasis& Basis, FVector PlayerVelocity, FVector BallVelocity);

	// Calculated deflection off a body surface (doc §19). The approach speed along Normal bounces back scaled by
	// Restitution, and the sliding part loses TangentDamping. SpinTransfer turns the sliding speed into spin
	// about Normal x slide. Relative to the body's own velocity. A ball already moving away is left unchanged.
	UFUNCTION(BlueprintPure, Category = "Football|Contact")
	static FVector ComputeDeflection(FVector BallVelocity, FVector BodyVelocity, FVector Normal, float Restitution, float TangentDamping,
		FVector BallSpinDeg, float SpinTransfer, float BallRadius, FVector& OutSpinDeg);
};
