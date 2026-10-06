// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlackEyeFastBakeLibrary.generated.h"

class ULevelSequence;

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

	/** Frames stepped (not sampled) before StartFrame, after the snap, so damping is settled when sampling begins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	int32 WarmUpFrames = 0;

	/**
	 * Camera ticks per frame. 1 matches a render at the sequence frame rate (one tick per output frame). Higher values
	 * match a faster-ticking viewport: Black Eye's LookAt result depends on how many ticks it gets (DESIGN §8).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake", meta = (ClampMin = 1, ClampMax = 16))
	int32 SubSteps = 1;

	/** Refresh only the subjects' skeletal meshes each frame (fast). False refreshes every skeletal mesh in the world. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	bool bRefreshSubjectsOnly = true;

	/** Where to write the per-frame samples. Empty: no file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	FString CsvPath;

	/** BakeShot only: point the shot's camera cuts at the baked twin afterwards, so the shot plays the bake. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fast Bake")
	bool bLockAfterBake = true;
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
};
