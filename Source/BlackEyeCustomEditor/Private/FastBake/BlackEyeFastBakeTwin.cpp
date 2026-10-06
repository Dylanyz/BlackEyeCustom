// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Fast Bake output: the baked twin (DESIGN section 5). A plain spawnable CineCamera beside the Black Eye camera in
// the same shot, carrying the bake as keys. Lock = the shot's camera cuts play the twin; unlock = they play the live
// Black Eye camera again. Every edit or master that nests the shot then plays and renders whichever is locked.

#include "BlackEyeFastBakeInternal.h"

#include "BlackEyeContract.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Engine/Engine.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneObjectBindingID.h"
#include "Interfaces/IPluginManager.h"
#include "ScopedTransaction.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "SequencerUtilities.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"

#define LOCTEXT_NAMESPACE "BlackEyeFastBake"

namespace BlackEyeFastBake
{
	// The twin is found again by a binding tag that names its Black Eye camera, so a re-bake rewrites the same
	// binding and a locked camera cut keeps pointing at it.
	const TCHAR* TwinTagPrefix = TEXT("BlackEyeFastBake_");
	// How a twin was baked lives in its template's actor Tags: transacted with the keys, so an undo reverts both
	// (package metadata isn't transacted).
	const TCHAR* InfoTagPrefix = TEXT("BlackEyeFastBake: ");

	FName TwinTag(const FGuid& Camera)
	{
		return FName(*(FString(TwinTagPrefix) + Camera.ToString(EGuidFormats::Digits)));
	}

	/**
	 * A tag can name several twins: `UMovieScene::TagBinding` appends (MovieScene.cpp:593-599), and a binding removed
	 * from script, not through Sequencer's Delete (ObjectBindingModel.cpp:1135-1149, which untags), leaves its ID behind.
	 * Measured on a production shot (2026-10-06): five IDs, only the last alive. Reading the first made every re-bake
	 * create a new twin that the locked camera cut never showed, and made Lock / Unlock and the bake info act on a
	 * dead binding. So the twin is the newest ID whose binding exists, and the rest are stale.
	 */
	TArray<FTwin> FindTwins(const UMovieScene& MovieScene)
	{
		TArray<FTwin> Out;
		for (const TPair<FName, FMovieSceneObjectBindingIDs>& Tag : MovieScene.AllTaggedBindings())
		{
			FString Name = Tag.Key.ToString();
			FGuid Camera;
			if (!Name.RemoveFromStart(TwinTagPrefix) || !FGuid::Parse(Name, Camera) || Tag.Value.IDs.Num() == 0)
			{
				continue;
			}
			FTwin& Twin = Out.AddDefaulted_GetRef();
			Twin.Camera = Camera;
			for (int32 i = Tag.Value.IDs.Num() - 1; i >= 0; --i)
			{
				const FGuid Id = Tag.Value.IDs[i].GetGuid();
				if (!Twin.Twin.IsValid() && MovieScene.FindBinding(Id))
				{
					Twin.Twin = Id;
				}
				else if (Id != Twin.Twin)
				{
					Twin.Stale.AddUnique(Id);
				}
			}
		}
		return Out;
	}

	/** Leaves the camera's tag naming only its current twin, so a sequence stops carrying dead ones. */
	void RetagTwin(UMovieScene& MovieScene, const FGuid& Camera, const FGuid& Twin)
	{
		MovieScene.RemoveTag(TwinTag(Camera));
		MovieScene.TagBinding(TwinTag(Camera), UE::MovieScene::FFixedObjectBindingID(Twin, MovieSceneSequenceID::Root));
	}

	FString BindingName(const UMovieScene& MovieScene, const FGuid& Guid)
	{
		return MovieScene.GetObjectDisplayName(Guid).ToString();
	}

