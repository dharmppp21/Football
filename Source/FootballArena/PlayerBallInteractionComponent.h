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
// - the dribble controller, ported 1:1 from BP_FootballPlayer.UpdateDribble, plus agile dribbling (ball closer while
//   RB is held). Possession (bHasBall / ControlledBall) is still decided by the Blueprint, which calls UpdateDribble every tick.
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

	// Agile dribbling (FIFA 22: hold RB): the ball is kept at most this far in front of the player (cm, along the facing)
	// instead of at the guide point. The guide point's height and sideways offset are kept. Skills that start close to
	// the feet (elastico) then don't have to pull the ball back.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dribble")
	float AgileGuideDistance = 40.f;

	// Call every tick: dribbles Ball while bHasBall, otherwise lets go of a ball this character was dribbling.
	// Does nothing while the owner's skill works with the ball.
	UFUNCTION(BlueprintCallable, Category = "Dribble")
	void UpdateDribble(bool bHasBall, AActor* Ball);

	// Agile dribbling on/off (the player's BP calls it from the RB button).
	UFUNCTION(BlueprintCallable, Category = "Dribble")
	void SetAgileDribble(bool bAgile) { bAgileDribble = bAgile; }

	UFUNCTION(BlueprintPure, Category = "Dribble")
	bool IsAgileDribble() const { return bAgileDribble; }

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

	UPROPERTY()
	TObjectPtr<USceneComponent> GuidePoint;

	UPROPERTY()
	TObjectPtr<UDataTable> LoadedRegionTable;

	// Indexed by EBodyRegion; points into LoadedRegionTable.
	TArray<const FBodyRegionRow*> RegionRows;

	TWeakObjectPtr<UBallMovementComponent> DribbledBall;
	bool bAgileDribble = false;
};
