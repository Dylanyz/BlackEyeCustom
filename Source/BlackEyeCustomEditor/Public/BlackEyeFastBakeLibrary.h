// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlackEyeFastBakeLibrary.generated.h"

class ULevelSequence;

/** A span of display frames, end exclusive. */
USTRUCT(BlueprintType)
struct FBlackEyeBakeRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake") int32 StartFrame = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake") int32 EndFrame = 0;
};

/** What one bake should do. Frames are display frames of the sequence being baked. */
USTRUCT(BlueprintType)
struct FBlackEyeFastBakeOptions
{
	GENERATED_BODY()

	/** Binding name of the Black Eye camera in the sequence. Empty: the first Black Eye camera found. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	FString CameraBindingName;

	/** First frame to sample. INDEX_NONE: the playback range start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	int32 StartFrame = INDEX_NONE;

	/** One past the last frame to sample. INDEX_NONE: the playback range end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	int32 EndFrame = INDEX_NONE;

	/**
	 * Bake only these spans (each end exclusive), keyed and nothing between them; StartFrame and EndFrame are then
	 * ignored. Spans closer together than WarmUpFrames are stepped through in one run. GetEditBakePlan fills it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	TArray<FBlackEyeBakeRange> Ranges;

	/** Frames stepped (not sampled) before StartFrame (or before each run of Ranges), after the snap and settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	int32 WarmUpFrames = 0;

	/**
	 * BakeShot only: keep the twin's keys outside the frames baked now, so bakes for several edits that use the same
	 * shot add up. False replaces every key (a full re-bake).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	bool bKeepOtherKeys = false;

	/**
	 * Camera ticks per frame. 1 matches a render at the sequence frame rate (one tick per output frame). Higher values
	 * match a faster-ticking viewport: Black Eye's LookAt result depends on how many ticks it gets (DESIGN §8).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake", meta = (ClampMin = 1, ClampMax = 16))
	int32 SubSteps = 1;

	/**
	 * After the opening snap, the camera ticks this long with time held at the first frame, so the bake starts settled
	 * where the live camera sits when the editor is parked there. 0 starts from the raw snap (DESIGN trap 4.15).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake", meta = (ClampMin = 0, Units = "s"))
	float SettleSeconds = 10.f;

	/** Refresh only the subjects' skeletal meshes each frame (fast). False refreshes every skeletal mesh in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	bool bRefreshSubjectsOnly = true;

	/** Where to write the per-frame samples. Empty: no file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	FString CsvPath;

	/** BakeShot only: point the shot's camera cuts at the baked twin afterwards, so the shot plays the bake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	bool bLockAfterBake = true;

	/** Appended to the progress dialog's title (a batch's "shot 2 of 5"). Not reflected. */
	FString ProgressNote;
};

/** What one bake did, and what it cost. */
USTRUCT(BlueprintType)
struct FBlackEyeFastBakeReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") bool bSuccess = false;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString Message;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString CameraLabel;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") int32 NumFrames = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") int32 NumRefreshedMeshes = 0;
	/** False when every sample is identical: LookAt/Follow did nothing (no valid viewport, no subjects). */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") bool bCameraMoved = false;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") double TotalSeconds = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") double MsPerFrame = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") double MsEvaluate = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") double MsRefreshMeshes = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") double MsCameraTick = 0.0;
	/** Sequence seconds baked per wall-clock second. */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") double SpeedVsRealtime = 0.0;
	/** BakeShot only: the twin binding written, and whether the shot now plays it. */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString TwinBindingName;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") bool bLocked = false;
};

/** One baked Black Eye camera in a sequence. */
USTRUCT(BlueprintType)
struct FBlackEyeBakeInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString CameraBindingName;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString TwinBindingName;
	/** True when the sequence's camera cuts play the twin rather than the live Black Eye camera. */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") bool bLocked = false;
	/** When and how it was baked: date, frame range, sub-steps, warm-up, Black Eye version. */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString Info;
};

/** One Black Eye camera of one shot, and the frames of it an edit shows. GetEditBakePlan returns these. */
USTRUCT(BlueprintType)
struct FBlackEyeShotBakePlan
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") TObjectPtr<ULevelSequence> Shot = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") FString CameraBindingName;
	/** Shot display frames the edit shows, plus handles, merged and sorted. Pass as FBlackEyeFastBakeOptions::Ranges. */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") TArray<FBlackEyeBakeRange> Ranges;
	/** How many cinematic shot sections of the edit use this camera. */
	UPROPERTY(BlueprintReadOnly, Category = "Fast Bake") int32 NumUses = 0;
};

/**
 * Fast Bake entry points, callable from Blueprint and Python (unreal.BlackEyeFastBakeLibrary).
 * BakeShot is the feature; BakeCameraToCsv and the realtime record are for measuring it. docs/fast-bake/DESIGN.md.
 */
UCLASS()
class BLACKEYECUSTOMEDITOR_API UBlackEyeFastBakeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Steps one Black Eye camera through the sequence at a fixed 1/DisplayRate, offline, and samples its camera
	 * component world transform, focal length, focus distance and aperture each frame. Opens the sequence in
	 * Sequencer as root. Needs a live level viewport (LookAt does nothing without one).
	 */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static FBlackEyeFastBakeReport BakeCameraToCsv(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options);

	/**
	 * Starts sampling the camera after every editor world tick while Sequencer plays, for comparing a realtime
	 * playback against a bake. Snaps the camera first. Play the sequence after calling this.
	 */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static bool StartRealtimeRecord(ULevelSequence* Sequence, const FString& CameraBindingName);

	/** Stops a realtime record and writes it. Returns the number of samples written. */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static int32 StopRealtimeRecord(const FString& CsvPath);

	/**
	 * Bakes one Black Eye camera of a shot into keys on its twin: a spawnable plain CineCamera named
	 * "<camera binding>_Bake" beside it, created on the first bake and rewritten on every re-bake. The twin gets the
	 * camera's lens, filmback, crop, overscan, post process and its non-Black-Eye components (a lens component such
	 * as DynamicLens), and keys for transform, focal length, focus distance and aperture on every frame. One undo
	 * step. With bLockAfterBake the shot's camera cuts then play the twin.
	 */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static FBlackEyeFastBakeReport BakeShot(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options);

	/**
	 * Locks (camera cuts play the baked twin) or unlocks (camera cuts play the live Black Eye camera) a baked camera.
	 * Empty CameraBindingName: every baked camera in the sequence. Returns the number of camera cut sections changed.
	 */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static int32 SetLocked(ULevelSequence* Sequence, const FString& CameraBindingName, bool bLocked);

	/** Every baked Black Eye camera in the sequence: its twin, whether it's locked, and how it was baked. */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static TArray<FBlackEyeBakeInfo> GetBakeInfo(ULevelSequence* Sequence);

	/**
	 * What an edit (a sequence cutting between shots on a Cinematic Shot track) needs baked: for every shot it shows,
	 * the Black Eye camera on the shot's camera cuts and the shot frames the edit uses, widened by HandleFrames on
	 * each side. Nested edits are followed; a shot's own Sub tracks (its scene) are not. Bake each entry with BakeShot
	 * and its Ranges; the Sequencer toolbar's Fast Bake menu and BlackEyeCustom.FastBake.BakeEdit do all of them.
	 */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Fast Bake")
	static TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* Edit, int32 HandleFrames);
};
