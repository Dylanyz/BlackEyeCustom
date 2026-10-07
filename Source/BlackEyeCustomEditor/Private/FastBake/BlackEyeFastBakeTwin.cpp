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
#include "Evaluation/MovieSceneEvaluationTemplateInstance.h"
#include "Evaluation/MovieSceneSequenceHierarchy.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneFolder.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSpawnRegister.h"
#include "Sections/MovieSceneBoolSection.h"
#include "SubObjectLocator.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "UniversalObjectLocator.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ScopeExit.h"
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
	 * dead binding. So the twin is the newest ID whose binding exists, and the dead ones are stale.
	 * Changed 2026-10-07: older twins that still exist are kept as the camera's other bake cameras (the Bake Edit
	 * window's "Create new"), no longer stale. Every retag since 2026-10-06 left one live ID, so no shot carries old
	 * live twins from the bug.
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
			for (const auto& ID : Tag.Value.IDs)
			{
				const FGuid Id = ID.GetGuid();
				(MovieScene.FindBinding(Id) ? Twin.Alive : Twin.Stale).AddUnique(Id);
			}
			Twin.Twin = Twin.Alive.Num() ? Twin.Alive.Last() : FGuid();
		}
		return Out;
	}

	FGuid DefaultTwin(const UMovieScene& MovieScene, const FTwin& Twins)
	{
		if (const UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack()))
		{
			for (const UMovieSceneSection* Section : Cuts->GetAllSections())
			{
				const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
				if (Cut && Twins.Alive.Contains(Cut->GetCameraBindingID().GetGuid()))
				{
					return Cut->GetCameraBindingID().GetGuid();
				}
			}
		}
		return Twins.Twin;
	}

	/** The camera's twins as found, or an empty entry for it. */
	FTwin TwinsOf(const UMovieScene& MovieScene, const FGuid& Camera)
	{
		for (const FTwin& Found : FindTwins(MovieScene))
		{
			if (Found.Camera == Camera)
			{
				return Found;
			}
		}
		FTwin None;
		None.Camera = Camera;
		return None;
	}

	/** Which twin a bake writes: a new one, the one asked for, else the default. Invalid: make a new one. */
	FGuid ChooseTwin(const UMovieScene& MovieScene, const FTwin& Twins, const FBlackEyeFastBakeOptions& Options)
	{
		if (Options.bCreateNewTwin)
		{
			return FGuid();
		}
		if (Options.TwinBinding.IsValid() && Twins.Alive.Contains(Options.TwinBinding))
		{
			return Options.TwinBinding;
		}
		return DefaultTwin(MovieScene, Twins);
	}

	/**
	 * Leaves the camera's tag naming its live twins, with Twin last (the newest, the default when no cut plays one), so
	 * a sequence stops carrying dead ones.
	 */
	void RetagTwin(UMovieScene& MovieScene, const FGuid& Camera, const FGuid& Twin)
	{
		const FTwin Twins = TwinsOf(MovieScene, Camera);
		MovieScene.RemoveTag(TwinTag(Camera));
		for (const FGuid& Other : Twins.Alive)
		{
			if (Other != Twin)
			{
				MovieScene.TagBinding(TwinTag(Camera), UE::MovieScene::FFixedObjectBindingID(Other, MovieSceneSequenceID::Root));
			}
		}
		MovieScene.TagBinding(TwinTag(Camera), UE::MovieScene::FFixedObjectBindingID(Twin, MovieSceneSequenceID::Root));
	}

	/** "<camera>_Bake", or "<camera>_Bake2", 3... when the camera already has bake cameras. */
	FString NewTwinName(const UMovieScene& MovieScene, const FGuid& Camera)
	{
		const FString Base = MovieScene.GetObjectDisplayName(Camera).ToString() + TEXT("_Bake");
		auto Taken = [&MovieScene](const FString& Name)
		{
			for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
			{
				if (MovieScene.GetObjectDisplayName(Binding.GetObjectGuid()).ToString() == Name)
				{
					return true;
				}
			}
			return false;
		};
		FString Name = Base;
		for (int32 i = 2; Taken(Name); ++i)
		{
			Name = Base + FString::FromInt(i);
		}
		return Name;
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

	/** Index spans [first, last] of samples on consecutive frames: one per baked range. */
	TArray<TPair<int32, int32>> SampleRuns(const TArray<FSample>& Samples)
	{
		TArray<TPair<int32, int32>> Runs;
		for (int32 i = 0; i < Samples.Num(); ++i)
		{
			if (Runs.Num() && Samples[i].Frame == Samples[i - 1].Frame + 1.0)
			{
				Runs.Last().Value = i;
			}
			else
			{
				Runs.Add({ i, i });
			}
		}
		return Runs;
	}

	/**
	 * The channel's keys become the new ones, plus (with bKeep) every old key outside the spans the new keys cover,
	 * first to last key of each baked range. Old keys keep their own values and tangents.
	 */
	template <typename ChannelType, typename ValueType>
	void MergeKeys(ChannelType& Channel, const TArray<FFrameNumber>& Times, TArray<ValueType>&& Values, const TArray<TPair<int32, int32>>& Runs, bool bKeep)
	{
		if (!bKeep || Channel.GetNumKeys() == 0)
		{
			Channel.Set(Times, MoveTemp(Values));
			Channel.AutoSetTangents();
			return;
		}
		auto Data = Channel.GetData();
		const TArrayView<const FFrameNumber> OldTimes = Data.GetTimes();
		const TArrayView<const ValueType> OldValues = Data.GetValues();
		auto Replaced = [&](FFrameNumber T)
		{
			return Runs.ContainsByPredicate([&](const TPair<int32, int32>& R) { return T >= Times[R.Key] && T <= Times[R.Value]; });
		};
		TArray<FFrameNumber> OutTimes;
		TArray<ValueType> OutValues;
		OutTimes.Reserve(OldTimes.Num() + Times.Num());
		OutValues.Reserve(OldTimes.Num() + Times.Num());
		int32 Old = 0, New = 0;
		while (Old < OldTimes.Num() || New < Times.Num())
		{
			if (Old < OldTimes.Num() && Replaced(OldTimes[Old]))
			{
				++Old;
			}
			else if (New < Times.Num() && (Old >= OldTimes.Num() || Times[New] <= OldTimes[Old]))
			{
				OutTimes.Add(Times[New]);
				OutValues.Add(Values[New++]);
			}
			else
			{
				OutTimes.Add(OldTimes[Old]);
				OutValues.Add(OldValues[Old++]);
			}
		}
		Channel.Set(OutTimes, MoveTemp(OutValues));
		Channel.AutoSetTangents();
	}

	// Auto-tangent cubic keys on every frame: smooth between frames, which matters for motion blur sub-samples.
	void SetDoubleKeys(FMovieSceneDoubleChannel& Channel, const TArray<FFrameNumber>& Times, const TArray<double>& Values,
	                   const TArray<TPair<int32, int32>>& Runs, bool bKeep)
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
		MergeKeys(Channel, Times, MoveTemp(Keys), Runs, bKeep);
	}

	void SetFloatKeys(FMovieSceneFloatChannel& Channel, const TArray<FFrameNumber>& Times, const TArray<float>& Values,
	                  const TArray<TPair<int32, int32>>& Runs, bool bKeep)
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
		MergeKeys(Channel, Times, MoveTemp(Keys), Runs, bKeep);
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

	/**
	 * With bKeep, the track's one section as it is, so keys outside this bake survive (several edits baking parts of a
	 * shot add up); otherwise, or if the track isn't one section, a fresh one.
	 */
	template <typename SectionType>
	SectionType* KeepOrResetSection(UMovieSceneTrack* Track, bool bKeep)
	{
		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		SectionType* Existing = (bKeep && Sections.Num() == 1) ? Cast<SectionType>(Sections[0]) : nullptr;
		if (Existing)
		{
			Existing->Modify();
			return Existing;
		}
		return ResetToOneSection<SectionType>(Track);
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
	void WriteLensKeys(UMovieScene& MovieScene, const FGuid& ComponentBinding, const TArray<FFrameNumber>& Times, const TArray<FSample>& Samples,
	                   bool bKeep)
	{
		const TArray<TPair<int32, int32>> Runs = SampleRuns(Samples);
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
			UMovieSceneFloatSection* Section = KeepOrResetSection<UMovieSceneFloatSection>(Track, bKeep);
			SetFloatKeys(Section->GetChannel(), Times, Values, Runs, bKeep);
		}
	}

	/** The twin actor's transform: the Black Eye camera component's world transform, rotation unwound. */
	void WriteTransformKeys(UMovieScene& MovieScene, const FGuid& Twin, const TArray<FFrameNumber>& Times, const TArray<FSample>& Samples,
	                        bool bKeep)
	{
		UMovieScene3DTransformTrack* Track = MovieScene.FindTrack<UMovieScene3DTransformTrack>(Twin);
		if (!Track)
		{
			Track = MovieScene.AddTrack<UMovieScene3DTransformTrack>(Twin);
		}
		UMovieScene3DTransformSection* Section = KeepOrResetSection<UMovieScene3DTransformSection>(Track, bKeep);
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
		const TArray<TPair<int32, int32>> Runs = SampleRuns(Samples);
		// Kept keys next to a new range must be on the same winding, or the twin spins 360 deg between them: shift
		// each range by the whole turns that bring it nearest the old curve there.
		for (int32 i = 3; i < 6 && bKeep; ++i)
		{
			for (const TPair<int32, int32>& Run : Runs)
			{
				double Old = 0.0;
				if (Ch[i]->GetNumKeys() > 0 && Ch[i]->Evaluate(Times[Run.Key], Old))
				{
					const double Turns = 360.0 * FMath::RoundToDouble((Old - V[i][Run.Key]) / 360.0);
					for (int32 k = Run.Key; k <= Run.Value; ++k)
					{
						V[i][k] += Turns;
					}
				}
			}
		}
		for (int32 i = 0; i < 6; ++i)
		{
			SetDoubleKeys(*Ch[i], Times, V[i], Runs, bKeep);
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
		const FString Name = NewTwinName(MovieScene, Camera);
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

	/** How a twin was baked: date, frames, settings, Black Eye version, the components its setup carries. */
	FString MakeBakeInfo(const FBlackEyeFastBakeOptions& Options, const TArray<FSample>& Samples, const FString& Components)
	{
		const TSharedPtr<IPlugin> BE = IPluginManager::Get().FindEnabledPlugin(TEXT("Black_Eye"));
		FString Frames;
		const TArray<TPair<int32, int32>> Runs = SampleRuns(Samples);
		for (int32 i = 0; i < Runs.Num() && i < 8; ++i)
		{
			Frames += FString::Printf(TEXT("%s%.0f-%.0f"), i ? TEXT(", ") : TEXT(""), Samples[Runs[i].Key].Frame, Samples[Runs[i].Value].Frame);
		}
		if (Runs.Num() > 8)
		{
			Frames += FString::Printf(TEXT(" and %d more"), Runs.Num() - 8);
		}
		if (Options.bKeepOtherKeys)
		{
			Frames += TEXT(" (earlier keys outside kept)");
		}
		return FString::Printf(TEXT("baked %s; frames %s; substeps %d; warmup %d%s; black eye %s; components copied: %s"),
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")), *Frames, FMath::Max(1, Options.SubSteps), FMath::Max(0, Options.WarmUpFrames),
			Options.bSettleEachRange ? TEXT(", settled at each cut") : TEXT(""), BE.IsValid() ? *BE->GetDescriptor().VersionName : TEXT("?"),
			*Components);
	}

	void SetBakeInfo(AActor& Actor, const FString& Info)
	{
		Actor.Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(InfoTagPrefix); });
		Actor.Tags.Add(FName(*(FString(InfoTagPrefix) + Info)));
	}

	/** The components line of a twin's bake info, kept when a bake leaves its setup alone. */
	FString InfoComponents(const AActor& Actor)
	{
		for (const FName& Tag : Actor.Tags)
		{
			const FString Text = Tag.ToString();
			const int32 At = Text.StartsWith(InfoTagPrefix) ? Text.Find(TEXT("components copied: ")) : INDEX_NONE;
			if (At != INDEX_NONE)
			{
				return Text.Mid(At + 19);
			}
		}
		return TEXT("unknown");
	}

	/**
	 * Splits the camera cuts playing any of From at Span's edges and points the pieces inside at To: only the frames
	 * baked play the bake, the rest keep what they played. Cuts already on To are left alone, so spans locked by earlier
	 * bakes stay locked. Also unlocks a span (From the twins, To the camera).
	 */
	int32 LockSpan(UMovieScene& MovieScene, TConstArrayView<FGuid> From, const FGuid& To, const TRange<FFrameNumber>& Span)
	{
		UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack());
		if (!Cuts)
		{
			return 0;
		}
		int32 Changed = 0;
		const FFrameRate Ticks = MovieScene.GetTickResolution();
		bool bSplit = false;
		for (UMovieSceneSection* Section : TArray<UMovieSceneSection*>(Cuts->GetAllSections()))
		{
			UMovieSceneCameraCutSection* Inside = Cast<UMovieSceneCameraCutSection>(Section);
			if (!Inside || !From.Contains(Inside->GetCameraBindingID().GetGuid()) || Inside->GetCameraBindingID().GetGuid() == To
				|| !Inside->GetRange().Overlaps(Span))
			{
				continue;
			}
			// SplitSection keeps the left part in place and returns the right one (MovieSceneSection.cpp, SplitSection).
			const FFrameNumber SpanStart = Span.GetLowerBoundValue();
			if (!Inside->HasStartFrame() || Inside->GetInclusiveStartFrame() < SpanStart)
			{
				Inside = Cast<UMovieSceneCameraCutSection>(Inside->SplitSection(FQualifiedFrameTime(FFrameTime(SpanStart), Ticks), false));
				bSplit = true;
			}
			const FFrameNumber SpanEnd = Span.GetUpperBoundValue();
			if (Inside && (!Inside->HasEndFrame() || Inside->GetExclusiveEndFrame() > SpanEnd))
			{
				Inside->SplitSection(FQualifiedFrameTime(FFrameTime(SpanEnd), Ticks), false);
				bSplit = true;
			}
			if (Inside)
			{
				Inside->Modify();
				Inside->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(To));
				++Changed;
			}
		}
		if (bSplit)
		{
			Cuts->RearrangeAllSections(); // sorted by time, as Sequencer keeps them
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

	// The bake has put the playhead back where the user left it, which may be outside the shot, where neither the
	// camera nor the twin is spawned. Write at the first baked frame, and put the playhead back afterwards. Checked
	// here, before anything is modified: cancelling a transaction doesn't revert what it already changed
	// (UTransBuffer::Cancel, EditorTransaction.cpp:1411-1453), so a failure past this point would leave a half twin.
	const FFrameTime UserTime = Sequencer.GetLocalTime().Time;
	ON_SCOPE_EXIT
	{
		Sequencer.SetLocalTimeDirectly(UserTime);
		Sequencer.ForceEvaluate();
	};
	Sequencer.SetLocalTimeDirectly(FFrameRate::TransformTime(FFrameTime::FromDecimal(Bake.Samples[0].Frame), MovieScene.GetDisplayRate(), MovieScene.GetTickResolution()));
	Sequencer.ForceEvaluate();
	bool bCameraSpawned = false;
	for (const TWeakObjectPtr<>& Bound : Sequencer.FindBoundObjects(Bake.CameraBinding, Sequencer.GetFocusedTemplateID()))
	{
		bCameraSpawned |= Bound.IsValid();
	}
	if (!bCameraSpawned)
	{
		Report.Message = TEXT("the Black Eye camera isn't spawned at the first baked frame; nothing was written");
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("FastBake", "Black Eye Fast Bake"));
	Sequence->Modify();
	MovieScene.Modify();

	// Find this camera's twin (the one asked for, else the one its cuts play, else the newest), or make it.
	const FTwin Twins = TwinsOf(MovieScene, Bake.CameraBinding);
	FGuid Twin = ChooseTwin(MovieScene, Twins, Options);
	const TArray<FGuid> Stale = Twins.Stale;
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
	SetBakeInfo(*Spawned, MakeBakeInfo(Options, Bake.Samples, Extra.Num() ? FString::Join(Extra, TEXT(", ")) : TEXT("none")));

	Sequencer.GetSpawnRegister().SaveDefaultSpawnableState(Twin, Sequencer.GetFocusedTemplateID(), Sequencer.GetSharedPlaybackState());

	// Respawn the twin alone from the saved template, then key it, as Sequencer does after changing a spawnable's
	// template (SequencerUtilities.cpp:5657-5661). RestorePreAnimatedState would destroy and respawn every spawnable in
	// the shot, the whole scene (~0.2 s of MetaHuman re-initialisation on a production shot, measured 2026-10-06).
	Sequencer.GetSpawnRegister().DestroySpawnedObject(Twin, Sequencer.GetFocusedTemplateID(), Sequencer.GetSharedPlaybackState());
	Sequencer.ForceEvaluate();
	const TArray<FFrameNumber> Times = KeyTimes(MovieScene, Bake.Samples);
	WriteTransformKeys(MovieScene, Twin, Times, Bake.Samples, Options.bKeepOtherKeys);

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
	WriteLensKeys(MovieScene, CameraComponentBinding, Times, Bake.Samples, Options.bKeepOtherKeys);

	ApplyLock(MovieScene, Bake.CameraBinding, Twin, Stale, Options, Bake.Samples);
	Report.bLocked = IsLocked(MovieScene, Twin);
	Sequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
	Sequencer.ForceEvaluate();
	return true;
}