	/**
	 * Copies every editable property UCameraComponent and UCineCameraComponent declare (filmback, lens settings and
	 * clamps, focus, crop, overscan, aspect, post process...), none of USceneComponent's (transform, attachment) and
	 * none of the Black Eye subclass's. The twin then renders through the same camera body; only motion is keyed.
	 */
	void CopyCameraSettings(const UCineCameraComponent* From, UCineCameraComponent* To)
	{
		for (TFieldIterator<FProperty> It(UCineCameraComponent::StaticClass()); It; ++It)
		{
			const UClass* Owner = It->GetOwnerClass();
			if ((Owner == UCameraComponent::StaticClass() || Owner == UCineCameraComponent::StaticClass())
				&& It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst))
			{
				It->CopyCompleteValue_InContainer(To, From);
			}
		}
		// The keys drive focus; a tracked actor would fight them.
		To->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		To->FocusSettings.TrackingFocusSettings.ActorToTrack = nullptr;
	}

	/**
	 * Copies the camera's components that aren't Black Eye's or the CineCamera's own (a lens component such as
	 * DynamicLens) onto the twin template, so the twin renders the same lens. Their focal corrections are idempotent
	 * (DESIGN section 5), so applying them to already-corrected keys changes nothing.
	 */
	TArray<FString> CopyExtraComponents(const AActor* From, ACineCameraActor* To)
	{
		TArray<FString> Copied;
		// The twin is ours entirely: a re-bake keeps the instance components that still match one on the camera and
		// drops the rest, so the twin never carries components the camera no longer has.
		TArray<UActorComponent*> Stale(To->GetInstanceComponents());
		TInlineComponentArray<UActorComponent*> Components(From);
		for (const UActorComponent* Source : Components)
		{
			const UClass* Class = Source->GetClass();
			// Only what a user or a Blueprint added. Native components made at runtime (the editor's frustum and proxy
			// mesh visualizers) are transient or visualization-only and belong to the camera class, not the setup.
			// The instance list counts too: helpers that add a component with AddInstanceComponent can leave its
			// CreationMethod at Native (DynamicLens's own does).
			const bool bAddedByUser = From->GetInstanceComponents().Contains(Source)
				|| Source->CreationMethod == EComponentCreationMethod::Instance
				|| Source->CreationMethod == EComponentCreationMethod::SimpleConstructionScript
				|| Source->CreationMethod == EComponentCreationMethod::UserConstructionScript;
			if (!bAddedByUser || Source->IsDefaultSubobject() || Source->HasAnyFlags(RF_Transient) || Source->IsVisualizationComponent()
				|| Class->GetOuterUPackage()->GetName() == TEXT("/Script/Black_Eye"))
			{
				continue;
			}
			UActorComponent* const* Existing = Stale.FindByPredicate([&](const UActorComponent* C)
			{
				return C && C->GetFName() == Source->GetFName() && C->GetClass() == Class;
			});
			UActorComponent* Target = Existing ? *Existing : nullptr;
			if (Target)
			{
				Stale.Remove(Target);
			}
			else
			{
				Target = NewObject<UActorComponent>(To, Class, MakeUniqueObjectName(To, Class, Source->GetFName()), RF_Transactional);
				To->AddInstanceComponent(Target);
			}
			Target->Modify();
			const bool bWasRegistered = Target->IsRegistered();
			if (bWasRegistered)
			{
				Target->UnregisterComponent();
			}
			UEngine::CopyPropertiesForUnrelatedObjects(const_cast<UActorComponent*>(Source), Target);
			if (USceneComponent* Scene = Cast<USceneComponent>(Target))
			{
				Scene->SetupAttachment(To->GetCineCameraComponent());
			}
			if (To->GetWorld())
			{
				Target->RegisterComponent();
			}
			Copied.Add(FString::Printf(TEXT("%s (%s)"), *Source->GetName(), *Class->GetName()));
		}
		for (UActorComponent* Old : Stale)
		{
			if (Old)
			{
				To->RemoveInstanceComponent(Old);
				Old->DestroyComponent();
			}
		}
		return Copied;
	}

	TArray<FFrameNumber> KeyTimes(const UMovieScene& MovieScene, const TArray<FSample>& Samples)
	{
		TArray<FFrameNumber> Times;
		Times.Reserve(Samples.Num());
		for (const FSample& S : Samples)
		{
			Times.Add(FFrameRate::TransformTime(FFrameTime::FromDecimal(S.Frame), MovieScene.GetDisplayRate(), MovieScene.GetTickResolution()).RoundToFrame());
		}
		return Times;
	}

	// Auto-tangent cubic keys on every frame: smooth between frames, which matters for motion blur sub-samples.
	void SetDoubleKeys(FMovieSceneDoubleChannel& Channel, const TArray<FFrameNumber>& Times, const TArray<double>& Values)
	{
		TArray<FMovieSceneDoubleValue> Keys;
		Keys.Reserve(Values.Num());
		for (double V : Values)
		{
			FMovieSceneDoubleValue Key(V);
			Key.InterpMode = RCIM_Cubic;
			Key.TangentMode = RCTM_Auto;
			Keys.Add(Key);
		}
		Channel.Set(Times, MoveTemp(Keys));
		Channel.AutoSetTangents();
	}

	void SetFloatKeys(FMovieSceneFloatChannel& Channel, const TArray<FFrameNumber>& Times, const TArray<float>& Values)
	{
		TArray<FMovieSceneFloatValue> Keys;
		Keys.Reserve(Values.Num());
		for (float V : Values)
		{
			FMovieSceneFloatValue Key(V);
			Key.InterpMode = RCIM_Cubic;
			Key.TangentMode = RCTM_Auto;
			Keys.Add(Key);
		}
		Channel.Set(Times, MoveTemp(Keys));
		Channel.AutoSetTangents();
	}

	/** One fresh infinite section on a track, replacing whatever an earlier bake left there. */
	template <typename SectionType>
	SectionType* ResetToOneSection(UMovieSceneTrack* Track)
	{
		Track->Modify();
		Track->RemoveAllAnimationData();
		SectionType* Section = Cast<SectionType>(Track->CreateNewSection());
		Section->SetRange(TRange<FFrameNumber>::All());
		Track->AddSection(*Section);
		return Section;
	}

	UMovieSceneFloatTrack* FindOrAddFloatTrack(UMovieScene& MovieScene, const FGuid& Binding, FName Name, const FString& Path)
	{
		if (const FMovieSceneBinding* Found = MovieScene.FindBinding(Binding))
		{
			for (UMovieSceneTrack* Track : Found->GetTracks())
			{
				UMovieSceneFloatTrack* Float = Cast<UMovieSceneFloatTrack>(Track);
				if (Float && Float->GetPropertyPath() == FName(*Path))
				{
					return Float;
				}
			}
		}
		UMovieSceneFloatTrack* Track = MovieScene.AddTrack<UMovieSceneFloatTrack>(Binding);
		Track->SetPropertyNameAndPath(Name, Path);
		return Track;
	}

	/** Twin camera component values as keys on its component binding (the binding Sequencer's UI would make). */
	void WriteLensKeys(UMovieScene& MovieScene, const FGuid& ComponentBinding, const TArray<FFrameNumber>& Times, const TArray<FSample>& Samples)
	{
		struct FLensChannel { FName Name; const TCHAR* Path; float FSample::* Member; };
		const FLensChannel Channels[] = {
			{ TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"), &FSample::FocalLength },
			{ TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"), &FSample::FocusDistance },
			{ TEXT("CurrentAperture"), TEXT("CurrentAperture"), &FSample::Aperture },
		};
		for (const FLensChannel& C : Channels)
		{
			TArray<float> Values;
			Values.Reserve(Samples.Num());
			for (const FSample& S : Samples)
			{
				Values.Add(S.*C.Member);
			}
			UMovieSceneFloatTrack* Track = FindOrAddFloatTrack(MovieScene, ComponentBinding, C.Name, C.Path);
			UMovieSceneFloatSection* Section = ResetToOneSection<UMovieSceneFloatSection>(Track);
			SetFloatKeys(Section->GetChannel(), Times, Values);
		}
	}

	/** The twin actor's transform: the Black Eye camera component's world transform, rotation unwound. */
	void WriteTransformKeys(UMovieScene& MovieScene, const FGuid& Twin, const TArray<FFrameNumber>& Times, const TArray<FSample>& Samples)
	{
		UMovieScene3DTransformTrack* Track = MovieScene.FindTrack<UMovieScene3DTransformTrack>(Twin);
		if (!Track)
		{
			Track = MovieScene.AddTrack<UMovieScene3DTransformTrack>(Twin);
		}
		UMovieScene3DTransformSection* Section = ResetToOneSection<UMovieScene3DTransformSection>(Track);
		TArrayView<FMovieSceneDoubleChannel*> Ch = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Ch.Num() < 9)
		{
			return;
		}

		TArray<double> V[6];
		FRotator Prev = Samples.Num() ? Samples[0].CameraWorld.Rotator() : FRotator::ZeroRotator;
		for (const FSample& S : Samples)
		{
			const FVector L = S.CameraWorld.GetLocation();
			FRotator R = S.CameraWorld.Rotator();
			// Unwind against the previous key so a turn through 180 deg doesn't spin the long way between keys.
			R.Roll = Prev.Roll + FMath::FindDeltaAngleDegrees(Prev.Roll, R.Roll);
			R.Pitch = Prev.Pitch + FMath::FindDeltaAngleDegrees(Prev.Pitch, R.Pitch);
			R.Yaw = Prev.Yaw + FMath::FindDeltaAngleDegrees(Prev.Yaw, R.Yaw);
			Prev = R;
			V[0].Add(L.X); V[1].Add(L.Y); V[2].Add(L.Z);
			V[3].Add(R.Roll); V[4].Add(R.Pitch); V[5].Add(R.Yaw); // channel order: Rotation X, Y, Z
		}
		for (int32 i = 0; i < 6; ++i)
		{
			SetDoubleKeys(*Ch[i], Times, V[i]);
		}
		for (int32 i = 6; i < 9; ++i)
		{
			Ch[i]->Reset();
			Ch[i]->SetDefault(1.0);
		}
	}

	/** A spawnable CineCamera, made the way Sequencer's Add > Actor does (not CreateCamera, which adds a camera cut). */
	FGuid CreateTwin(const TSharedRef<ISequencer>& Sequencer, UMovieScene& MovieScene, const FGuid& Camera)
	{
		const FString Name = BindingName(MovieScene, Camera) + TEXT("_Bake");
		const FGuid Twin = FSequencerUtilities::MakeNewSpawnable(Sequencer, *ACineCameraActor::StaticClass(), nullptr, true, FName(*Name));
		if (Twin.IsValid())
		{
			MovieScene.SetObjectDisplayName(Twin, FText::FromString(Name)); // WriteTwin tags it (RetagTwin)
		}
		return Twin;
	}

	/** Points every camera cut on any of From at To. */
	int32 RebindCuts(UMovieScene& MovieScene, TConstArrayView<FGuid> From, const FGuid& To)
	{
		int32 Changed = 0;
		if (UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack()))
		{
			for (UMovieSceneSection* Section : Cuts->GetAllSections())
			{
				UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
				if (Cut && From.Contains(Cut->GetCameraBindingID().GetGuid()) && Cut->GetCameraBindingID().GetGuid() != To)
				{
					Cut->Modify();
					Cut->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(To));
					++Changed;
				}
			}
		}
		return Changed;
	}

	bool IsLocked(const UMovieScene& MovieScene, const FGuid& Twin)
	{
		if (const UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack()))
		{
			for (const UMovieSceneSection* Section : Cuts->GetAllSections())
			{
				const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
				if (Cut && Cut->GetCameraBindingID().GetGuid() == Twin)
				{
					return true;
				}
			}
		}
		return false;
	}
}

