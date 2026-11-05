#include "RLPhysicsPlayer.h"

#include "Camera/CameraComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include <mujoco/mujoco.h>
THIRD_PARTY_INCLUDES_END

namespace
{
	constexpr int32 NumBones = 22;
	constexpr int32 NumHinges = 53;
	constexpr int32 NumObs = 307;
	constexpr int32 NumRefQ = 7 + NumHinges;
	constexpr int32 NumRefV = 6 + NumHinges;
	constexpr double PhysicsDt = 0.002;  // ball_model.DT: the ball's contacts were fitted at this step
	constexpr int32 SubSteps = 10;       // 50 Hz control
	constexpr double MaxLag = 0.1;       // s of physics one frame may catch up on; after a hitch the kick runs late, unchanged

	const char* const BoneNames[NumBones] = {
		"Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
		"RightShoulder", "RightArm", "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase",
		"RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"};

	// RL_Train/ball_model.py, the port of this game's own ball.
	namespace BallK
	{
		constexpr double R = 0.11, Mass = 0.43, Inertia = 0.0034687, AngDamping = 0.02, AeroK = 0.0228;
		constexpr double CdHigh = 0.187, CdStep = 0.261, CrisisSpeed = 12.91, CrisisWidth = 0.718, DragSpin = 0.5;
		constexpr double ClMax = 0.32, ClSpinScale = 0.13, RollRes = 1.35, ContactHeight = 0.015;
		constexpr double SpinFriction = 2600.0 * UE_DOUBLE_PI / 180.0;
	}

	// MuJoCo numbers in UE's math types: only the algebra is shared, so these stay in MuJoCo's axes (w, x, y, z order on disk).
	FQuat MjQuat(const double* Q) { return FQuat(Q[1], Q[2], Q[3], Q[0]); }
	FVector MjVec(const double* V) { return FVector(V[0], V[1], V[2]); }
	FQuat Conj(const FQuat& Q) { return FQuat(-Q.X, -Q.Y, -Q.Z, Q.W); }

	FQuat YawOnly(const FQuat& Q)
	{
		const FVector F = Q.RotateVector(FVector(1, 0, 0));
		const double Yaw = FMath::Atan2(F.Y, F.X);
		return FQuat(0, 0, FMath::Sin(Yaw / 2), FMath::Cos(Yaw / 2));
	}

	// bake_ue.py: MuJoCo (x forward, y left, z up, m) -> UE (x, y swapped, cm), a mirror.
	FVector ToUeLocation(const double* V) { return FVector(V[1], V[0], V[2]) * 100.0; }
	FQuat ToUeRotation(const FQuat& Q) { return FQuat(-Q.Y, -Q.X, -Q.Z, Q.W); }

	struct FReader
	{
		const uint8* P;
		const uint8* End;
		template <typename T>
		void Read(T* Out, int64 N)
		{
			check(P + N * sizeof(T) <= End);
			FMemory::Memcpy(Out, P, N * sizeof(T));
			P += N * sizeof(T);
		}
		int32 Int()
		{
			int32 V;
			Read(&V, 1);
			return V;
		}
	};

	struct FLayer
	{
		int32 In = 0, Out = 0;
		TArray<float> Weights;  // Out values per input, so one layer is a run of a[j] += x * w[j], which vectorises
		TArray<float> Bias;
		TArray<float> Result;
	};
}

// a plain MLP rather than NNE. NNE's editor default is one all-core thread pool shared with MetaHuman, which
// would hide the cost this test measures. Move to NNE when a policy stops being an MLP.
struct FRLRuntime
{
	mjModel* M = nullptr;
	mjData* D = nullptr;
	FString Error;

	// the network
	TArray<float> Mean, Div, Input;
	TArray<FLayer> Layers;

	// the kick
	int32 RefCount = 0, Steps = 0, Side = 0;
	double Target = 0;
	double BallP[3], BallV[3];
	double DefaultPose[NumHinges], W[NumBones][4];
	TArray<float> RefQ, RefV;

