// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Fast Bake: step a Black Eye camera offline at a fixed dt and sample what it solves. Writing the samples onto the
// baked twin and locking it is BlackEyeFastBakeTwin.cpp.
// The why behind every step is docs/fast-bake/DESIGN.md (sections 3 and 4); numbers in brackets below are its traps.

#include "BlackEyeFastBakeLibrary.h"
#include "BlackEyeFastBakeInternal.h"

#include "BlackEyeContract.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "ConstraintsManager.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Evaluation/MovieSceneEvaluationTemplateInstance.h"
#include "Evaluation/MovieScenePlayback.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "MovieScene.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Transform/AnimationEvaluation.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(BlackEyeFastBakeLibrary)

// Debugging switches, a bitmask, for bisecting a bake that disagrees with live playback. 0 in normal use.
// 1 keep viewport camera cuts on | 2 don't force Constrain Aspect Ratio | 4 skip the subject mesh refresh
// 8 evaluate without HasJumped | 16 freeze time at the first frame (tick the camera only)
// 32 skip ticking the camera's own components after it | 64 don't advance GFrameCounter per step
static TAutoConsoleVariable<int32> CVarFastBakeDebug(
	TEXT("BlackEyeCustom.FastBake.Debug"), 0,
	TEXT("Bitmask to switch bake steps off when bisecting (see BlackEyeFastBake.cpp). 0 = normal."));

static int32 DebugFlags() { return CVarFastBakeDebug.GetValueOnGameThread(); }

static TAutoConsoleVariable<int32> CVarFastBakeVerbose(
	TEXT("BlackEyeCustom.FastBake.Verbose"), 0,
	TEXT("1: log each refreshed mesh's animation state for the first frames of a bake (diagnosing frozen subjects)."));

namespace BlackEyeFastBake
{
	FSample Sample(const ACineCameraActor* Camera, double Frame)
	{
		const UCineCameraComponent* Cam = Camera->GetCineCameraComponent();
		FSample S;
		S.Frame = Frame;
		// The camera component, not the actor: LookAt moves the mount and camera components under the root.
		S.CameraWorld = Cam->GetComponentToWorld();
		S.FocalLength = Cam->CurrentFocalLength;
		S.FocusDistance = Cam->FocusSettings.ManualFocusDistance;
		S.Aperture = Cam->CurrentAperture;
		BlackEyeContract::GetFirstSubjectPoint(Camera, true, S.Subject);
		BlackEyeContract::GetFirstSubjectPoint(Camera, false, S.FollowSubject);
		return S;
	}

	bool WriteCsv(const FString& Path, const TArray<FSample>& Samples)
	{
		FString Out = TEXT("frame,x,y,z,qx,qy,qz,qw,pitch,yaw,roll,focal,focus,aperture,sx,sy,sz,fx,fy,fz\n");
		for (const FSample& S : Samples)
		{
			const FVector L = S.CameraWorld.GetLocation();
			const FQuat Q = S.CameraWorld.GetRotation();
			const FRotator R = Q.Rotator();
			Out += FString::Printf(TEXT("%.4f,%.4f,%.4f,%.4f,%.8f,%.8f,%.8f,%.8f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
				S.Frame, L.X, L.Y, L.Z, Q.X, Q.Y, Q.Z, Q.W, R.Pitch, R.Yaw, R.Roll, S.FocalLength, S.FocusDistance, S.Aperture,
				S.Subject.X, S.Subject.Y, S.Subject.Z, S.FollowSubject.X, S.FollowSubject.Y, S.FollowSubject.Z);
		}
		return FFileHelper::SaveStringToFile(Out, *Path);
	}

	/** Opens the sequence in Sequencer (ControlRigAssetActions.cpp:473-477 is the engine's own pattern). */
	TSharedPtr<ISequencer> OpenSequencer(ULevelSequence* Sequence, FString& OutError)
	{
		UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Sequence || !AssetEditors)
		{
			OutError = TEXT("no sequence, or no editor");
			return nullptr;
		}
		IAssetEditorInstance* Editor = AssetEditors->FindEditorForAsset(Sequence, false);
		TSharedPtr<ISequencer> Sequencer = Editor ? static_cast<ILevelSequenceEditorToolkit*>(Editor)->GetSequencer() : nullptr;
		if (Sequencer.IsValid() && Sequencer->GetRootMovieSceneSequence() == Sequence && Sequencer->GetFocusedMovieSceneSequence() == Sequence)
		{
			return Sequencer;
		}
		// Not open on its own yet. Open it, but don't bake in the same tick: a Sequencer opened this frame isn't ready
		// to be edited, and doing so crashed (BlackEyeFastBakeMenu.cpp, SequencerSettleSeconds). The menu waits for it;
		// a script calls again.
		AssetEditors->OpenEditorForAsset(Sequence);
		OutError = TEXT("the sequence wasn't open in Sequencer on its own, so it has been opened now; run again in a moment");
		return nullptr;
	}