void BlackEyeFastBake::ApplyLock(UMovieScene& MovieScene, const FGuid& Camera, const FGuid& Twin, TConstArrayView<FGuid> Stale,
                                 const FBlackEyeFastBakeOptions& Options, const TArray<FSample>& Samples)
{
	// A cut left on a stale twin was locked to a removed bake: it plays this one now, lock or not.
	RebindCuts(MovieScene, Stale, Twin);
	if (!Options.bLockAfterBake)
	{
		return;
	}
	// Locking plays this bake: cuts on the live camera or on its other bake cameras move to it.
	TArray<FGuid> From = TwinsOf(MovieScene, Camera).Alive;
	From.Remove(Twin);
	From.Add(Camera);
	if (!Options.bLockBakedFramesOnly)
	{
		RebindCuts(MovieScene, From, Twin);
		return;
	}
	const TArray<FFrameNumber> Times = KeyTimes(MovieScene, Samples);
	const FFrameNumber OneFrame = FFrameRate::TransformTime(FFrameTime(1), MovieScene.GetDisplayRate(), MovieScene.GetTickResolution()).CeilToFrame();
	for (const TPair<int32, int32>& Run : SampleRuns(Samples))
	{
		LockSpan(MovieScene, From, Twin, TRange<FFrameNumber>(Times[Run.Key], Times[Run.Value] + OneFrame));
	}
}

