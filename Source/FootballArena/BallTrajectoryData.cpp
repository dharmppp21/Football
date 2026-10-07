#include "BallTrajectoryData.h"

static float EvalCurve(const FRuntimeFloatCurve& Curve, float T) {
	const FRichCurve* Rich = Curve.GetRichCurveConst();
	return Rich ? Rich->Eval(T) : 0.f;
}

FVector UBallTrajectoryData::Evaluate(float T) const {
	const FRichCurve* Speed = SpeedCurve.GetRichCurveConst();
	if (Speed && Speed->GetNumKeys() > 0) {
		T = Speed->Eval(T);
	}
	return FVector(EvalCurve(ForwardCurve, T) * ForwardAmount, EvalCurve(LateralCurve, T) * LateralAmount, EvalCurve(VerticalCurve, T) * VerticalAmount);
}

void UBallTrajectoryData::RebuildCurvesFromSamples() {
	Modify();
	FRichCurve* Curves[] = { ForwardCurve.GetRichCurve(), LateralCurve.GetRichCurve(), VerticalCurve.GetRichCurve() };
	for (int32 Axis = 0; Axis < 3; ++Axis) {
		FRichCurve& Curve = *Curves[Axis];
		Curve.Reset();
		for (const FBallTrajectorySample& Sample : Samples) {
			const FKeyHandle Key = Curve.AddKey(Sample.T, float(Sample.LocalOffset[Axis]));
			Curve.SetKeyInterpMode(Key, RCIM_Cubic, false);
			Curve.SetKeyTangentMode(Key, RCTM_SmartAuto, false);
		}
		Curve.AutoSetTangents();
	}
	MarkPackageDirty();
}
