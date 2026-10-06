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
};

/**
 * Fast Bake entry points, callable from Blueprint and Python (unreal.BlackEyeFastBakeLibrary).
 * P0 spike: bakes to CSV only; writing keys onto a twin camera is P1. docs/fast-bake/DESIGN.md.
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
};