namespace BlackEyeFastBake
{
	/**
	 * A spawn track that keeps a new twin spawned for the whole shot, as Sequencer's Add > Actor gives a legacy
	 * spawnable (FLevelSequenceEditorActorSpawner::SetupDefaultsForSpawnable, LevelSequenceEditorActorSpawner.cpp:238-254).
	 */
	void AddSpawnTrack(UMovieScene& MovieScene, const FGuid& Twin)
	{
		UMovieSceneSpawnTrack* Track = MovieScene.AddTrack<UMovieSceneSpawnTrack>(Twin);
		UMovieSceneBoolSection* Section = Cast<UMovieSceneBoolSection>(Track->CreateNewSection());
		Section->GetChannel().SetDefault(true);
		Section->SetRange(TRange<FFrameNumber>::All());
		Track->AddSection(*Section);
		Track->SetObjectId(Twin);
	}

	/**
	 * The twin's camera-component binding: the child Sequencer made for it on an earlier bake, or a new one as data,
	 * located by the component's name under the spawned twin. That is how Sequencer binds a spawned actor's component
	 * (GetHandleToObject binds it with the actor as context; SubObjectLocator.cpp:14-44).
	 */
	FGuid FindOrAddCameraComponentBinding(ULevelSequence& Shot, UMovieScene& MovieScene, const FGuid& Twin, const FString& ComponentName)
	{
		for (int32 i = 0; i < MovieScene.GetPossessableCount(); ++i)
		{
			const FMovieScenePossessable& Possessable = MovieScene.GetPossessable(i);
			const UClass* Class = Possessable.GetPossessedObjectClass();
			if (Possessable.GetParent() == Twin && Class && Class->IsChildOf(UCameraComponent::StaticClass()))
			{
				return Possessable.GetGuid();
			}
		}
		// The non-const accessor lives on the base; ULevelSequence only overrides the const one, which hides it.
		FMovieSceneBindingReferences* References = static_cast<UMovieSceneSequence&>(Shot).GetBindingReferences();
		if (!References || ComponentName.IsEmpty())
		{
			return FGuid();
		}
		const FGuid Guid = MovieScene.AddPossessable(ComponentName, UCineCameraComponent::StaticClass());
		if (FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid))
		{
			Possessable->SetParent(Twin, &MovieScene);
		}
		if (FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(Twin))
		{
			Spawnable->AddChildPossessable(Guid); // legacy spawnables list their children (SequencerUtilities.cpp:3757-3761)
		}
		FUniversalObjectLocator Locator;
		Locator.AddFragment<FSubObjectLocator>(ComponentName);
		References->AddBinding(Guid, MoveTemp(Locator));
		return Guid;
	}

	/**
	 * A spawned twin never re-reads its template, so before its setup changes every spawned copy of it goes: the root,
	 * and each instance of the shot under a master. Destroying first also keeps the editor spawn register from saving a
	 * modified spawned copy back over the new template (LevelSequenceEditorSpawnRegister.cpp:116-128). The next
	 * evaluation spawns it again from the new one.
	 */
	void DestroySpawnedTwin(ISequencer& Sequencer, const ULevelSequence* Shot, const FGuid& Twin)
	{
		TArray<FMovieSceneSequenceID> IDs;
		if (Sequencer.GetRootMovieSceneSequence() == Shot)
		{
			IDs.Add(MovieSceneSequenceID::Root);
		}
		if (const FMovieSceneSequenceHierarchy* Hierarchy = Sequencer.GetEvaluationTemplate().GetHierarchy())
		{
			for (const TPair<FMovieSceneSequenceID, FMovieSceneSubSequenceData>& Pair : Hierarchy->AllSubSequenceData())
			{
				if (Pair.Value.GetLoadedSequence() == Shot)
				{
					IDs.Add(Pair.Key);
				}
			}
		}
		for (const FMovieSceneSequenceID& ID : IDs)
		{
			Sequencer.GetSpawnRegister().DestroySpawnedObject(Twin, ID, Sequencer.GetSharedPlaybackState());
		}
	}
}

