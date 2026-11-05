#include "MotionBricksComponent.h"
#include "FootballCharacter.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMotionBricks, Log, All);

// motion-bricks.cpp's C ABI (include/motionbricks/motionbricks.h): every call returns an mb_status (0 = MB_OK) and ends with
// an error buffer and its capacity.
namespace {
using FStatus = uint32;
#define MB_FN(Name, ...) using F_##Name = FStatus (*)(__VA_ARGS__, char*, uint64); F_##Name Name = nullptr;
struct FMBApi {
	MB_FN(mb_runtime_options_create, void**)
	MB_FN(mb_runtime_options_set_device, void*, uint32)
	MB_FN(mb_runtime_options_set_threads, void*, uint32)
	MB_FN(mb_runtime_options_set_backend_directory, void*, const char*)
	MB_FN(mb_model_load, const char*, const void*, void**)
	MB_FN(mb_model_get_joint_count, const void*, uint32*)
	MB_FN(mb_model_get_joint_name, const void*, uint32, const char**)
	MB_FN(mb_model_get_joint_parent, const void*, uint32, int32*)
	MB_FN(mb_style_load, const void*, const char*, void**)
	MB_FN(mb_agent_create, const void*, void**)
	MB_FN(mb_agent_reset, void*, const void*)
	MB_FN(mb_agent_plan, void*, const void*, void**)
	MB_FN(mb_agent_advance, void*, uint32)
	MB_FN(mb_command_create, void**)
	MB_FN(mb_command_set_style, void*, const void*)
	MB_FN(mb_command_set_movement_direction, void*, float, float, float)
	MB_FN(mb_command_set_facing_direction, void*, float, float, float)
	MB_FN(mb_command_set_target_speed, void*, float)
	MB_FN(mb_command_set_seed, void*, uint64)
	MB_FN(mb_motion_get_frame_count, const void*, uint64*)
	MB_FN(mb_motion_get_root_translations, const void*, const float**, uint64*)
	MB_FN(mb_motion_get_local_rotations_xyzw, const void*, const float**, uint64*)
	void (*mb_runtime_options_free)(void*) = nullptr;
	void (*mb_model_free)(void*) = nullptr;
	void (*mb_style_free)(void*) = nullptr;
	void (*mb_agent_free)(void*) = nullptr;
	void (*mb_command_free)(void*) = nullptr;
	void (*mb_motion_free)(void*) = nullptr;
};
#undef MB_FN
FMBApi Api;  // one process-wide table; fine for one test pawn, a per-instance table if several load different DLLs

// Runs one call and logs its error text; true on MB_OK.
template <typename F, typename... A>
bool Call(const TCHAR* What, F Fn, A... Args) {
	char Error[1024] = {0};
	const FStatus Status = Fn ? Fn(Args..., Error, sizeof(Error)) : 9;
	if (Status != 0) {
		UE_LOG(LogMotionBricks, Error, TEXT("%s failed (%u): %s"), What, Status, UTF8_TO_TCHAR(Error));
	}
	return Status == 0;
}
}  // namespace