	// the model
	double Lo[NumHinges], Hi[NumHinges], Scale[NumHinges], Action[NumHinges];
	int32 BodyId[NumBones], Parent[NumBones], Hips = 0, BallBody = 0, Toes[2], QA = 0, VA = 0, BallGeom = 0, GroundGeom = 0;
	TArray<uint8> GeomKind, GeomBad;  // kind: 0 untracked, 1 left foot, 2 arm, 3 body, 4 right foot

	// the running kick
	int32 Step = 0, Sub = 0;
	double Accum = 0, Wait = 0;
	FVector Origin = FVector::ZeroVector;
	bool bTouch[5];
	int32 FirstKind = 0;
	bool bAny = false, bFootTouched = false, bReleased = false, bFell = false, bReported = false;
	double ReleaseSpeed = 0;

	// what the first play measured
	double TimePhysics = 0, TimePolicy = 0, TimePose = 0, MaxStep = 0;
	int32 CountPhysics = 0, CountPolicy = 0, CountPose = 0;
	TArray<double> Trace;

	~FRLRuntime()
	{
		if (D) mj_deleteData(D);
		if (M) mj_deleteModel(M);
	}

	bool Load(const FString& Dir, const FString& PolicyName, const FString& KickName)
	{
		char Err[1000] = "";
		const FString Xml = Dir / TEXT("footballer.xml");
		M = mj_loadXML(TCHAR_TO_UTF8(*Xml), nullptr, Err, sizeof(Err));
		if (!M)
		{
			Error = FString::Printf(TEXT("%s: %s"), *Xml, UTF8_TO_TCHAR(Err));
			return false;
		}
		M->opt.timestep = PhysicsDt;
		D = mj_makeData(M);
		return LoadPolicy(Dir / (PolicyName + TEXT(".bin"))) && LoadKick(Dir / (KickName + TEXT(".bin"))) && FindIds();
	}