bool BlackEyeFastBake::WriteTwinDirect(ULevelSequence* Shot, const FBlackEyeFastBakeOptions& Options, const FBakeOutput& Bake, ACineCameraActor* Scratch,
                                       ISequencer* Sequencer, FBlackEyeFastBakeReport& Report)
{
	if (!Shot || Bake.Samples.Num() == 0)
	{
		Report.Message = TEXT("nothing baked to write");
		return false;
	}
	UMovieScene& MovieScene = *Shot->GetMovieScene();
	// Templates are read through the shot alone, whatever Sequencer has open.
	const TSharedRef<UE::MovieScene::FSharedPlaybackState> State = MovieSceneHelpers::CreateTransientSharedPlaybackState(GWorld, Shot);

	const FTwin Twins = TwinsOf(MovieScene, Bake.CameraBinding);
	FGuid Twin = ChooseTwin(MovieScene, Twins, Options);
	const TArray<FGuid> Stale = Twins.Stale;
	const bool bNew = !Twin.IsValid();
	Shot->Modify();
	MovieScene.Modify();

	if (bNew || Options.bRefreshTwinSetup)
	{
		// The setup is what was authored: the camera's spawnable template, or the level actor of a possessable one.
		const ACineCameraActor* Setup = Cast<ACineCameraActor>(MovieSceneHelpers::GetObjectTemplate(Shot, Bake.CameraBinding, State));
		if (!Setup)
		{
			Setup = Bake.Camera.Get();
		}
		if (!Setup || !Scratch)
		{
			Report.Message = TEXT("no camera setup to copy onto the twin");
			return false;
		}
		{
			// The scratch is transient and reused for every twin, so dressing it stays out of the undo buffer. Copying
			// extra components drops any a previous camera left on it (CopyExtraComponents).
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			UCineCameraComponent* ScratchCam = Scratch->GetCineCameraComponent();
			CopyCameraSettings(Setup->GetCineCameraComponent(), ScratchCam);
			ScratchCam->SetRelativeTransform(FTransform::Identity);
			const TArray<FString> Extra = CopyExtraComponents(Setup, Scratch);
			SetBakeInfo(*Scratch, MakeBakeInfo(Options, Bake.Samples, Extra.Num() ? FString::Join(Extra, TEXT(", ")) : TEXT("none")));
		}
		if (bNew)
		{
			// A legacy spawnable, like every twin made so far: Sequencer's Add > Actor makes one too
			// (MakeNewSpawnable -> UMovieScene::AddSpawnable, SequencerUtilities.cpp:1007).
			const FString Name = NewTwinName(MovieScene, Bake.CameraBinding);
			UObject* Template = MovieSceneHelpers::MakeSpawnableTemplateFromInstance(*Scratch, &MovieScene,
				MakeUniqueObjectName(&MovieScene, ACineCameraActor::StaticClass(), FName(*Name)));
			Twin = MovieScene.AddSpawnable(Name, *Template);
			MovieScene.SetObjectDisplayName(Twin, FText::FromString(Name));
			AddSpawnTrack(MovieScene, Twin);
		}
		else
		{
			if (Sequencer)
			{
				DestroySpawnedTwin(*Sequencer, Shot, Twin);
			}
			// Sequencer's Save Default State does this from a spawned copy (LevelSequenceEditorSpawnRegister.cpp:159-196).
			MovieSceneHelpers::CopyObjectTemplate(Shot, Twin, Scratch, State);
		}
	}
	else if (AActor* Template = Cast<AActor>(MovieSceneHelpers::GetObjectTemplate(Shot, Twin, State)))
	{
		// Keys only: the setup stays and the bake info follows the keys. Only tags change, which nothing spawned reads.
		// Templates aren't made transactional (MakeSpawnableTemplateFromInstance), so the tag edit would escape undo.
		Template->SetFlags(RF_Transactional);
		Template->Modify();
		SetBakeInfo(*Template, MakeBakeInfo(Options, Bake.Samples, InfoComponents(*Template)));
	}
	RetagTwin(MovieScene, Bake.CameraBinding, Twin);
	Report.TwinBindingName = BindingName(MovieScene, Twin);

	const TArray<FFrameNumber> Times = KeyTimes(MovieScene, Bake.Samples);
	WriteTransformKeys(MovieScene, Twin, Times, Bake.Samples, Options.bKeepOtherKeys);
	const ACineCameraActor* TwinTemplate = Cast<ACineCameraActor>(MovieSceneHelpers::GetObjectTemplate(Shot, Twin, State));
	const FGuid CameraComponentBinding = FindOrAddCameraComponentBinding(*Shot, MovieScene, Twin,
		TwinTemplate && TwinTemplate->GetCineCameraComponent() ? TwinTemplate->GetCineCameraComponent()->GetName() : FString());
	if (!CameraComponentBinding.IsValid())
	{
		Report.Message = TEXT("twin written without lens keys: its camera component could not be bound");
		return false;
	}
	WriteLensKeys(MovieScene, CameraComponentBinding, Times, Bake.Samples, Options.bKeepOtherKeys);
	ApplyLock(MovieScene, Bake.CameraBinding, Twin, Stale, Options, Bake.Samples);
	Report.bLocked = IsLocked(MovieScene, Twin);
	return true;
}

