#include "AnimNotifyState_BallContactWindow.h"

FString UAnimNotifyState_BallContactWindow::GetNotifyName_Implementation() const {
	return DebugLabel.IsNone() ? FString::Printf(TEXT("Ball Contact %d"), ContactIndex) : FString::Printf(TEXT("Ball Contact %d %s"), ContactIndex, *DebugLabel.ToString());
}