	bool LoadPolicy(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			Error = Path + TEXT(": cannot read");
			return false;
		}
		FReader R{Bytes.GetData(), Bytes.GetData() + Bytes.Num()};
		const int32 In = R.Int(), NumLayers = R.Int();
		if (In != NumObs)
		{
			Error = FString::Printf(TEXT("%s: %d inputs, this loop writes %d"), *Path, In, NumObs);
			return false;
		}
		Mean.SetNumUninitialized(In);
		Div.SetNumUninitialized(In);
		Input.SetNumUninitialized(In);
		R.Read(Mean.GetData(), In);
		R.Read(Div.GetData(), In);
		Layers.SetNum(NumLayers);
		for (FLayer& L : Layers)
		{
			L.In = R.Int();
			L.Out = R.Int();
			L.Weights.SetNumUninitialized(L.In * L.Out);
			L.Bias.SetNumUninitialized(L.Out);
			L.Result.SetNumUninitialized(L.Out);
			R.Read(L.Weights.GetData(), L.In * L.Out);
			R.Read(L.Bias.GetData(), L.Out);
		}
		if (Layers.Last().Out != NumHinges)
		{
			Error = Path + TEXT(": wrong action count");
			return false;
		}
		return true;
	}

	bool LoadKick(const FString& Path)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			Error = Path + TEXT(": cannot read");
			return false;
		}
		FReader R{Bytes.GetData(), Bytes.GetData() + Bytes.Num()};
		RefCount = R.Int();
		Steps = R.Int();
		Side = R.Int();
		R.Read(&Target, 1);
		R.Read(BallP, 3);
		R.Read(BallV, 3);
		R.Read(DefaultPose, NumHinges);
		R.Read(&W[0][0], NumBones * 4);
		RefQ.SetNumUninitialized(RefCount * NumRefQ);
		RefV.SetNumUninitialized(RefCount * NumRefV);
		R.Read(RefQ.GetData(), RefQ.Num());
		R.Read(RefV.GetData(), RefV.Num());
		return true;
	}

	bool FindIds()
	{
		if (M->nu != NumHinges || M->nq < NumRefQ + 7)
		{
			Error = TEXT("footballer.xml: not the 53-hinge body with a ball");
			return false;
		}
		for (int32 I = 0; I < NumHinges; I++)
		{
			const int32 Joint = M->actuator_trnid[2 * I];
			Lo[I] = M->jnt_range[2 * Joint];
			Hi[I] = M->jnt_range[2 * Joint + 1];
			DefaultPose[I] = FMath::Clamp(DefaultPose[I], Lo[I], Hi[I]);
			Scale[I] = FMath::Clamp(0.25 * (Hi[I] - Lo[I]), 0.15, 0.5);
		}
		for (int32 I = 0; I < NumBones; I++)
		{
			BodyId[I] = mj_name2id(M, mjOBJ_BODY, BoneNames[I]);
			if (BodyId[I] < 0)
			{
				Error = FString::Printf(TEXT("footballer.xml has no body %hs"), BoneNames[I]);
				return false;
			}
			Parent[I] = -1;
			for (int32 K = 0; K < I; K++)
			{
				if (BodyId[K] == M->body_parentid[BodyId[I]]) Parent[I] = K;
			}
		}
		Hips = BodyId[0];
		BallBody = mj_name2id(M, mjOBJ_BODY, "ball");
		Toes[0] = mj_name2id(M, mjOBJ_BODY, "LeftToeBase");
		Toes[1] = mj_name2id(M, mjOBJ_BODY, "RightToeBase");
		const int32 BallJoint = mj_name2id(M, mjOBJ_JOINT, "ball");
		QA = M->jnt_qposadr[BallJoint];
		VA = M->jnt_dofadr[BallJoint];
		BallGeom = mj_name2id(M, mjOBJ_GEOM, "ball_g");
		GroundGeom = mj_name2id(M, mjOBJ_GEOM, "ground");
		const auto IsBone = [this](int32 Body, std::initializer_list<const char*> Names)
		{
			for (const char* Name : Names)
			{
				if (mj_name2id(M, mjOBJ_BODY, Name) == Body) return true;
			}
			return false;
		};
		GeomKind.SetNumZeroed(M->ngeom);
		GeomBad.SetNumZeroed(M->ngeom);
		for (int32 G = 0; G < M->ngeom; G++)
		{
			const int32 B = M->geom_bodyid[G];
			if (B == 0 || B == BallBody) continue;
			const bool bLeft = IsBone(B, {"LeftFoot", "LeftToeBase"}), bRight = IsBone(B, {"RightFoot", "RightToeBase"});
			const bool bArm = IsBone(B, {"LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand"});
			GeomKind[G] = bLeft ? 1 : bRight ? 4 : bArm ? 2 : 3;
			GeomBad[G] = !bLeft && !bRight;
		}
		return true;
	}

	void Reset()
	{
		mj_resetData(M, D);
		for (int32 I = 0; I < NumRefQ; I++) D->qpos[I] = RefQ[I];
		for (int32 I = 0; I < NumRefV; I++) D->qvel[I] = RefV[I];
		for (int32 I = 0; I < NumHinges; I++)
		{
			D->ctrl[I] = RefQ[7 + I];
			Action[I] = (RefQ[7 + I] - DefaultPose[I]) / Scale[I];
		}
		for (int32 I = 0; I < 3; I++)
		{
			D->qpos[QA + I] = BallP[I];
			D->qvel[VA + I] = BallV[I];
			D->qvel[VA + 3 + I] = 0;
		}
		D->qpos[QA + 3] = 1;
		D->qpos[QA + 4] = D->qpos[QA + 5] = D->qpos[QA + 6] = 0;
		mj_forward(M, D);
		const double Start[3] = {RefQ[0], RefQ[1], 0};
		Origin = ToUeLocation(Start);  // every kick starts at the actor (bake_ue.py does the same)
		Step = Sub = 0;
		Wait = 0;
		FirstKind = 0;
		bAny = bFootTouched = bReleased = bFell = false;
		ReleaseSpeed = 0;
	}

	void Observe(float* O) const
	{
		const FVector P = MjVec(D->xpos + 3 * Hips);
		const FQuat Q = MjQuat(D->xquat + 4 * Hips);
		const FQuat Inv = Conj(YawOnly(Q));
		const FVector Ang = MjVec(D->cvel + 6 * Hips);
		const FVector Lin = MjVec(D->cvel + 6 * Hips + 3) + FVector::CrossProduct(Ang, P - MjVec(D->subtree_com + 3 * M->body_rootid[Hips]));
		const auto Put = [&O](double V) { *O++ = (float)V; };
		const auto PutV = [&Put](const FVector& V) { Put(V.X); Put(V.Y); Put(V.Z); };
		const auto PutTan = [&PutV](const FQuat& R) { PutV(R.RotateVector(FVector(1, 0, 0))); PutV(R.RotateVector(FVector(0, 0, 1))); };
		Put(P.Z);
		PutTan(Inv * Q);
		PutV(Inv.RotateVector(Lin));
		PutV(Inv.RotateVector(Ang) * 0.25);
		for (int32 I = 0; I < NumHinges; I++) Put(D->qpos[7 + I] - DefaultPose[I]);
		for (int32 I = 0; I < NumHinges; I++) Put(D->qvel[6 + I] * 0.05);
		for (int32 I = 0; I < NumHinges; I++) Put(Action[I]);
		const auto Ahead = [this](int32 N) { return FMath::Min(Step + N, RefCount - 1); };
		const float* NextQ = &RefQ[Ahead(1) * NumRefQ];
		const float* NextV = &RefV[Ahead(1) * NumRefV];
		for (int32 I = 0; I < NumHinges; I++) Put((double)NextQ[7 + I] - DefaultPose[I]);
		for (int32 I = 0; I < NumHinges; I++) Put(NextV[6 + I] * 0.05);
		const auto RefP = [](const float* F) { return FVector(F[0], F[1], F[2]); };
		const auto RefR = [](const float* F) { return FQuat(F[4], F[5], F[6], F[3]); };
		PutV(Inv.RotateVector(RefP(NextQ) - P));
		PutTan(Conj(Q) * RefR(NextQ));
		for (const int32 N : {10, 25})  // 0.2 s and 0.5 s ahead, at any playback rate
		{
			const float* F = &RefQ[Ahead(N) * NumRefQ];
			const FQuat H = Inv * YawOnly(RefR(F));
			PutV(Inv.RotateVector(RefP(F) - P));
			Put(H.W * H.W - H.Z * H.Z);
			Put(2 * H.W * H.Z);
		}
		const FVector Ball = MjVec(D->qpos + QA);
		for (const int32 Toe : Toes)
		{
			const FVector Rel = Inv.RotateVector(Ball - MjVec(D->xpos + 3 * Toe));
			PutV(FVector(FMath::Clamp(Rel.X, -2.0, 2.0), FMath::Clamp(Rel.Y, -2.0, 2.0), FMath::Clamp(Rel.Z, -2.0, 2.0)));
		}
		const FVector Vel = Inv.RotateVector(MjVec(D->qvel + VA)) * 0.1;
		PutV(FVector(FMath::Clamp(Vel.X, -3.0, 3.0), FMath::Clamp(Vel.Y, -3.0, 3.0), FMath::Clamp(Vel.Z, -3.0, 3.0)));
		Put(Target * 0.1);
	}

	const float* RunNetwork()
	{
		float* X = Input.GetData();
		for (int32 I = 0; I < Input.Num(); I++) X[I] = (X[I] - Mean[I]) / Div[I];
		for (int32 LayerIndex = 0; LayerIndex < Layers.Num(); LayerIndex++)
		{
			FLayer& L = Layers[LayerIndex];
			float* Y = L.Result.GetData();
			FMemory::Memcpy(Y, L.Bias.GetData(), L.Out * sizeof(float));
			const float* Row = L.Weights.GetData();
			for (int32 I = 0; I < L.In; I++, Row += L.Out)
			{
				const float V = X[I];
				for (int32 J = 0; J < L.Out; J++) Y[J] += V * Row[J];
			}
			if (LayerIndex + 1 < Layers.Num())
			{
				for (int32 J = 0; J < L.Out; J++) Y[J] = Y[J] > 0 ? Y[J] : FMath::Exp(Y[J]) - 1.0f;  // ELU
			}
			X = Y;
		}
		return X;
	}

	void ApplyBallExtras()  // ball_model.extras: drag and lift in the air, rolling resistance and spin friction on the ground
	{
		using namespace BallK;
		const FVector P = MjVec(D->qpos + QA), V = MjVec(D->qvel + VA);
		const FVector Spin = MjQuat(D->qpos + QA + 3).RotateVector(MjVec(D->qvel + VA + 3));
		const bool bAirborne = P.Z - R > ContactHeight;
		FVector F = FVector::ZeroVector;
		const double Speed = V.Size();
		if (Speed >= 0.01)
		{
			const FVector Dir = V / Speed;
			const FVector Across = Spin - Dir * FVector::DotProduct(Spin, Dir);
			const double Sp = R * Across.Size() / Speed;
			double Cd = CdHigh + CdStep / (1.0 + FMath::Exp((Speed - CrisisSpeed) / CrisisWidth));
			if (bAirborne) Cd += DragSpin * FMath::Min(Sp, 0.4);
			const double Pressure = AeroK * Speed * Speed;
			F = -Dir * (Cd * Pressure);
			const FVector Lift = FVector::CrossProduct(Across, Dir);
			const double N = Lift.Size();
			if (bAirborne && Sp > 0 && N > 1e-9) F += Lift / N * (ClMax * (1.0 - FMath::Exp(-Sp / ClSpinScale)) * Pressure);
		}
		FVector T = -AngDamping * Inertia * Spin;
		if (!bAirborne)
		{
			const double Flat = FMath::Sqrt(V.X * V.X + V.Y * V.Y);
			if (Flat > 1e-9) F -= FVector(V.X, V.Y, 0) / Flat * (Mass * RollRes * FMath::Min(Flat / 0.05, 1.0));
			if (FMath::Abs(Spin.Z) > 1e-9)
			{
				const double Slow = FMath::Min(SpinFriction, FMath::Abs(Spin.Z) / PhysicsDt);
				T.Z += -FMath::Sign(Spin.Z) * Slow * Inertia;
			}
		}
		double* X = D->xfrc_applied + 6 * BallBody;
		X[0] = F.X; X[1] = F.Y; X[2] = F.Z; X[3] = T.X; X[4] = T.Y; X[5] = T.Z;
	}

	void ControlStart()
	{
		const double T0 = FPlatformTime::Seconds();
		if (!bReported) Trace.Append(D->qpos, M->nq);
		Observe(Input.GetData());
		const float* Out = RunNetwork();
		for (int32 I = 0; I < NumHinges; I++)
		{
			Action[I] = FMath::Clamp((double)Out[I], -5.0, 5.0);
			D->ctrl[I] = FMath::Clamp(DefaultPose[I] + Action[I] * Scale[I], Lo[I], Hi[I]);
		}
		FMemory::Memzero(bTouch);
		TimePolicy += FPlatformTime::Seconds() - T0;
		CountPolicy++;
	}

	void ControlEnd()  // as eval_tracker_kick judges a kick: what the ball touched first, its speed when it left the foot, falls
	{
		const double T0 = FPlatformTime::Seconds();
		const bool bRightKick = Side == 1;
		const bool bFoot = bRightKick ? bTouch[4] : bTouch[1], bPlant = bRightKick ? bTouch[1] : bTouch[4];
		const bool bArm = bTouch[2], bBody = bTouch[3];
		if ((bFoot || bPlant || bArm || bBody) && !bAny) FirstKind = bFoot && !bPlant ? 1 : bPlant ? 4 : bArm ? 2 : 3;
		bAny |= bFoot || bPlant || bArm || bBody;
		bFootTouched |= bFoot;
		if (bFootTouched && !bFoot && !bReleased)
		{
			bReleased = true;
			ReleaseSpeed = MjVec(D->qvel + VA).Size();
		}
		for (int32 I = 0; I < D->ncon; I++)
		{
			const mjContact& C = D->contact[I];
			if (C.dist < 0 && ((C.geom[0] == GroundGeom && GeomBad[C.geom[1]]) || (C.geom[1] == GroundGeom && GeomBad[C.geom[0]]))) bFell = true;
		}
		mj_kinematics(M, D);
		mj_comPos(M, D);
		mj_comVel(M, D);
		Step++;
		TimePhysics += FPlatformTime::Seconds() - T0;
	}

	void PhysicsStep()
	{
		const bool bJudged = Step < Steps;
		if (bJudged && Sub == 0) ControlStart();
		const double T0 = FPlatformTime::Seconds();
		ApplyBallExtras();
		mj_step(M, D);
		for (int32 I = 0; I < D->ncon; I++)
		{
			const mjContact& C = D->contact[I];
			if (C.dist < 0 && C.geom[0] == BallGeom) bTouch[GeomKind[C.geom[1]]] = true;
			else if (C.dist < 0 && C.geom[1] == BallGeom) bTouch[GeomKind[C.geom[0]]] = true;
		}
		const double Spent = FPlatformTime::Seconds() - T0;
		TimePhysics += Spent;
		MaxStep = FMath::Max(MaxStep, Spent);
		CountPhysics++;
		if (bJudged && ++Sub == SubSteps)
		{
			Sub = 0;
			ControlEnd();
		}
	}
};

