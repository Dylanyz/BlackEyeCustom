// SPDX-License-Identifier: Apache-2.0
//
// Shared between the bake loop (BlackEyeFastBake.cpp) and the twin writer (BlackEyeFastBakeTwin.cpp).

#pragma once

#include "CoreMinimal.h"
#include "BlackEyeFastBakeLibrary.h"

class ISequencer;
class ULevelSequence;

namespace BlackEyeFastBake
{
	/** One baked frame: what the Black Eye camera solved, plus its subjects for checking. */
	struct FSample
	{
		double Frame = 0.0;                          // display frame of the baked sequence
		FTransform CameraWorld;                      // camera component world transform (not the actor's)
		float FocalLength = 0.f;
		float FocusDistance = 0.f;
		float Aperture = 0.f;
		FVector Subject = FVector::ZeroVector;       // LookAt Target_0's point: did the bake see the live pose?
		FVector FollowSubject = FVector::ZeroVector; // Follow Target_0's point
	};

	/** What a bake run hands to the twin writer. */
	struct FBakeOutput
	{
		TSharedPtr<ISequencer> Sequencer;
		FGuid CameraBinding;
		TArray<FSample> Samples;
	};

	/** Opens the sequence in Sequencer as root and focused sequence, or explains why not. */
	TSharedPtr<ISequencer> OpenSequencer(ULevelSequence* Sequence, FString& OutError);

	/** Steps and samples one Black Eye camera (DESIGN section 3, steps 1-4 and 6). Restores the editor on return. */
	FBlackEyeFastBakeReport RunBake(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Out);

	/** Writes the samples onto the camera's baked twin, creating it on the first bake (DESIGN section 5). */
	bool WriteTwin(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Bake,
	               FBlackEyeFastBakeReport& Report);

	/** Points camera cuts at the twin (lock) or back at the Black Eye camera. Returns the sections changed. */
	int32 SetLocked(ULevelSequence* Sequence, const FString& CameraBindingName, bool bLocked);

	/** Every baked camera in the sequence. */
	TArray<FBlackEyeBakeInfo> GetBakeInfo(ULevelSequence* Sequence);

	/** The Black Eye Fast Bake submenu on Sequencer's binding right-click menu (BlackEyeFastBakeMenu.cpp). */
	void RegisterMenus();
	void UnregisterMenus();
}
