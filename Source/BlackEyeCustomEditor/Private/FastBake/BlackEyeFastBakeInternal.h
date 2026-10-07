// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Shared between the bake loop (BlackEyeFastBake.cpp) and the twin writer (BlackEyeFastBakeTwin.cpp).

#pragma once

#include "CoreMinimal.h"
#include "BlackEyeFastBakeLibrary.h"

class ISequencer;
class ULevelSequence;
class UMovieScene;

namespace BlackEyeFastBake
{
	/** A Black Eye camera and its baked twin, from the binding tag the twin writer leaves (BlackEyeFastBakeTwin.cpp). */
	struct FTwin
	{
		FGuid Camera;
		FGuid Twin;          // the newest tagged twin whose binding exists; invalid when none does
		TArray<FGuid> Stale; // every other tagged twin: removed, or older. Camera cuts may still point at them
	};

	/** Every baked camera in the sequence. */
	TArray<FTwin> FindTwins(const UMovieScene& MovieScene);

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

	/** Sorted, empty spans dropped, overlapping or touching spans merged. */
	TArray<FBlackEyeBakeRange> NormalizeRanges(TArray<FBlackEyeBakeRange> Ranges);

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

	/** The edit's shots, cameras and used frames plus handles (BlackEyeFastBakeEdit.cpp). */
	TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* Edit, int32 HandleFrames);

	/** The Black Eye Fast Bake submenu on Sequencer's binding right-click menu (BlackEyeFastBakeMenu.cpp). */
	void RegisterMenus();
	void UnregisterMenus();

	/** Bake an edit: Sequencer toolbar and Content Browser entries, the dialog (BlackEyeFastBakeEdit.cpp). */
	void RegisterEditMenus();
	void UnregisterEditMenus();

	/** Shared with BlackEyeFastBakeEdit.cpp (BlackEyeFastBakeMenu.cpp). */
	void Notify(const FText& Text, bool bSuccess);
	void RunNextTick(TFunction<void()> Action);
	void OpenThen(ULevelSequence* Sequence, TFunction<void()> Then);
}