UMotionBricksComponent::UMotionBricksComponent() {
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

bool UMotionBricksComponent::Load() {
	FPlatformProcess::PushDllDirectory(*LibraryDirectory);
	Dll = FPlatformProcess::GetDllHandle(*FPaths::Combine(LibraryDirectory, TEXT("motionbricks.dll")));
	FPlatformProcess::PopDllDirectory(*LibraryDirectory);
	if (!Dll) {
		UE_LOG(LogMotionBricks, Error, TEXT("motionbricks.dll not found in %s"), *LibraryDirectory);
		return false;
	}
#define MB_GET(Name) Api.Name = reinterpret_cast<decltype(Api.Name)>(FPlatformProcess::GetDllExport(Dll, TEXT(#Name)));
	MB_GET(mb_runtime_options_create) MB_GET(mb_runtime_options_set_device) MB_GET(mb_runtime_options_set_threads)
	MB_GET(mb_runtime_options_set_backend_directory) MB_GET(mb_model_load) MB_GET(mb_model_get_joint_count)
	MB_GET(mb_model_get_joint_name) MB_GET(mb_model_get_joint_parent) MB_GET(mb_style_load) MB_GET(mb_agent_create)
	MB_GET(mb_agent_reset) MB_GET(mb_agent_plan) MB_GET(mb_agent_advance) MB_GET(mb_command_create) MB_GET(mb_command_set_style)
	MB_GET(mb_command_set_movement_direction) MB_GET(mb_command_set_facing_direction) MB_GET(mb_command_set_target_speed)
	MB_GET(mb_command_set_seed) MB_GET(mb_motion_get_frame_count) MB_GET(mb_motion_get_root_translations)
	MB_GET(mb_motion_get_local_rotations_xyzw) MB_GET(mb_runtime_options_free) MB_GET(mb_model_free) MB_GET(mb_style_free)
	MB_GET(mb_agent_free) MB_GET(mb_command_free) MB_GET(mb_motion_free)
#undef MB_GET
	const FTCHARToUTF8 Lib(*LibraryDirectory), ModelDir(*ModelDirectory), StyleFile(*StylePath);
	if (!Call(TEXT("options"), Api.mb_runtime_options_create, &Options)
		|| !Call(TEXT("device"), Api.mb_runtime_options_set_device, Options, uint32(1))  // CPU
		|| !Call(TEXT("threads"), Api.mb_runtime_options_set_threads, Options, uint32(Threads))
		|| !Call(TEXT("backends"), Api.mb_runtime_options_set_backend_directory, Options, Lib.Get())
		|| !Call(TEXT("model"), Api.mb_model_load, ModelDir.Get(), static_cast<const void*>(Options), &Model)
		|| !Call(TEXT("style"), Api.mb_style_load, static_cast<const void*>(Model), StyleFile.Get(), &Style)
		|| !Call(TEXT("agent"), Api.mb_agent_create, static_cast<const void*>(Model), &Agent)
		|| !Call(TEXT("reset"), Api.mb_agent_reset, Agent, static_cast<const void*>(Style))
		|| !Call(TEXT("command"), Api.mb_command_create, &Command)
		|| !Call(TEXT("command style"), Api.mb_command_set_style, Command, static_cast<const void*>(Style))) {
		return false;
	}
	uint32 Count = 0;
	Call(TEXT("joints"), Api.mb_model_get_joint_count, static_cast<const void*>(Model), &Count);
	JointCount = int32(Count);
	for (uint32 J = 0; J < Count; ++J) {
		const char* Name = nullptr;
		int32 Parent = -1;
		Call(TEXT("joint name"), Api.mb_model_get_joint_name, static_cast<const void*>(Model), J, &Name);
		Call(TEXT("joint parent"), Api.mb_model_get_joint_parent, static_cast<const void*>(Model), J, &Parent);
		JointNames.Add(FName(UTF8_TO_TCHAR(Name ? Name : "")));
		JointParents.Add(Parent);
	}
	UE_LOG(LogMotionBricks, Log, TEXT("MotionBricks loaded: %d joints, style %s"), JointCount, *StylePath);
	return JointCount > 0;
}

void UMotionBricksComponent::Unload() {
	if (Pending.IsValid()) {
		Pending.Wait();  // the worker owns the agent until it returns
		Pending = TFuture<FChunk>();
	}
	if (Api.mb_command_free && Command) Api.mb_command_free(Command);
	if (Api.mb_agent_free && Agent) Api.mb_agent_free(Agent);
	if (Api.mb_style_free && Style) Api.mb_style_free(Style);
	if (Api.mb_model_free && Model) Api.mb_model_free(Model);
	if (Api.mb_runtime_options_free && Options) Api.mb_runtime_options_free(Options);
	Command = Agent = Style = Model = Options = nullptr;
	if (Dll) {
		FPlatformProcess::FreeDllHandle(Dll);
		Dll = nullptr;
	}
}

void UMotionBricksComponent::BeginPlay() {
	Super::BeginPlay();
	if (!Load()) {
		Unload();
		return;
	}
	// The proxy's pose this frame comes from this tick.
	if (const AFootballCharacter* Character = Cast<AFootballCharacter>(GetOwner()); Character && Character->MotionSource) {
		Character->MotionSource->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
	}
	StartPlan();
}

void UMotionBricksComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
	Unload();
	Super::EndPlay(EndPlayReason);
}