bool BlackEyeFastBake::WriteTwin(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options, FBakeOutput& Bake,
                                 FBlackEyeFastBakeReport& Report)
{
	if (!Bake.Sequencer.IsValid() || Bake.Samples.Num() == 0)
	{
		Report.Message = TEXT("nothing baked to write");
		return false;
	}
	ISequencer& Sequencer = *Bake.Sequencer;
	UMovieScene& MovieScene = *Sequence->GetMovieScene();
	const FScopedTransaction Transaction(LOCTEXT("FastBake", "Black Eye Fast Bake"));
	Sequence->Modify();
	MovieScene.Modify();

	// Find this camera's twin, or make it.
	FGuid Twin;
	TArray<FGuid> Stale;
	for (const FTwin& Found : FindTwins(MovieScene))
	{
		if (Found.Camera == Bake.CameraBinding)
		{
			Twin = Found.Twin;
			Stale = Found.Stale;
		}
	}
	if (!Twin.IsValid())
	{
		Twin = CreateTwin(Bake.Sequencer.ToSharedRef(), MovieScene, Bake.CameraBinding);
	}
	if (!Twin.IsValid())
	{
		Report.Message = TEXT("could not create the twin camera");
		return false;
	}
	RetagTwin(MovieScene, Bake.CameraBinding, Twin);
	// A cut left on a stale twin was locked to an older bake: it plays this one now, lock or not.
	RebindCuts(MovieScene, Stale, Twin);
	Report.TwinBindingName = BindingName(MovieScene, Twin);

	// The live Black Eye camera, for its settings: the bake left the sequence evaluated, so it is spawned.
	AActor* Camera = nullptr;
	for (const TWeakObjectPtr<>& Bound : Sequencer.FindBoundObjects(Bake.CameraBinding, Sequencer.GetFocusedTemplateID()))
	{
		Camera = Cast<AActor>(Bound.Get());
	}
	// Edit the spawned twin, then save it as its template: Sequencer's own "Save Default State". A component added to
	// a template by hand doesn't survive spawning; one saved from a spawned instance does (measured, DESIGN 5).
	Sequencer.ForceEvaluate();
	ACineCameraActor* Spawned = nullptr;
	for (const TWeakObjectPtr<>& Bound : Sequencer.FindBoundObjects(Twin, Sequencer.GetFocusedTemplateID()))
	{
		Spawned = Cast<ACineCameraActor>(Bound.Get());
	}
	if (!Camera || !Spawned)
	{
		Report.Message = TEXT("could not resolve the Black Eye camera or the spawned twin (is the current time inside the shot?)");
		return false;
	}

	// Copy what was authored, not the running copy: a spawnable camera's spawned instance is transient and carries
	// runtime edits (a lens component matching the filmback, Black Eye's focal). Its template is the setup. A
	// possessable camera has no template; the level actor is the setup.
	const ACineCameraActor* Setup = Cast<ACineCameraActor>(
		MovieSceneHelpers::GetObjectTemplate(Sequence, Bake.CameraBinding, Sequencer.GetSharedPlaybackState()));
	if (!Setup)
	{
		Setup = Cast<ACineCameraActor>(Camera);
	}
	Spawned->Modify();
	UCineCameraComponent* TwinCam = Spawned->GetCineCameraComponent();
	TwinCam->Modify();
	CopyCameraSettings(Setup->GetCineCameraComponent(), TwinCam);
	TwinCam->SetRelativeTransform(FTransform::Identity); // the actor transform carries the camera's world transform
	const TArray<FString> Extra = CopyExtraComponents(Setup, Spawned);

	// How it was baked, for GetBakeInfo and the stale check (P2): saved into the template with everything else.
	const TSharedPtr<IPlugin> BE = IPluginManager::Get().FindEnabledPlugin(TEXT("Black_Eye"));
	const FString Info = FString::Printf(TEXT("baked %s; frames %.0f-%.0f; substeps %d; warmup %d; black eye %s; components copied: %s"),
		*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")), Bake.Samples[0].Frame, Bake.Samples.Last().Frame,
		FMath::Max(1, Options.SubSteps), FMath::Max(0, Options.WarmUpFrames), BE.IsValid() ? *BE->GetDescriptor().VersionName : TEXT("?"),
		Extra.Num() ? *FString::Join(Extra, TEXT(", ")) : TEXT("none"));
	Spawned->Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(InfoTagPrefix); });
	Spawned->Tags.Add(FName(*(FString(InfoTagPrefix) + Info)));

	Sequencer.GetSpawnRegister().SaveDefaultSpawnableState(Twin, Sequencer.GetFocusedTemplateID(), Sequencer.GetSharedPlaybackState());

	// Respawn from the saved template, then key it.
	Sequencer.RestorePreAnimatedState();
	Sequencer.ForceEvaluate();
	const TArray<FFrameNumber> Times = KeyTimes(MovieScene, Bake.Samples);
	WriteTransformKeys(MovieScene, Twin, Times, Bake.Samples);

	Spawned = nullptr;
	for (const TWeakObjectPtr<>& Bound : Sequencer.FindBoundObjects(Twin, Sequencer.GetFocusedTemplateID()))
	{
		Spawned = Cast<ACineCameraActor>(Bound.Get());
	}
	const FGuid CameraComponentBinding = Spawned ? Sequencer.GetHandleToObject(Spawned->GetCineCameraComponent(), true) : FGuid();
	if (!CameraComponentBinding.IsValid())
	{
		Report.Message = TEXT("twin written without lens keys: its camera component could not be bound (is it spawned at the current time?)");
		return false;
	}
	WriteLensKeys(MovieScene, CameraComponentBinding, Times, Bake.Samples);

	if (Options.bLockAfterBake)
	{
		RebindCuts(MovieScene, { Bake.CameraBinding }, Twin);
	}
	Report.bLocked = IsLocked(MovieScene, Twin);
	Sequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
	Sequencer.ForceEvaluate();
	return true;
}

