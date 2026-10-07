// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Shared between the bake loop (BlackEyeFastBake.cpp) and the twin writer (BlackEyeFastBakeTwin.cpp).

#pragma once

#include "CoreMinimal.h"
#include "BlackEyeFastBakeLibrary.h"
#include "Evaluation/MovieSceneTimeTransform.h"
#include "MovieSceneSequenceID.h"

class ACineCameraActor;
class ISequencer;
class ULevelSequence;
class UMovieScene;
class UMovieSceneSubSection;

namespace BlackEyeFastBake
{
	/**
	 * A Black Eye camera and its bake cameras (twins), from the binding tag the twin writer leaves
	 * (BlackEyeFastBakeTwin.cpp). A camera can have several: the Bake Edit window's "Create new".
	 */
	struct FTwin
	{
		FGuid Camera;
		FGuid Twin;          // the newest twin whose binding exists; invalid when none does
		TArray<FGuid> Alive; // every twin whose binding exists, oldest first (Twin is the last)
		TArray<FGuid> Stale; // tagged twins whose binding is gone. Camera cuts may still point at them
	};

	/** Every baked camera in the sequence. */
	TArray<FTwin> FindTwins(const UMovieScene& MovieScene);

	/** The twin one of the camera's cuts plays, else the newest. */
	FGuid DefaultTwin(const UMovieScene& MovieScene, const FTwin& Twins);

	/**
	 * Points a shot's camera cuts for one Black Eye camera. Lock: cuts on the camera or its other twins play Twin. Unlock:
	 * cuts on any of its twins play the camera. Span (shot ticks) limits it, splitting cuts at its edges; All() is the
	 * whole shot. One undo step. Returns the cut sections changed (BlackEyeFastBakeTwin.cpp).
	 */
	int32 SetShotCameraLock(ULevelSequence* Shot, const FGuid& Camera, const FGuid& Twin, bool bLock, const TRange<FFrameNumber>& Span);

	/** What a shot's cuts for one camera play within Span: "live Black Eye", a twin's name, or "mixed". */
	FString DescribeCutPlay(const UMovieScene& MovieScene, const FGuid& Camera, const TRange<FFrameNumber>& Span);