int32 BlackEyeFastBake::SetShotCameraLock(ULevelSequence* Shot, const FGuid& Camera, const FGuid& Twin, bool bLock, const TRange<FFrameNumber>& Span)
{
	UMovieScene* MovieScene = Shot ? Shot->GetMovieScene() : nullptr;
	if (!MovieScene || (bLock && !Twin.IsValid()))
	{
		return 0;
	}
	const FScopedTransaction Transaction(bLock ? LOCTEXT("LockShot", "Lock Black Eye bake") : LOCTEXT("UnlockShot", "Unlock Black Eye bake"));
	const FTwin Twins = TwinsOf(*MovieScene, Camera);
	TArray<FGuid> From = Twins.Stale;
	From.Append(Twins.Alive);
	if (bLock)
	{
		From.Remove(Twin);
		From.Add(Camera);
	}
	const FGuid To = bLock ? Twin : Camera;
	const int32 Changed = Span == TRange<FFrameNumber>::All() ? RebindCuts(*MovieScene, From, To) : LockSpan(*MovieScene, From, To, Span);
	if (Changed > 0)
	{
		Shot->MarkPackageDirty();
	}
	return Changed;
}

namespace BlackEyeFastBake
{
	extern TArray<TWeakPtr<ISequencer>> OpenSequencers; // BlackEyeFastBakeMenu.cpp

