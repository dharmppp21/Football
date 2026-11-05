#include "FootballAnimPropertiesLibrary.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"

// Gait: a sprint once the top speed is raised to at least SprintGaitMaxSpeed (cm/s, the sprint button), a walk while the stick
// asks for less than WalkGaitAskedSpeed. Fixed numbers; read them from the attributes if gaits get tuned per player.
static constexpr float SprintGaitMaxSpeed = 550.f;
static constexpr float WalkGaitAskedSpeed = 250.f;

namespace {

enum class EGaitFact : uint8 { Walk, Run, Sprint };

struct FAnimFacts {
	FTransform ActorTransform;
	FRotator Aim = FRotator::ZeroRotator;
	FRotator OrientationIntent = FRotator::ZeroRotator;
	FVector Velocity = FVector::ZeroVector;
	FVector InputAcceleration = FVector::ZeroVector;
	FVector GroundLocation = FVector::ZeroVector;
	FVector GroundNormal = FVector::UpVector;
	float MaxAcceleration = 0.f;
	float MaxDeceleration = 0.f;
	EGaitFact Gait = EGaitFact::Run;
	bool bOnGround = true;
};

// Sets an enum field (a user-defined enum's byte, or an enum property) to the first entry whose display or short name is one of Names.
bool SetEnum(FProperty* Property, void* Value, std::initializer_list<const TCHAR*> Names) {
	UEnum* Enum = nullptr;
	FNumericProperty* Underlying = nullptr;
	if (FByteProperty* Byte = CastField<FByteProperty>(Property)) {
		Enum = Byte->Enum;
		Underlying = Byte;
	} else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property)) {
		Enum = EnumProperty->GetEnum();
		Underlying = EnumProperty->GetUnderlyingProperty();
	}
	if (!Enum || !Underlying) {
		return false;
	}
	for (int32 i = 0; i < Enum->NumEnums(); ++i) {
		const FString Display = Enum->GetDisplayNameTextByIndex(i).ToString(), Short = Enum->GetNameStringByIndex(i);
		for (const TCHAR* Want : Names) {
			if (Display.Equals(Want, ESearchCase::IgnoreCase) || Short.Equals(Want, ESearchCase::IgnoreCase)) {
				Underlying->SetIntPropertyValue(Value, Enum->GetValueByIndex(i));
				return true;
			}
		}
	}
	return false;
}