	/** The Black Eye camera bound in the focused sequence, by binding name or the first one found. */
	ACineCameraActor* FindCamera(ISequencer& Sequencer, const FString& BindingName, FGuid& InOutBinding)
	{
		const UMovieScene* MovieScene = Sequencer.GetFocusedMovieSceneSequence()->GetMovieScene();
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			const FGuid Guid = Binding.GetObjectGuid();
			if (InOutBinding.IsValid() ? Guid != InOutBinding
				: (!BindingName.IsEmpty() && MovieScene->GetObjectDisplayName(Guid).ToString() != BindingName))
			{
				continue;
			}
			for (const TWeakObjectPtr<>& Bound : Sequencer.FindBoundObjects(Guid, Sequencer.GetFocusedTemplateID()))
			{
				AActor* Actor = Cast<AActor>(Bound.Get());
				if (BlackEyeContract::IsBlackEyeCamera(Actor))
				{
					InOutBinding = Guid;
					return Cast<ACineCameraActor>(Actor);
				}
			}
		}
		return nullptr;
	}

	/**
	 * With no camera named: the Black Eye camera the shot's camera cuts play, so a shot holding a spare one picks the
	 * right camera. A locked shot's cut plays the twin, so a twin maps back to the camera it was baked from; without
	 * that, a re-bake of a locked shot baked the spare (measured 2026-10-06). Invalid when no cut leads to one.
	 */
	FGuid CutCameraBinding(ISequencer& Sequencer, const UMovieScene& MovieScene)
	{
		const UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack());
		if (!Cuts)
		{
			return FGuid();
		}
		const TArray<FTwin> Twins = FindTwins(MovieScene);
		for (const UMovieSceneSection* Section : Cuts->GetAllSections())
		{
			const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
			FGuid Candidate = Cut ? Cut->GetCameraBindingID().GetGuid() : FGuid();
			for (const FTwin& Twin : Twins)
			{
				if (Twin.Twin == Candidate || Twin.Stale.Contains(Candidate))
				{
					Candidate = Twin.Camera;
				}
			}
			if (Candidate.IsValid() && FindCamera(Sequencer, FString(), Candidate))
			{
				return Candidate;
			}
		}
		return FGuid();
	}

	/** Evaluates the root sequence at a display frame, blocking. Root == focused, so no time transform is needed. */
	void Evaluate(ISequencer& Sequencer, const UMovieScene& MovieScene, FFrameTime DisplayTime)
	{
		const FFrameTime Tick = FFrameRate::TransformTime(DisplayTime, MovieScene.GetDisplayRate(), MovieScene.GetTickResolution());
		// Always Stopped, whatever Sequencer is doing: a bake started during playback must not fire what Playing fires
		// (anim notifies, audio), and must step the same way as one started while parked.
		FMovieSceneContext Context(FMovieSceneEvaluationRange(Tick, MovieScene.GetTickResolution()), EMovieScenePlayerStatus::Stopped);
		// Jumped, so every track evaluates its absolute state at this frame. Safe only because viewport camera
		// cuts are off for the bake: with them on, a jump makes every frame a camera cut that snaps the camera [4.1].
		Context.SetHasJumped((DebugFlags() & 8) == 0);
		Sequencer.GetEvaluationTemplate().EvaluateSynchronousBlocking(Context);
	}

	/**
	 * Runs the camera's own ticking components, in tick-group order, as a world tick would after the actor. A lens
	 * component that corrects Black Eye's output must run: DynamicLens restores the focal length Black Eye shrinks
	 * by the overscan factor on every LookAt tick, and without it focal collapses to 0 within a few hundred steps
	 * (DESIGN trap 4.13). Follow, LookAt and Black Eye's collider don't tick (bCanEverTick false), so they aren't stepped
	 * twice. Black Eye's camera component does (inherited from UCameraComponent), but only to size its editor proxy mesh
	 * and frustum (BlackEyeCineCameraComponent.cpp:62-86), which nothing samples.
	 * BE-NATIVE: fixing the overscan feedback in FBlackEyeLookAtState::UpdateFrom (BlackEyeLookUtils.cpp ~L353)
	 * removes the need for any component to run after Black Eye.
	 */
	void TickCameraComponents(AActor* Camera, float Dt)
	{
		TInlineComponentArray<UActorComponent*> Components(Camera);
		Components.StableSort([](const UActorComponent& A, const UActorComponent& B)
		{
			return A.PrimaryComponentTick.TickGroup < B.PrimaryComponentTick.TickGroup;
		});
		for (UActorComponent* Component : Components)
		{
			if (Component->IsRegistered() && Component->PrimaryComponentTick.bCanEverTick && Component->IsComponentTickEnabled())
			{
				Component->TickComponent(Dt, LEVELTICK_All, &Component->PrimaryComponentTick);
			}
		}
	}

	int32 AttachDepth(const USceneComponent* Component)
	{
		int32 Depth = 0;
		for (const USceneComponent* P = Component->GetAttachParent(); P; P = P->GetAttachParent())
		{
			++Depth;
		}
		return Depth;
	}

	/** Skeletal meshes to pose each frame, parents first (a MetaHuman Face copies the head from its Body) [4.6]. */
	void GatherMeshes(UWorld* World, const ACineCameraActor* Camera, bool bSubjectsOnly, TArray<USkeletalMeshComponent*>& Out)
	{
		Out.Reset();
		if (bSubjectsOnly)
		{
			TArray<AActor*> Subjects;
			BlackEyeContract::GetSubjectActors(Camera, Subjects);
			// A subject is often a tracker attached to a character's bone (a MetaHuman Face's FACIAL_L_Eye), so the
			// meshes to pose are those of every actor up its attach chain, not just its own.
			TSet<AActor*> Owners;
			for (AActor* Subject : Subjects)
			{
				for (AActor* A = Subject; A && !Owners.Contains(A); A = A->GetAttachParentActor())
				{
					Owners.Add(A);
				}
			}
			for (AActor* Owner : Owners)
			{
				TArray<USkeletalMeshComponent*> Meshes;
				Owner->GetComponents(Meshes);
				for (USkeletalMeshComponent* Mesh : Meshes)
				{
					Out.AddUnique(Mesh);
				}
			}
		}
		else
		{
			for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
			{
				if (It->GetWorld() == World && It->IsRegistered())
				{
					Out.Add(*It);
				}
			}
		}
		Out.StableSort([](const USkeletalMeshComponent& A, const USkeletalMeshComponent& B) { return AttachDepth(&A) < AttachDepth(&B); });
	}

	/** Engine-side state the bake changes, put back when it ends, whatever happens. */
	struct FRestore
	{
		TSharedPtr<ISequencer> Sequencer;
		FQualifiedFrameTime Time;
		bool bCameraCuts = true;
		TWeakObjectPtr<ACineCameraActor> Camera;
		bool bCameraTick = true;
		bool bConstrainAspect = false;
		TMap<TWeakObjectPtr<USkeletalMeshComponent>, TPair<int32, bool>> Meshes; // forced LOD, URO

		void Touch(USkeletalMeshComponent* Mesh)
		{
			if (!Meshes.Contains(Mesh))
			{
				Meshes.Add(Mesh, { Mesh->GetForcedLOD(), (bool)Mesh->bEnableUpdateRateOptimizations });
				Mesh->SetForcedLOD(1); // LOD 0: the bone a tracker sits on must be the full-detail one
				Mesh->bEnableUpdateRateOptimizations = false; // [4.6]
			}
		}

		~FRestore()
		{
			for (const auto& It : Meshes)
			{
				if (USkeletalMeshComponent* Mesh = It.Key.Get())
				{
					Mesh->SetForcedLOD(It.Value.Key);
					Mesh->bEnableUpdateRateOptimizations = It.Value.Value;
				}
			}
			if (ACineCameraActor* Cam = Camera.Get())
			{
				Cam->GetCineCameraComponent()->bConstrainAspectRatio = bConstrainAspect;
				Cam->SetActorTickEnabled(bCameraTick);
			}
			if (Sequencer.IsValid())
			{
				Sequencer->SetPerspectiveViewportCameraCutEnabled(bCameraCuts);
				Sequencer->SetLocalTimeDirectly(Time.Time);
				Sequencer->ForceEvaluate();
			}
		}
	};

	// Realtime record state (one at a time).
	TWeakPtr<ISequencer> RecordSequencer;
	FGuid RecordBinding;
	FDelegateHandle RecordHandle;
	TArray<FSample> RecordSamples;
}

