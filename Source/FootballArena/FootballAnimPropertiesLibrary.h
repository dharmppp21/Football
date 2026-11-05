#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FootballAnimPropertiesLibrary.generated.h"

class APawn;

// The bridge to the Game Animation Sample's motion-matching AnimBP (SandboxCharacter_CMC_ABP, on AFootballCharacter::MotionSource):
// that AnimBP asks its pawn for S_CharacterPropertiesForAnimation through BPI_SandboxCharacter_Pawn::Get_PropertiesForAnimation,
// which BP_FootballPlayer implements with this one call.
UCLASS()
class FOOTBALLARENA_API UFootballAnimPropertiesLibrary : public UBlueprintFunctionLibrary {
	GENERATED_BODY()

public:
	// Fills Properties - the sample's S_CharacterPropertiesForAnimation, matched field by field by name, so it keeps working
	// whatever order the sample keeps them in - from Pawn's movement: transform, velocity, acceleration, its limits, gait
	// (sprint with the top speed raised, walk with the stick only partly pushed, else run), on the ground or in the air,
	// orient to movement, standing. Fields it doesn't know keep their defaults.
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Football|Animation", meta = (CustomStructureParam = "Properties"))
	static void FillSandboxAnimationProperties(APawn* Pawn, int32& Properties);
	DECLARE_FUNCTION(execFillSandboxAnimationProperties);

	static void FillProperties(const APawn* Pawn, const UScriptStruct* Struct, void* Data);
};