int32 BlackEyeFastBake::SetLocked(ULevelSequence* Sequence, const FString& CameraBindingName, bool bLocked)
{
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		return 0;
	}
	const FScopedTransaction Transaction(bLocked ? LOCTEXT("Lock", "Lock Black Eye bake") : LOCTEXT("Unlock", "Unlock Black Eye bake"));
	int32 Changed = 0;
	for (const FTwin& Found : FindTwins(*MovieScene))
	{
		if (!CameraBindingName.IsEmpty() && BindingName(*MovieScene, Found.Camera) != CameraBindingName)
		{
			continue;
		}
		// Cuts on a stale twin move too: to the current twin on lock, back to the live camera on unlock.
		TArray<FGuid> From = Found.Stale;
		if (bLocked && Found.Twin.IsValid())
		{
			From.Add(Found.Camera);
			Changed += RebindCuts(*MovieScene, From, Found.Twin);
		}
		else if (!bLocked)
		{
			if (Found.Twin.IsValid())
			{
				From.Add(Found.Twin);
			}
			Changed += RebindCuts(*MovieScene, From, Found.Camera);
		}
	}
	if (Changed > 0)
	{
		Sequence->MarkPackageDirty();
	}
	return Changed;
}

TArray<FBlackEyeBakeInfo> BlackEyeFastBake::GetBakeInfo(ULevelSequence* Sequence)
{
	TArray<FBlackEyeBakeInfo> Out;
	const UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		return Out;
	}
	const TSharedRef<UE::MovieScene::FSharedPlaybackState> State = MovieSceneHelpers::CreateTransientSharedPlaybackState(GWorld, Sequence);
	for (const FTwin& Found : FindTwins(*MovieScene))
	{
		if (!Found.Twin.IsValid())
		{
			continue; // every twin it had is gone: not baked, as far as the menu is concerned
		}
		FBlackEyeBakeInfo& Info = Out.AddDefaulted_GetRef();
		Info.CameraBindingName = BindingName(*MovieScene, Found.Camera);
		Info.TwinBindingName = BindingName(*MovieScene, Found.Twin);
		Info.bLocked = IsLocked(*MovieScene, Found.Twin);
		if (const AActor* Template = Cast<AActor>(MovieSceneHelpers::GetObjectTemplate(Sequence, Found.Twin, State)))
		{
			for (const FName& Tag : Template->Tags)
			{
				FString Text = Tag.ToString();
				if (Text.RemoveFromStart(InfoTagPrefix))
				{
					Info.Info = Text;
				}
			}
		}
	}
	return Out;
}