FBlackEyeFastBakeReport UBlackEyeFastBakeLibrary::BakeCameraToCsv(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options)
{
	BlackEyeFastBake::FBakeOutput Out;
	return BlackEyeFastBake::RunBake(Sequence, Options, Out);
}

FBlackEyeFastBakeReport BlackEyeFastBake::RunBake(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Out)
{
	FBlackEyeFastBakeReport Report;

	TSharedPtr<ISequencer> Sequencer = OpenSequencer(Sequence, Report.Message);
	if (!Sequencer)
	{
		return Report;
	}
	UMovieScene& MovieScene = *Sequence->GetMovieScene();
	const FFrameRate DisplayRate = MovieScene.GetDisplayRate();
	const TRange<FFrameNumber> Playback = MovieScene.GetPlaybackRange();
	const int32 PlaybackStart = FFrameRate::TransformTime(FFrameTime(Playback.GetLowerBoundValue()), MovieScene.GetTickResolution(), DisplayRate).RoundToFrame().Value;
	const int32 PlaybackEnd = FFrameRate::TransformTime(FFrameTime(Playback.GetUpperBoundValue()), MovieScene.GetTickResolution(), DisplayRate).RoundToFrame().Value;
	const int32 Start = Options.StartFrame != INDEX_NONE ? Options.StartFrame : PlaybackStart;
	const int32 End = Options.EndFrame != INDEX_NONE ? Options.EndFrame : PlaybackEnd;
	const int32 WarmUpStart = Start - FMath::Max(0, Options.WarmUpFrames);
	const float Dt = static_cast<float>(DisplayRate.AsInterval());
	if (End <= Start)
	{
		Report.Message = FString::Printf(TEXT("empty range [%d, %d)"), Start, End);
		return Report;
	}

	FRestore Restore;
	Restore.Sequencer = Sequencer;
	Restore.Time = Sequencer->GetLocalTime();
	Restore.bCameraCuts = Sequencer->IsPerspectiveViewportCameraCutEnabled();
	// BE-NATIVE: an editor camera cut that snaps correctly would make this unnecessary (DESIGN §7).
	if ((DebugFlags() & 1) == 0)
	{
		Sequencer->SetPerspectiveViewportCameraCutEnabled(false); // [4.1]
	}

	// Spawnables exist only once evaluated, so evaluate the first frame before looking for the camera.
	Evaluate(*Sequencer, MovieScene, FFrameTime(WarmUpStart));
	// No name given: the camera the cuts play (through a twin if locked), and only then the first Black Eye camera.
	FGuid Binding = Options.CameraBindingName.IsEmpty() ? CutCameraBinding(*Sequencer, MovieScene) : FGuid();
	ACineCameraActor* Camera = FindCamera(*Sequencer, Options.CameraBindingName, Binding);
	if (!Camera)
	{
		Report.Message = TEXT("no Black Eye camera bound in the sequence") +
			(Options.CameraBindingName.IsEmpty() ? FString() : FString::Printf(TEXT(" named '%s'"), *Options.CameraBindingName));
		return Report;
	}
	Report.CameraLabel = Camera->GetActorLabel();
	UWorld* World = Camera->GetWorld();

	// Only our steps may advance the camera. BE-NATIVE: a public StepCamera(dt) replaces tick juggling (DESIGN §7).
	Restore.Camera = Camera;
	Restore.bCameraTick = Camera->IsActorTickEnabled();
	Camera->SetActorTickEnabled(false);
	// Solve against the filmback aspect, as a render does, not the editor viewport's shape.
	Restore.bConstrainAspect = Camera->GetCineCameraComponent()->bConstrainAspectRatio;
	Camera->GetCineCameraComponent()->bConstrainAspectRatio = (DebugFlags() & 2) ? Restore.bConstrainAspect : true;
	// Black Eye's Tick dereferences the active viewport without a null check when the camera is selected [4.2].
	if (Camera->IsSelected())
	{
		GEditor->SelectActor(Camera, false, true);
	}

	TArray<USkeletalMeshComponent*> Meshes;
	TArray<FSample> Samples;
	Samples.Reserve(End - Start);
	double TEval = 0.0, TRefresh = 0.0, TTick = 0.0;
	const double T0 = FPlatformTime::Seconds();

	// Modal: a mouse button held over the viewport changes what LookAt does [4.5].
	FScopedSlowTask Task(static_cast<float>(End - WarmUpStart), FText::FromString(FString::Printf(TEXT("Fast Bake: %s"), *Report.CameraLabel)));
	Task.MakeDialog(true);

	const int32 SubSteps = FMath::Max(1, Options.SubSteps);
	const float StepDt = Dt / SubSteps;
	bool bFailed = false;

	// One step: put the world at Time, pose the subjects, then advance (or snap) the camera.
	auto Step = [&](FFrameTime Time, int32 Frame, bool bSnap)
	{
		// Each step stands for one engine frame, so the frame counter advances as it would between editor ticks, the
		// way the engine's own out-of-loop frames do it (CommandletHelpers::TickEngine, Commandlet.cpp:144;
		// HighResScreenshotBeginFrame, UnrealClient.cpp:1604). Without it, per-frame work keyed on GFrameCounter runs
		// once for the whole bake: an anim instance's native and Blueprint update (AnimInstanceProxy.cpp:1336), and
		// Sequencer re-posing every animated mesh on every step because its pose was "already ticked this frame"
		// (MovieSceneSkeletalAnimationSystem.cpp:710-723) [4.18].
		if ((DebugFlags() & 64) == 0)
		{
			++GFrameCounter;
		}
		double T = FPlatformTime::Seconds();
		Evaluate(*Sequencer, MovieScene, (DebugFlags() & 16) ? FFrameTime(WarmUpStart) : Time);
		{
			const UE::Anim::FEvaluationForCachingScope CachingScope(StepDt);
			FConstraintsManagerController::Get(World).EvaluateAllConstraints();
		}
		TEval += FPlatformTime::Seconds() - T;

		// Spawnables re-spawn at section boundaries, so resolve the camera and subjects every step [4.8].
		ACineCameraActor* Current = FindCamera(*Sequencer, FString(), Binding);
		if (!Current)
		{
			Report.Message = FString::Printf(TEXT("camera binding stopped resolving at frame %d"), Frame);
			bFailed = true;
			return;
		}
		if (Current != Camera)
		{
			Camera = Current;
			Restore.Camera = Camera;
			Camera->SetActorTickEnabled(false);
			Camera->GetCineCameraComponent()->bConstrainAspectRatio = true;
			if (Camera->IsSelected())
			{
				GEditor->SelectActor(Camera, false, true); // [4.2], as at the start
			}
		}

		T = FPlatformTime::Seconds();
		GatherMeshes(World, Camera, Options.bRefreshSubjectsOnly, Meshes);
		if (DebugFlags() & 4)
		{
			Meshes.Reset();
		}
		const bool bVerbose = CVarFastBakeVerbose.GetValueOnGameThread() > 0 && (Frame - WarmUpStart < 4 || Frame % 50 == 0);
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			Restore.Touch(Mesh);
			const FVector Before = bVerbose ? Mesh->GetSocketLocation(TEXT("head")) : FVector::ZeroVector;
			// Sequencer's evaluation only sets the animation time; the pose is built here, as an editor tick would
			// (FSequencerBaker::TickFrameInternal, SequencerBaker.cpp:420-436).
			Mesh->TickAnimation(StepDt, false);
			Mesh->RefreshBoneTransforms();
			Mesh->RefreshFollowerComponents();
			Mesh->UpdateComponentToWorld();
			Mesh->FinalizeBoneTransform();
			// Re-place anything attached to a bone or socket (a tracker on an eye), as the engine does for follower
			// meshes (SkinnedMeshComponent.cpp:3348); finalizing the pose alone leaves them where they were.
			Mesh->UpdateChildTransforms(EUpdateTransformFlags::OnlyUpdateIfUsingSocket);
			Mesh->MarkRenderTransformDirty();
			Mesh->MarkRenderDynamicDataDirty();
			if (bVerbose)
			{
				const UAnimInstance* Anim = Mesh->GetAnimInstance();
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] verbose f%d %s mode %d anim %s shouldTick %d ticked %d postEval %d head z %.2f -> %.2f"),
					Frame, *Mesh->GetName(), (int32)Mesh->GetAnimationMode(), Anim ? *Anim->GetClass()->GetName() : TEXT("none"),
					Mesh->ShouldTickAnimation(), Mesh->PoseTickedThisFrame(), Mesh->IsPostEvaluatingAnimation(),
					Before.Z, Mesh->GetSocketLocation(TEXT("head")).Z);
			}
		}
		{
			// A tracker may be constraint-driven off a bone that only just moved [4.6].
			const UE::Anim::FEvaluationForCachingScope CachingScope(StepDt);
			FConstraintsManagerController::Get(World).EvaluateAllConstraints();
		}
		Report.NumRefreshedMeshes = FMath::Max(Report.NumRefreshedMeshes, Meshes.Num());
		TRefresh += FPlatformTime::Seconds() - T;

		T = FPlatformTime::Seconds();
		if (bSnap)
		{
			// One explicit snap at the start replaces the camera cut a game or render would send.
			BlackEyeContract::SnapNow(Camera);
		}
		else
		{
			// The BEC override of AActor::Tick runs Follow then LookAt (BlackEyeCineCameraActorBase.cpp:124-150).
			const FRotator CamBefore = Camera->GetCineCameraComponent()->GetComponentRotation();
			Camera->Tick(StepDt);
			if (CVarFastBakeVerbose.GetValueOnGameThread() > 0 && (Frame - WarmUpStart < 4 || Frame % 50 == 0))
			{
				const FViewport* Active = GEditor ? GEditor->GetActiveViewport() : nullptr;
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] verbose f%d viewport %s %dx%d | actor yaw %.2f | camera yaw %.2f -> %.2f | focal %.2f"),
					Frame, Active ? TEXT("ok") : TEXT("NONE"), Active ? Active->GetSizeXY().X : 0, Active ? Active->GetSizeXY().Y : 0,
					Camera->GetActorRotation().Yaw, CamBefore.Yaw, Camera->GetCineCameraComponent()->GetComponentRotation().Yaw,
					Camera->GetCineCameraComponent()->CurrentFocalLength);
			}
		}
		if ((DebugFlags() & 32) == 0)
		{
			TickCameraComponents(Camera, StepDt);
		}
		TTick += FPlatformTime::Seconds() - T;
	};

	for (int32 Frame = WarmUpStart; Frame < End; ++Frame)
	{
		Task.EnterProgressFrame(1.f);
		if (Task.ShouldCancel())
		{
			Report.Message = TEXT("cancelled");
			return Report;
		}

		if (Frame == WarmUpStart)
		{
			Step(FFrameTime(Frame), Frame, true);
			// The snap is one huge-dt tick and lands somewhere else than where Follow and LookAt settle (trap 4.15):
			// 60 cm off on a production camera, still visible a second later in a short shot. So settle with time held
			// at the first frame, as the live camera does while the editor is parked there.
			// BE-NATIVE: a snap that solves to the settled state would make this unnecessary (DESIGN section 7).
			const int32 SettleTicks = FMath::CeilToInt(FMath::Max(0.f, Options.SettleSeconds) / StepDt);
			const double TSettle = FPlatformTime::Seconds();
			for (int32 I = 0; I < SettleTicks && Camera; ++I)
			{
				Camera->Tick(StepDt);
				TickCameraComponents(Camera, StepDt);
			}
			TTick += FPlatformTime::Seconds() - TSettle;
		}
		else
		{
			// Sub-steps land on (Frame-1, Frame], ending exactly on the frame that is sampled.
			for (int32 S = 1; S <= SubSteps && !bFailed; ++S)
			{
				Step(FFrameTime(Frame - 1) + FFrameTime::FromDecimal(static_cast<double>(S) / SubSteps), Frame, false);
			}
		}
		if (bFailed)
		{
			return Report;
		}

		if (Frame >= Start)
		{
			Samples.Add(Sample(Camera, Frame));
		}
	}

	Report.TotalSeconds = FPlatformTime::Seconds() - T0;
	const int32 Stepped = End - WarmUpStart;
	Report.NumFrames = Samples.Num();
	Report.MsPerFrame = 1000.0 * Report.TotalSeconds / Stepped;
	Report.MsEvaluate = 1000.0 * TEval / Stepped;
	Report.MsRefreshMeshes = 1000.0 * TRefresh / Stepped;
	Report.MsCameraTick = 1000.0 * TTick / Stepped;
	Report.SpeedVsRealtime = (Stepped * DisplayRate.AsInterval()) / FMath::Max(Report.TotalSeconds, 1e-9);

	// LookAt silently does nothing without a valid viewport [4.4]: a camera that never moved is a failed bake.
	for (const FSample& S : Samples)
	{
		if (!S.CameraWorld.Equals(Samples[0].CameraWorld, 1e-3) || !FMath::IsNearlyEqual(S.FocalLength, Samples[0].FocalLength, 1e-4f))
		{
			Report.bCameraMoved = true;
			break;
		}
	}

	if (!Options.CsvPath.IsEmpty() && !WriteCsv(Options.CsvPath, Samples))
	{
		Report.Message = FString::Printf(TEXT("could not write %s"), *Options.CsvPath);
		return Report;
	}
	Report.bSuccess = Report.bCameraMoved;
	Report.Message = Report.bCameraMoved ? TEXT("ok") : TEXT("the camera never moved: no valid viewport, or no subjects");
	Out.Sequencer = Sequencer;
	Out.CameraBinding = Binding;
	Out.Samples = MoveTemp(Samples);
	UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] Fast Bake %s: %d frames in %.2fs, %.2f ms/frame (eval %.2f, meshes %.2f x%d, tick %.3f), %.1fx realtime: %s"),
		*Report.CameraLabel, Report.NumFrames, Report.TotalSeconds, Report.MsPerFrame, Report.MsEvaluate, Report.MsRefreshMeshes,
		Report.NumRefreshedMeshes, Report.MsCameraTick, Report.SpeedVsRealtime, *Report.Message);
	return Report;
}