void Fill(const FAnimFacts& F, const UStruct* Struct, void* Data) {
	for (TFieldIterator<FProperty> It(Struct); It; ++It) {
		FProperty* Property = *It;
		void* Value = Property->ContainerPtrToValuePtr<void>(Data);
		const FString Name = Property->GetAuthoredName();
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property)) {
			if (StructProperty->Struct == TBaseStructure<FVector>::Get()) {
				FVector& Out = *static_cast<FVector*>(Value);
				if (Name == TEXT("Velocity")) {
					Out = F.Velocity;
				} else if (Name == TEXT("InputAcceleration")) {
					Out = F.InputAcceleration;
				} else if (Name == TEXT("GroundLocation")) {
					Out = F.GroundLocation;
				} else if (Name == TEXT("GroundNormal")) {
					Out = F.GroundNormal;
				} else if (Name == TEXT("LandVelocity")) {
					Out = FVector::ZeroVector;
				} else if (Name == TEXT("OrientationIntent")) {
					Out = F.OrientationIntent.Vector();
				}
			} else if (StructProperty->Struct == TBaseStructure<FRotator>::Get()) {
				FRotator& Out = *static_cast<FRotator*>(Value);
				if (Name == TEXT("AimingRotation")) {
					Out = F.Aim;
				} else if (Name == TEXT("OrientationIntent")) {
					Out = F.OrientationIntent;
				}
			} else if (StructProperty->Struct == TBaseStructure<FTransform>::Get()) {
				if (Name == TEXT("ActorTransform")) {
					*static_cast<FTransform*>(Value) = F.ActorTransform;
				}
			} else {
				Fill(F, StructProperty->Struct, Value); // a nested struct: the input state
			}
		} else if (FNumericProperty* Number = CastField<FNumericProperty>(Property); Number && Number->IsFloatingPoint()) {
			if (Name == TEXT("CurrentMaxAcceleration")) {
				Number->SetFloatingPointPropertyValue(Value, double(F.MaxAcceleration));
			} else if (Name == TEXT("CurrentMaxDeceleration")) {
				Number->SetFloatingPointPropertyValue(Value, double(F.MaxDeceleration));
			}
		} else if (FBoolProperty* Bool = CastField<FBoolProperty>(Property)) {
			if (Name == TEXT("WantsToSprint")) {
				Bool->SetPropertyValue(Value, F.Gait == EGaitFact::Sprint);
			} else if (Name == TEXT("WantsToWalk")) {
				Bool->SetPropertyValue(Value, F.Gait == EGaitFact::Walk);
			} else if (Name.StartsWith(TEXT("WantsTo")) || Name == TEXT("JustLanded")) {
				Bool->SetPropertyValue(Value, false); // no crouch, strafe or aim in football
			}
		} else if (Name == TEXT("Gait")) {
			SetEnum(Property, Value, {F.Gait == EGaitFact::Sprint ? TEXT("Sprint") : F.Gait == EGaitFact::Walk ? TEXT("Walk") : TEXT("Run")});
		} else if (Name == TEXT("MovementMode")) {
			if (F.bOnGround) {
				SetEnum(Property, Value, {TEXT("OnGround"), TEXT("On Ground"), TEXT("Grounded"), TEXT("Walking")});
			} else {
				SetEnum(Property, Value, {TEXT("InAir"), TEXT("In Air"), TEXT("Falling")});
			}
		} else if (Name == TEXT("RotationMode")) {
			SetEnum(Property, Value, {TEXT("OrientToMovement"), TEXT("Orient To Movement"), TEXT("VelocityDirection")});
		} else if (Name == TEXT("Stance")) {
			SetEnum(Property, Value, {TEXT("Stand"), TEXT("Standing")});
		}
	}
}

} // namespace

void UFootballAnimPropertiesLibrary::FillProperties(const APawn* Pawn, const UScriptStruct* Struct, void* Data) {
	if (!Pawn || !Struct || !Data) {
		return;
	}
	FAnimFacts F;
	F.ActorTransform = Pawn->GetActorTransform();
	F.Aim = Pawn->GetControlRotation();
	F.Velocity = Pawn->GetVelocity();
	F.OrientationIntent = Pawn->GetActorRotation();
	F.GroundLocation = Pawn->GetActorLocation();
	const ACharacter* Character = Cast<ACharacter>(Pawn);
	if (const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr) {
		F.InputAcceleration = Movement->GetCurrentAcceleration();
		F.MaxAcceleration = Movement->GetMaxAcceleration();
		F.MaxDeceleration = Movement->GetMaxBrakingDeceleration();
		F.bOnGround = !Movement->IsFalling();
		if (Movement->CurrentFloor.IsWalkableFloor()) {
			F.GroundNormal = Movement->CurrentFloor.HitResult.ImpactNormal;
		}
		F.GroundLocation.Z -= Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		// The stick's share of the top speed (the movement scales the acceleration by it).
		const float Asked = F.MaxAcceleration > 0.f ? float(F.InputAcceleration.Size2D()) / F.MaxAcceleration * Movement->MaxWalkSpeed : 0.f;
		F.Gait = Movement->MaxWalkSpeed >= SprintGaitMaxSpeed ? EGaitFact::Sprint
			: Asked > 1.f && Asked < WalkGaitAskedSpeed ? EGaitFact::Walk : EGaitFact::Run;
		if (F.InputAcceleration.SizeSquared2D() > 1.0) {
			F.OrientationIntent = F.InputAcceleration.Rotation(); // where he wants to face
		}
	}
	Fill(F, Struct, Data);
}

DEFINE_FUNCTION(UFootballAnimPropertiesLibrary::execFillSandboxAnimationProperties) {
	P_GET_OBJECT(APawn, Pawn);
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* Data = Stack.MostRecentPropertyAddress;
	const FStructProperty* Property = CastField<FStructProperty>(Stack.MostRecentProperty);
	P_FINISH;
	P_NATIVE_BEGIN;
	if (Property && Data) {
		FillProperties(Pawn, Property->Struct, Data);
	}
	P_NATIVE_END;
}