ARLPhysicsPlayer::ARLPhysicsPlayer()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	BodyMesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(RootComponent);
	BodyMesh->SetBoundsScale(4.f);  // the mesh's bounds stay at the actor while the body runs off it
	BallMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BallMesh"));
	BallMesh->SetupAttachment(RootComponent);
	BallMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ViewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ViewCamera"));
	ViewCamera->SetupAttachment(RootComponent);
}

void ARLPhysicsPlayer::BeginPlay()
{
	Super::BeginPlay();
	const FString Dll = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Binaries/Win64/mujoco.dll"));
	if (!FPlatformProcess::GetDllHandle(*Dll))  // delay-loaded: the game runs without MuJoCo until one of these actors plays
	{
		UE_LOG(LogTemp, Error, TEXT("RL player: cannot load %s"), *Dll);
		SetActorTickEnabled(false);
		return;
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Football/Models/Footballer/Footballer_latest"));
	UStaticMesh* BallAsset = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Football/Models/Ball/Football"));
	Runtime = new FRLRuntime;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("RLLive"));
	if (!Mesh || !BallAsset || !Runtime->Load(Dir, Policy, Kick))
	{
		UE_LOG(LogTemp, Error, TEXT("RL player: %s"), Mesh && BallAsset ? *Runtime->Error : TEXT("the Footballer or ball asset is missing"));
		delete Runtime;
		Runtime = nullptr;
		SetActorTickEnabled(false);
		return;
	}
	BodyMesh->SetSkinnedAssetAndUpdate(Mesh);
	BallMesh->SetStaticMesh(BallAsset);
	BallMesh->SetWorldScale3D(FVector(10.9));  // the mesh is 2 cm across (rl_bake_anims.BALL)
	BoneIndex.SetNum(NumBones);
	for (int32 I = 0; I < NumBones; I++)
	{
		BoneIndex[I] = BodyMesh->GetBoneIndex(FName(BoneNames[I]));
		if (BoneIndex[I] == INDEX_NONE)
		{
			UE_LOG(LogTemp, Error, TEXT("RL player: the Footballer has no bone %hs"), BoneNames[I]);
			delete Runtime;
			Runtime = nullptr;
			SetActorTickEnabled(false);
			return;
		}
	}
	Runtime->Reset();
	FrameCamera();
	UpdatePose();
}