	/** Every tag naming this binding loses it, as Sequencer's Delete does (ObjectBindingModel.cpp:1143-1148). */
	void UntagEverywhere(UMovieScene& MovieScene, const FGuid& Binding)
	{
		const UE::MovieScene::FFixedObjectBindingID ID(Binding, MovieSceneSequenceID::Root);
		TArray<FName> Tags;
		for (const TPair<FName, FMovieSceneObjectBindingIDs>& Tag : MovieScene.AllTaggedBindings())
		{
			if (Tag.Value.IDs.Contains(FMovieSceneObjectBindingID(ID)))
			{
				Tags.Add(Tag.Key);
			}
		}
		for (const FName& Tag : Tags)
		{
			MovieScene.UntagBinding(Tag, ID);
		}
	}

	void RemoveFromFolders(TArrayView<UMovieSceneFolder* const> Folders, const FGuid& Binding)
	{
		for (UMovieSceneFolder* Folder : Folders)
		{
			if (Folder)
			{
				if (Folder->GetChildObjectBindings().Contains(Binding))
				{
					Folder->RemoveChildObjectBinding(Binding);
				}
				RemoveFromFolders(Folder->GetChildFolders(), Binding);
			}
		}
	}

	/** The twin's spawned copies in every open Sequencer showing the shot, as root or under an edit. */
	void DestroySpawnedTwinEverywhere(const ULevelSequence* Shot, const FGuid& Twin)
	{
		OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
		for (const TWeakPtr<ISequencer>& Weak : OpenSequencers)
		{
			if (const TSharedPtr<ISequencer> Sequencer = Weak.Pin())
			{
				DestroySpawnedTwin(*Sequencer, Shot, Twin);
			}
		}
	}