	/** A Black Eye camera bound in a shot, for the Bake Edit window's shot controls (BlackEyeFastBakeEdit.cpp). */
	struct FShotCamera
	{
		FGuid Camera;
		FString Name;
		bool bOnCut = false;      // one of the shot's camera cuts plays it (or its twin)
		TArray<FGuid> Twins;      // its bake cameras, oldest first
		FGuid DefaultTwin;        // the one its cuts play, else the newest; invalid: none yet
	};
	TArray<FShotCamera> ListShotCameras(ULevelSequence* Shot);

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
		TWeakObjectPtr<ACineCameraActor> Camera; // the live camera last stepped: the setup of a possessable one
	};

	/**
	 * Where a bake steps its shot. Shot by shot: the shot is Sequencer's root. From the master: the master is root, and
	 * the shot is reached as one of its sub-sequence instances, evaluated alone through the root instance's
	 * OverrideRootSequence (BlackEyeFastBakeMaster.cpp).
	 */
	struct FBakeTarget
	{
		TSharedPtr<ISequencer> Sequencer;
		FMovieSceneSequenceID SequenceID = MovieSceneSequenceID::Root; // the shot's instance
		FMovieSceneTimeTransform ShotToRoot;                           // shot ticks to root ticks
		bool bRestoreView = true; // put the playhead back and re-evaluate when done (the master batch does it once)
	};

	/** Sorted, empty spans dropped, overlapping spans merged, and touching ones too unless bMergeTouching is false. */
	TArray<FBlackEyeBakeRange> NormalizeRanges(TArray<FBlackEyeBakeRange> Ranges, bool bMergeTouching = true);

	/** Opens the sequence in Sequencer as root and focused sequence, or explains why not. */
	TSharedPtr<ISequencer> OpenSequencer(ULevelSequence* Sequence, FString& OutError);

	/** Steps and samples one Black Eye camera (DESIGN section 3, steps 1-4 and 6). Restores the editor on return. */
	FBlackEyeFastBakeReport RunBake(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Out);

	/** RunBake on a shot reached through Target, which must already be evaluating it (Sequencer open, override set). */
	FBlackEyeFastBakeReport RunBakeIn(const FBakeTarget& Target, ULevelSequence* Shot, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Out);

	/** Writes the samples onto the camera's baked twin, creating it on the first bake (DESIGN section 5). */
	bool WriteTwin(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Bake,
	               FBlackEyeFastBakeReport& Report);

	/**
	 * WriteTwin as data, with no spawned twin and no Sequencer focus: the setup comes from Scratch, a transient camera
	 * reused for every twin of a batch. Sequencer, if given, is used only to respawn twins whose setup changed. Opens no
	 * transaction; the caller wraps a batch in one (DESIGN section 6, "Bake Edit from the master").
	 */
	bool WriteTwinDirect(ULevelSequence* Shot, const FBlackEyeFastBakeOptions& Options, const FBakeOutput& Bake, ACineCameraActor* Scratch,
	                     ISequencer* Sequencer, FBlackEyeFastBakeReport& Report);

	/** Points camera cuts at the twin (lock) or back at the Black Eye camera. Returns the sections changed. */
	int32 SetLocked(ULevelSequence* Sequence, const FString& CameraBindingName, bool bLocked);

	/** Every baked camera in the sequence. */
	TArray<FBlackEyeBakeInfo> GetBakeInfo(ULevelSequence* Sequence);

	/** The edit's shots, cameras and used frames plus handles; Only limits it to those sections (BlackEyeFastBakeEdit.cpp). */
	TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* Edit, int32 HandleFrames, TConstArrayView<const UMovieSceneSubSection*> Only = {});

	/**
	 * GetEditBakePlan with handles per side. bSettleAtCut (Bake Edit from the master, "Like a render"): every span the
	 * edit shows stays its own range, starting on its cut, so each can open with a snap; tail handles stop at the next
	 * span's start. Pass with FBlackEyeFastBakeOptions::bSettleEachRange.
	 */
	TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* Edit, int32 HeadFrames, int32 TailFrames, bool bSettleAtCut,
	                                              TConstArrayView<const UMovieSceneSubSection*> Only);

	/** Bake Edit from the master: every plan's shot baked in the open master's Sequencer (BlackEyeFastBakeMaster.cpp). */
	struct FMasterBakeSettings
	{
		bool bWriteDirect = true; // twin written as data; false: Sequencer steps into each shot to write it
		FBlackEyeFastBakeOptions Options; // per camera: ranges and name are filled from each plan
	};
	struct FMasterBakeResult
	{
		int32 Baked = 0;
		int32 KeyedFrames = 0;
		double Seconds = 0.0;
		TArray<FString> Failures;
		bool bCancelled = false;
	};
	void RunMasterBake(TSharedRef<ISequencer> Sequencer, TArray<FBlackEyeShotBakePlan> Plans, const FMasterBakeSettings& Settings,
	                   TFunction<void(const FMasterBakeResult&)> Done);

	/** A transient CineCamera in the editor world for WriteTwinDirect, outside undo and the outliner. */
	ACineCameraActor* SpawnScratchCamera();
	void DestroyScratchCamera(ACineCameraActor* Scratch);

	/** Locks a twin per Options (all cuts, or only the baked spans), and moves cuts off stale twins (BlackEyeFastBakeTwin.cpp). */
	void ApplyLock(UMovieScene& MovieScene, const FGuid& Camera, const FGuid& Twin, TConstArrayView<FGuid> Stale,
	               const FBlackEyeFastBakeOptions& Options, const TArray<FSample>& Samples);

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