void ARLPhysicsPlayer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	delete Runtime;
	Runtime = nullptr;
	Super::EndPlay(EndPlayReason);
}

void ARLPhysicsPlayer::FrameCamera()  // side-on to the kick's run-up, whichever way the clip faces
{
	const FRLRuntime& R = *Runtime;
	const float* Last = &R.RefQ[FMath::Min(R.Steps, R.RefCount - 1) * NumRefQ];
	const double LastD[3] = {Last[0], Last[1], Last[2]};
	const FVector End = ToUeLocation(LastD) - R.Origin;
	FVector Dir = FVector(End.X, End.Y, 0).GetSafeNormal();
	if (Dir.IsNearlyZero()) Dir = FVector::XAxisVector;
	const FVector Mid = FVector(End.X, End.Y, 0) * 0.5 + FVector(0, 0, 90);
	const FVector Pos = Mid + FVector(-Dir.Y, Dir.X, 0) * 650.0 + FVector(0, 0, 60);
	ViewCamera->SetRelativeLocationAndRotation(Pos, (Mid - Pos).Rotation());
}

void ARLPhysicsPlayer::UpdatePose()
{
	const double T0 = FPlatformTime::Seconds();
	FRLRuntime& R = *Runtime;
	FQuat G[NumBones];
	for (int32 I = 0; I < NumBones; I++)
	{
		G[I] = MjQuat(R.D->xquat + 4 * R.BodyId[I]) * MjQuat(R.W[I]);  // bake_ue.to_ue
		const FQuat L = R.Parent[I] >= 0 ? Conj(G[R.Parent[I]]) * G[I] : G[I];
		BodyMesh->BoneSpaceTransforms[BoneIndex[I]].SetRotation(ToUeRotation(L));
	}
	BodyMesh->BoneSpaceTransforms[BoneIndex[0]].SetTranslation(ToUeLocation(R.D->qpos) - R.Origin);
	BodyMesh->RefreshBoneTransforms();
	BallMesh->SetRelativeLocationAndRotation(ToUeLocation(R.D->qpos + R.QA) - R.Origin, ToUeRotation(MjQuat(R.D->qpos + R.QA + 3)));
	R.TimePose += FPlatformTime::Seconds() - T0;
	R.CountPose++;
}