	/**
	 * One binding deleted the way Sequencer's own Delete does it: untagged, its child bindings (the twin's camera
	 * component) deleted with their tracks, out of folders, the binding removed, its spawned copies destroyed
	 * (ObjectBindingModel.cpp:1135-1167, SpawnableModel.cpp:102-116, PossessableModel.cpp:217-248).
	 */
	void DeleteBinding(ULevelSequence& Shot, UMovieScene& MovieScene, const FGuid& Binding)
	{
		UntagEverywhere(MovieScene, Binding);
		TArray<FGuid> Children;
		for (int32 i = 0; i < MovieScene.GetPossessableCount(); ++i)
		{
			if (MovieScene.GetPossessable(i).GetParent() == Binding)
			{
				Children.Add(MovieScene.GetPossessable(i).GetGuid());
			}
		}
		for (const FGuid& Child : Children)
		{
			DeleteBinding(Shot, MovieScene, Child);
		}
		RemoveFromFolders(MovieScene.GetRootFolders(), Binding);
		if (MovieScene.FindSpawnable(Binding))
		{
			if (MovieScene.RemoveSpawnable(Binding))
			{
				DestroySpawnedTwinEverywhere(&Shot, Binding);
			}
		}
		else if (MovieScene.RemovePossessable(Binding))
		{
			DestroySpawnedTwinEverywhere(&Shot, Binding); // a custom spawnable binding; a plain possessable spawns nothing
			Shot.UnbindPossessableObjects(Binding);
		}
	}
}