FBlackEyeFastBakeReport UBlackEyeFastBakeLibrary::BakeShot(ULevelSequence* Sequence, const FBlackEyeFastBakeOptions& Options)
{
	BlackEyeFastBake::FBakeOutput Bake;
	FBlackEyeFastBakeReport Report = BlackEyeFastBake::RunBake(Sequence, Options, Bake);
	if (Report.bSuccess)
	{
		Report.bSuccess = BlackEyeFastBake::WriteTwin(Sequence, Options, Bake, Report);
	}
	UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] Bake Shot %s -> %s: %s%s"), *Report.CameraLabel, *Report.TwinBindingName,
		Report.bSuccess ? TEXT("ok") : *Report.Message, Report.bLocked ? TEXT(", locked") : TEXT(""));
	return Report;
}

int32 UBlackEyeFastBakeLibrary::SetLocked(ULevelSequence* Sequence, const FString& CameraBindingName, bool bLocked)
{
	return BlackEyeFastBake::SetLocked(Sequence, CameraBindingName, bLocked);
}

TArray<FBlackEyeBakeInfo> UBlackEyeFastBakeLibrary::GetBakeInfo(ULevelSequence* Sequence)
{
	return BlackEyeFastBake::GetBakeInfo(Sequence);
}

#undef LOCTEXT_NAMESPACE