bool UBlackEyeFastBakeLibrary::StartRealtimeRecord(ULevelSequence* Sequence, const FString& CameraBindingName)
{
	using namespace BlackEyeFastBake;
	StopRealtimeRecord(FString());

	FString Error;
	TSharedPtr<ISequencer> Sequencer = OpenSequencer(Sequence, Error);
	// Same camera choice as the bake, so a record and a bake of a shot compare the same camera.
	FGuid Binding = Sequencer && CameraBindingName.IsEmpty()
		? CutCameraBinding(*Sequencer, *Sequencer->GetFocusedMovieSceneSequence()->GetMovieScene()) : FGuid();
	ACineCameraActor* Camera = Sequencer ? FindCamera(*Sequencer, CameraBindingName, Binding) : nullptr;
	if (!Camera)
	{
		UE_LOG(LogBlackEyeCustom, Warning, TEXT("[BlackEyeCustom] realtime record: no Black Eye camera (%s)"), *Error);
		return false;
	}
	BlackEyeContract::SnapNow(Camera);

	RecordSequencer = Sequencer;
	RecordBinding = Binding;
	RecordSamples.Reset();
	RecordHandle = FWorldDelegates::OnWorldPostActorTick.AddLambda([](UWorld* World, ELevelTick, float)
	{
		TSharedPtr<ISequencer> Seq = RecordSequencer.Pin();
		if (!Seq || Seq->GetPlaybackStatus() != EMovieScenePlayerStatus::Playing)
		{
			return;
		}
		FGuid Binding = RecordBinding;
		ACineCameraActor* Cam = FindCamera(*Seq, FString(), Binding);
		if (Cam && Cam->GetWorld() == World)
		{
			const UMovieScene* MovieScene = Seq->GetFocusedMovieSceneSequence()->GetMovieScene();
			RecordSamples.Add(Sample(Cam, Seq->GetLocalTime().ConvertTo(MovieScene->GetDisplayRate()).AsDecimal()));
		}
	});
	return true;
}

int32 UBlackEyeFastBakeLibrary::StopRealtimeRecord(const FString& CsvPath)
{
	using namespace BlackEyeFastBake;
	if (RecordHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(RecordHandle);
		RecordHandle.Reset();
	}
	const int32 Num = RecordSamples.Num();
	if (!CsvPath.IsEmpty() && Num > 0)
	{
		WriteCsv(CsvPath, RecordSamples);
	}
	RecordSamples.Reset();
	RecordSequencer.Reset();
	return Num;
}