int32 BlackEyeFastBake::DeleteShotCameraTwins(ULevelSequence* Shot, const FGuid& Camera, TConstArrayView<FGuid> Twins)
{
	UMovieScene* MovieScene = Shot ? Shot->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		return 0;
	}
	const FTwin Found = TwinsOf(*MovieScene, Camera);
	TArray<FGuid> Doomed;
	for (const FGuid& Twin : Twins.Num() ? Twins : TConstArrayView<FGuid>(Found.Alive))
	{
		if (Found.Alive.Contains(Twin))
		{
			Doomed.Add(Twin);
		}
	}
	const bool bAll = Twins.Num() == 0;
	if (Doomed.Num() == 0 && !(bAll && Found.Stale.Num()))
	{
		return 0;
	}
	const FScopedTransaction Transaction(LOCTEXT("DeleteBake", "Delete Black Eye bake"));
	Shot->Modify();
	MovieScene->Modify();
	// Unlock first: every cut on a deleted twin (and, deleting them all, on a dead one) plays the camera again.
	TArray<FGuid> From = Doomed;
	if (bAll)
	{
		From.Append(Found.Stale);
	}
	RebindCuts(*MovieScene, From, Camera);
	for (const FGuid& Twin : Doomed)
	{
		DeleteBinding(*Shot, *MovieScene, Twin);
	}
	if (bAll)
	{
		MovieScene->RemoveTag(TwinTag(Camera)); // the dead IDs too
	}
	Shot->MarkPackageDirty();
	return Doomed.Num();
}

int32 BlackEyeFastBake::DeleteBakes(ULevelSequence* Sequence, const FString& CameraBindingName)
{
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		return 0;
	}
	const FScopedTransaction Transaction(LOCTEXT("DeleteBakes", "Delete Black Eye bakes"));
	int32 Deleted = 0;
	for (const FTwin& Found : FindTwins(*MovieScene))
	{
		if (CameraBindingName.IsEmpty() || BindingName(*MovieScene, Found.Camera) == CameraBindingName)
		{
			Deleted += DeleteShotCameraTwins(Sequence, Found.Camera, {});
		}
	}
	return Deleted;
}

FString BlackEyeFastBake::DescribeCutPlay(const UMovieScene& MovieScene, const FGuid& Camera, const TRange<FFrameNumber>& Span)
{
	const FTwin Twins = TwinsOf(MovieScene, Camera);
	TSet<FGuid> Played;
	if (const UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene.GetCameraCutTrack()))
	{
		for (const UMovieSceneSection* Section : Cuts->GetAllSections())
		{
			const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
			const FGuid Guid = Cut ? Cut->GetCameraBindingID().GetGuid() : FGuid();
			if (Cut && Cut->GetRange().Overlaps(Span) && (Guid == Camera || Twins.Alive.Contains(Guid) || Twins.Stale.Contains(Guid)))
			{
				Played.Add(Guid);
			}
		}
	}
	if (Played.Num() == 0)
	{
		return TEXT("not on a camera cut here");
	}
	if (Played.Num() > 1)
	{
		return TEXT("mixed: part live Black Eye, part bake");
	}
	const FGuid Only = *Played.CreateConstIterator();
	return Only == Camera ? TEXT("the live Black Eye camera") : FString::Printf(TEXT("the bake (%s)"), *BindingName(MovieScene, Only));
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
		// Cuts on a stale twin move too: to the default twin on lock, back to the live camera on unlock. Lock leaves cuts
		// already on another of the camera's bake cameras alone (the window chose them); unlock takes them all back.
		TArray<FGuid> From = Found.Stale;
		const FGuid Default = DefaultTwin(*MovieScene, Found);
		if (bLocked && Default.IsValid())
		{
			From.Add(Found.Camera);
			Changed += RebindCuts(*MovieScene, From, Default);
		}
		else if (!bLocked)
		{
			From.Append(Found.Alive);
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
		const FGuid Default = DefaultTwin(*MovieScene, Found);
		Info.TwinBindingName = BindingName(*MovieScene, Default);
		Info.bLocked = IsLocked(*MovieScene, Default);
		if (const AActor* Template = Cast<AActor>(MovieSceneHelpers::GetObjectTemplate(Sequence, Default, State)))
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

int32 UBlackEyeFastBakeLibrary::DeleteBakes(ULevelSequence* Sequence, const FString& CameraBindingName)
{
	return BlackEyeFastBake::DeleteBakes(Sequence, CameraBindingName);
}

TArray<FBlackEyeBakeInfo> UBlackEyeFastBakeLibrary::GetBakeInfo(ULevelSequence* Sequence)
{
	return BlackEyeFastBake::GetBakeInfo(Sequence);
}

#undef LOCTEXT_NAMESPACE