void UMotionBricksComponent::StartPlan() {
	// UE world -> MotionBricks world swaps Y and Z (MotionBricks: X left, Y up, Z forward; the same map as the G1 proxy's mesh
	// space), so a ground direction (X, Y) becomes (X, 0, Y).
	const AActor* Owner = GetOwner();
	const FVector Velocity = Owner ? Owner->GetVelocity() : FVector::ZeroVector;
	const FVector Forward = Owner ? Owner->GetActorForwardVector() : FVector::ForwardVector;
	const float Speed = float(Velocity.Size2D()) * 0.01f * SpeedScale;
	const FVector2D Face = FVector2D(Forward.X, Forward.Y).GetSafeNormal();
	const FVector2D Move = Speed > 0.05f ? FVector2D(Velocity.X, Velocity.Y).GetSafeNormal() : Face;
	const int32 Frames = ReplanFrames, Joints = JointCount;
	void* AgentPtr = Agent;
	void* CommandPtr = Command;
	Pending = Async(EAsyncExecution::ThreadPool, [AgentPtr, CommandPtr, Move, Face, Speed, Frames, Joints]() {
		FChunk Chunk;
		const double Start = FPlatformTime::Seconds();
		void* Motion = nullptr;
		if (Call(TEXT("move"), Api.mb_command_set_movement_direction, CommandPtr, float(Move.X), 0.f, float(Move.Y))
			&& Call(TEXT("face"), Api.mb_command_set_facing_direction, CommandPtr, float(Face.X), 0.f, float(Face.Y))
			&& Call(TEXT("speed"), Api.mb_command_set_target_speed, CommandPtr, Speed)
			&& Call(TEXT("plan"), Api.mb_agent_plan, AgentPtr, static_cast<const void*>(CommandPtr), &Motion)) {
			uint64 Count = 0, Values = 0;
			const float* Root = nullptr;
			const float* Rot = nullptr;
			Call(TEXT("frames"), Api.mb_motion_get_frame_count, static_cast<const void*>(Motion), &Count);
			Call(TEXT("root"), Api.mb_motion_get_root_translations, static_cast<const void*>(Motion), &Root, &Values);
			Call(TEXT("rotations"), Api.mb_motion_get_local_rotations_xyzw, static_cast<const void*>(Motion), &Rot, &Values);
			Chunk.Frames = int32(FMath::Min<uint64>(Count, uint64(Frames)));
			if (Root && Rot && Chunk.Frames > 0) {
				Chunk.Root.Append(Root, Chunk.Frames * 3);
				Chunk.Rot.Append(Rot, Chunk.Frames * Joints * 4);
			}
			Api.mb_motion_free(Motion);
			// The agent moves on to where this chunk ends; the next plan starts there.
			Call(TEXT("advance"), Api.mb_agent_advance, AgentPtr, uint32(Chunk.Frames));
		}
		Chunk.Ms = float((FPlatformTime::Seconds() - Start) * 1000.0);
		return Chunk;
	});
}

void UMotionBricksComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) {
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Agent) {
		return;
	}
	if (Pending.IsValid() && Pending.IsReady()) {
		FChunk Chunk = Pending.Get();
		Pending = TFuture<FChunk>();
		LastPlanMs = Chunk.Ms;
		++PlanCount;
		BufRoot.Append(Chunk.Root);
		BufRot.Append(Chunk.Rot);
		BufFrames += Chunk.Frames;
	}
	if (BufFrames == 0) {
		if (!Pending.IsValid()) {
			StartPlan();
		}
		return;
	}
	Playhead = FMath::Min(Playhead + DeltaTime * 30.0, double(BufFrames - 1));
	// Drop played frames, keeping the one before the playhead for interpolation.
	if (const int32 Drop = FMath::FloorToInt32(Playhead) - 1; Drop > 0) {
		BufRoot.RemoveAt(0, Drop * 3, EAllowShrinking::No);
		BufRot.RemoveAt(0, Drop * JointCount * 4, EAllowShrinking::No);
		BufFrames -= Drop;
		Playhead -= Drop;
	}
	if (!Pending.IsValid() && BufFrames - Playhead < ReplanFrames) {
		StartPlan();
	}
	// Pose at the playhead (30 fps buffer, slerped).
	const int32 I0 = FMath::FloorToInt32(Playhead), I1 = FMath::Min(I0 + 1, BufFrames - 1);
	const float A = float(Playhead - I0);
	CurLocal.SetNum(JointCount);
	for (int32 J = 0; J < JointCount; ++J) {
		const float* Q0 = &BufRot[(I0 * JointCount + J) * 4];
		const float* Q1 = &BufRot[(I1 * JointCount + J) * 4];
		CurLocal[J] = FQuat::Slerp(FQuat(Q0[0], Q0[1], Q0[2], Q0[3]), FQuat(Q1[0], Q1[1], Q1[2], Q1[3]), A).GetNormalized();
	}
	CurRootHeight = FMath::Lerp(BufRoot[I0 * 3 + 1], BufRoot[I1 * 3 + 1], A);
	// Pelvis heading about MotionBricks' up axis, smoothed so the gait's own pelvis sway stays in the pose.
	const FVector Fwd = CurLocal[0].RotateVector(FVector(0, 0, 1));
	const float Heading = FMath::Atan2(float(Fwd.X), float(Fwd.Z));
	if (!bHeadingInit) {
		CurHeading = Heading;
		bHeadingInit = true;
	} else {
		CurHeading += FMath::FindDeltaAngleRadians(CurHeading, Heading) * (1.f - FMath::Exp(-DeltaTime / HeadingSmoothing));
	}
	bHasPose = true;
}

bool UMotionBricksComponent::GetPose(TArray<FQuat>& OutLocal, float& OutRootHeight, float& OutHeading) const {
	if (!bHasPose) {
		return false;
	}
	OutLocal = CurLocal;
	OutRootHeight = CurRootHeight;
	OutHeading = CurHeading;
	return true;
}