void ARLPhysicsPlayer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Runtime) return;
	FRLRuntime& R = *Runtime;
	if (bTakeCamera)
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC && PC->GetViewTarget() != this) PC->SetViewTarget(this);
	}
	R.Accum = FMath::Min(R.Accum + DeltaSeconds, MaxLag);
	while (R.Accum >= PhysicsDt)
	{
		R.Accum -= PhysicsDt;
		R.PhysicsStep();  // whole physics steps, so the kick does not depend on the frame rate
		if (R.Step == R.Steps && !R.bReported)
		{
			R.bReported = true;
			R.Wait = PauseAtEnd;
			const double Seconds = R.Steps * SubSteps * PhysicsDt;
			const FString Text = FString::Printf(
				TEXT("kick=%s\nkind=%d\nreleased=%d\nball=%.4f\ntarget=%.4f\nfell=%d\nsteps=%d\nphysics_us=%.1f\nphysics_max_us=%.1f\ncontrol_us=%.1f\npose_us=%.1f\ncore_share=%.4f\n"),
				*Kick, R.FirstKind, R.bReleased, R.ReleaseSpeed, R.Target, R.bFell, R.Steps, 1e6 * R.TimePhysics / R.CountPhysics, 1e6 * R.MaxStep,
				1e6 * R.TimePolicy / R.CountPolicy, 1e6 * R.TimePose / FMath::Max(R.CountPose, 1), (R.TimePhysics + R.TimePolicy) / Seconds);
			const FString Out = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RLLive"));
			IFileManager::Get().MakeDirectory(*Out, true);
			FFileHelper::SaveStringToFile(Text, *(Out / (GetName() + TEXT("_") + Kick + TEXT(".txt"))));
			FFileHelper::SaveArrayToFile(TArrayView64<const uint8>((const uint8*)R.Trace.GetData(), R.Trace.Num() * sizeof(double)), *(Out / (GetName() + TEXT("_") + Kick + TEXT(".trace"))));
			R.Trace.Empty();
			UE_LOG(LogTemp, Display, TEXT("RL player %s: first touch %d, ball %.2f m/s (target %.2f), fell %d; %.0f us per physics step, %.0f us per policy step, %.1f %% of a core"),
				*Kick, R.FirstKind, R.ReleaseSpeed, R.Target, R.bFell, 1e6 * R.TimePhysics / R.CountPhysics, 1e6 * R.TimePolicy / R.CountPolicy, 100 * (R.TimePhysics + R.TimePolicy) / Seconds);
		}
		else if (R.Step == R.Steps && R.bReported && R.Wait <= 0)
		{
			R.Wait = PauseAtEnd;
		}
	}
	if (R.Step == R.Steps)
	{
		R.Wait -= DeltaSeconds;
		if (R.Wait <= 0) R.Reset();
	}
	UpdatePose();
}
