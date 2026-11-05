#include "BallPhysicsResponse.h"

// Velocity change of the ball's contact point per unit impulse D applied there: D/m + ((r x D) x r) / I, with the
// sphere's scalar inertia I.
static FVector PointResponse(const FVector& D, const FVector& R, double InvMass, double InvInertia) {
	return D * InvMass + FVector::CrossProduct(FVector::CrossProduct(R, D), R) * InvInertia;
}

FBallResponse UBallPhysicsResponse::ComputeResponse(const FBallResponseInput& In) {
	FBallResponse Out;
	Out.VelocityAfter = In.BallVelocity;
	Out.AngularVelocityAfter = In.BallAngularVelocity;
	const FVector N = In.ContactNormal.GetSafeNormal();
	if (In.Mode == EBallResponseMode::ControlledContact || In.Mode == EBallResponseMode::Capture || N.IsNearlyZero()
		|| In.BallMass <= 0.f || In.BallRadius <= 0.f) {
		return Out;
	}
	const double InvMass = 1.0 / In.BallMass;
	const double InvInertia = 1.0 / (FMath::Max(In.InertiaFactor, 0.01f) * In.BallMass * In.BallRadius * In.BallRadius);
	const FVector R = In.ContactPoint - In.BallCenter;
	// The ball's surface velocity at the contact, relative to the body.
	const FVector Relative = In.BallVelocity + FVector::CrossProduct(In.BallAngularVelocity, R) - In.BodyVelocity;
	const double Approach = -FVector::DotProduct(Relative, N);
	if (Approach <= 0.0) {
		return Out; // separating or sliding along: no impulse
	}
	// A finite body mass M (ContactStrength = M / (M + m)) adds 1/M to the effective inverse mass.
	const double Strength = FMath::Clamp(double(In.ContactStrength), 0.01, 1.0);
	const double InvBodyMass = (1.0 / Strength - 1.0) * InvMass;

	const double NormalImpulse = (1.0 + FMath::Clamp(double(In.Restitution), 0.0, 1.0)) * Approach
		/ (FVector::DotProduct(PointResponse(N, R, InvMass, InvInertia), N) + InvBodyMass);
	// Sideways slip that is left after the normal impulse, and the impulse that would stop it.
	const FVector AfterNormal = Relative + PointResponse(N * NormalImpulse, R, InvMass, InvInertia);
	const FVector Slip = AfterNormal - N * FVector::DotProduct(AfterNormal, N);
	FVector Tangent = FVector::ZeroVector;
	if (!Slip.IsNearlyZero(1e-4)) {
		const FVector T = -Slip.GetSafeNormal();
		const double Stick = Slip.Size() / (FVector::DotProduct(PointResponse(T, R, InvMass, InvInertia), T) + InvBodyMass);
		const double Cap = FMath::Max(double(In.Friction), 0.0) * NormalImpulse;
		Out.bSliding = Stick > Cap;
		Tangent = T * FMath::Min(Stick, Cap);
	}
	Out.bContact = true;
	Out.NormalImpulse = float(NormalImpulse);
	Out.TangentImpulse = Tangent;
	Out.LinearImpulse = N * NormalImpulse + Tangent;
	Out.AngularImpulse = FVector::CrossProduct(R, Out.LinearImpulse);
	Out.VelocityAfter = In.BallVelocity + Out.LinearImpulse * InvMass;
	Out.AngularVelocityAfter = In.BallAngularVelocity + Out.AngularImpulse * InvInertia;
	return Out;
}

FVector UBallPhysicsResponse::RollingVelocity(FVector Velocity, FVector AngularVelocity, float Radius, float InertiaFactor) {
	const double K = FMath::Max(double(InertiaFactor), 0.0);
	const FVector Rolling = (Velocity - FVector::CrossProduct(FVector::UpVector, AngularVelocity) * (K * Radius)) / (1.0 + K);
	return FVector(Rolling.X, Rolling.Y, 0.0);
}
