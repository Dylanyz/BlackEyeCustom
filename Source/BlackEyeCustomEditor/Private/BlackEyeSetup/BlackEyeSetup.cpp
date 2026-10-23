// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Black Eye Setup: three independent switches (Follow, LookAt, Focus) for a Black Eye camera in a sequence, each of
// which also sets that camera's Sequencer tracks, so the camera stays where it is and nothing fights it.
// The why, the rules per switch and the measurements: docs/blackeye-setup/DESIGN.md.

#include "BlackEyeSetupInternal.h"

#include "BlackEyeContract.h"
#include "FastBake/BlackEyeFastBakeInternal.h"

#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneActorReferenceChannel.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/TargetPoint.h"
#include "Editor.h"
#include "EditorUndoClient.h"
#include "EngineUtils.h"
#include "Misc/ITransaction.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneTimeHelpers.h"
#include "ScopedTransaction.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneActorReferenceSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneActorReferenceTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"

#include <type_traits>

#define LOCTEXT_NAMESPACE "BlackEyeSetup"

namespace BlackEyeFastBake
{
	extern TArray<TWeakPtr<ISequencer>> OpenSequencers; // BlackEyeFastBakeMenu.cpp
}

namespace BlackEyeSetup
{
	using namespace BlackEyeContract;

	namespace
	{
		// The CineCamera's own Tracking focus, set through the camera component (engine, not Black Eye).
		const TCHAR* FocusActorPropertyName = TEXT("ActorToTrack");
		const TCHAR* FocusActorPropertyPath = TEXT("FocusSettings.TrackingFocusSettings.ActorToTrack");

		// The first name a rig has wins. A `Heading` socket is what mdr-style rigs add for Subject Heading (DESIGN §4).
		const TCHAR* HeadingSocket = TEXT("Heading");
		const TCHAR* PelvisBones[] = { TEXT("pelvis"), TEXT("Pelvis"), TEXT("Bip01-Pelvis"), TEXT("Bip01_Pelvis"), TEXT("Bip01 Pelvis"),
		                               TEXT("hips"), TEXT("Hips"), TEXT("mixamorig:Hips"), TEXT("Hip") };
		const TCHAR* HeadBones[] = { TEXT("head"), TEXT("Head"), TEXT("Bip01-Head"), TEXT("Bip01_Head"), TEXT("Bip01 Head"),
		                             TEXT("FACIAL_L_Eye"), TEXT("mixamorig:Head") };
		constexpr double TrackerRadius = 60.0; // cm from the head a tracker may rest and still count as this character's

		/** Collects one line per change, plus warnings that need the user. */
		struct FLog
		{
			TArray<FString> Lines;
			void Add(const FString& Line) { Lines.Add(Line); }
			FString Join() const { return FString::Join(Lines, TEXT("\n")); }
		};

		ACineCameraActor* AsCamera(AActor* Actor) { return Cast<ACineCameraActor>(Actor); }
		UCineCameraComponent* Lens(AActor* Actor) { ACineCameraActor* C = AsCamera(Actor); return C ? C->GetCineCameraComponent() : nullptr; }

		FString Label(const AActor* Actor) { return Actor ? Actor->GetActorLabel() : FString(TEXT("(none)")); }

		bool IsTrackerLike(const AActor* Actor)
		{
			return Actor && (Actor->IsA<ATargetPoint>() || Actor->GetActorLabel().StartsWith(TEXT("tracker"), ESearchCase::IgnoreCase));
		}

		/** Every object Sequencer has bound to the binding now (in the focused sequence). */
		AActor* BoundActor(ISequencer& Sequencer, const FGuid& Guid)
		{
			for (const TWeakObjectPtr<>& Bound : Sequencer.FindBoundObjects(Guid, Sequencer.GetFocusedTemplateID()))
			{
				if (AActor* Actor = Cast<AActor>(Bound.Get()))
				{
					return Actor;
				}
			}
			return nullptr;
		}

		AActor* TemplateOf(ISequencer& Sequencer, ULevelSequence* Sequence, const FGuid& Guid)
		{
			return Cast<AActor>(MovieSceneHelpers::GetObjectTemplate(Sequence, Guid, Sequencer.GetSharedPlaybackState()));
		}

		bool IsBlackEyeBinding(ISequencer& Sequencer, ULevelSequence* Sequence, const FGuid& Guid)
		{
			return IsBlackEyeCamera(BoundActor(Sequencer, Guid)) || IsBlackEyeCamera(TemplateOf(Sequencer, Sequence, Guid));
		}

		/** Runs Edit on the saved setup (with Modify, for undo) and on the live copy (without, so it isn't saved back). */
		void EditBoth(const FCamera& Camera, TFunctionRef<void(AActor& Actor, bool bSetup)> Edit)
		{
			if (Camera.Setup)
			{
				Edit(*Camera.Setup, true);
			}
			if (Camera.Live && Camera.Live != Camera.Setup)
			{
				// No Modify on the spawned copy: the editor spawn register would then save the whole running copy over the
				// template on destroy or save (LevelSequenceEditorSpawnRegister.cpp:106-125, UE 5.8.3), Black Eye's
				// per-tick writes included. The template carries the change; the copy only shows it now.
				Edit(*Camera.Live, false);
			}
		}

		void ModifyIf(UObject* Object, bool bSetup)
		{
			if (Object && bSetup)
			{
				Object->Modify();
			}
		}

		// --- Tracks ---------------------------------------------------------------------------------------------------

		template <typename TrackType>
		TrackType* FindPropertyTrack(const UMovieScene& MovieScene, const FGuid& Binding, const FString& Path)
		{
			if (!Binding.IsValid())
			{
				return nullptr;
			}
			for (UMovieSceneTrack* Track : MovieScene.FindTracks(TrackType::StaticClass(), Binding))
			{
				TrackType* Typed = Cast<TrackType>(Track);
				if (Typed && Typed->GetPropertyPath().ToString() == Path)
				{
					return Typed;
				}
			}
			return nullptr;
		}

		/** The binding of one of the live camera's components; created only when asked (a spawned copy's component). */
		FGuid ComponentBinding(const FCamera& Camera, UObject* LiveComponent, bool bCreate)
		{
			return LiveComponent ? Camera.Sequencer->GetHandleToObject(LiveComponent, bCreate) : FGuid();
		}

		/** A bool property track's unkeyed sections take the value; keyed ones are left and reported. */
		void SetBoolTrackDefault(const FCamera& Camera, UObject* LiveComponent, FName Property, bool bValue, FLog& Log)
		{
			UMovieSceneBoolTrack* Track = FindPropertyTrack<UMovieSceneBoolTrack>(*Camera.MovieScene,
				ComponentBinding(Camera, LiveComponent, false), Property.ToString());
			if (!Track)
			{
				return;
			}
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				FMovieSceneBoolChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneBoolChannel>(0);
				if (!Channel)
				{
					continue;
				}
				if (Channel->GetNumKeys() > 0)
				{
					Log.Add(FString::Printf(TEXT("warning: %s is keyed in the sequence; its keys still decide it"), *Property.ToString()));
					continue;
				}
				Section->Modify();
				Channel->SetDefault(bValue);
			}
		}

		/** A vector property track's unkeyed channels take the value; keyed ones are left and reported. */
		void SetVectorTrackDefault(const FCamera& Camera, UObject* LiveComponent, FName Property, const FVector& Value, FLog& Log)
		{
			UMovieSceneDoubleVectorTrack* Track = FindPropertyTrack<UMovieSceneDoubleVectorTrack>(*Camera.MovieScene,
				ComponentBinding(Camera, LiveComponent, false), Property.ToString());
			if (!Track)
			{
				return;
			}
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				const TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
				bool bKeyed = false;
				Section->Modify();
				for (int32 Axis = 0; Axis < 3 && Axis < Channels.Num(); ++Axis)
				{
					if (Channels[Axis]->GetNumKeys() > 0)
					{
						bKeyed = true;
						continue;
					}
					Channels[Axis]->SetDefault(Value[Axis]);
				}
				if (bKeyed)
				{
					Log.Add(FString::Printf(TEXT("warning: %s is keyed in the sequence; its keys still decide it"), *Property.ToString()));
				}
			}
		}

		/** A float property track's unkeyed sections take the value; keyed ones are left and reported. */
		void SetFloatTrackDefault(const FCamera& Camera, UObject* LiveComponent, const FString& Path, float Value, FLog& Log)
		{
			UMovieSceneFloatTrack* Track = FindPropertyTrack<UMovieSceneFloatTrack>(*Camera.MovieScene, ComponentBinding(Camera, LiveComponent, false), Path);
			for (UMovieSceneSection* Section : Track ? Track->GetAllSections() : TArray<UMovieSceneSection*>())
			{
				FMovieSceneFloatChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
				if (!Channel)
				{
					continue;
				}
				if (Channel->GetNumKeys() > 0)
				{
					Log.Add(FString::Printf(TEXT("warning: %s is keyed in the sequence; its keys still decide it"), *Path));
					continue;
				}
				Section->Modify();
				Channel->SetDefault(Value);
			}
		}

		/**
		 * Black Eye's own focus off, in the sequence too. A Set Focus Distance track (Black Eye's panel or a Details key adds
		 * one) applies its default every frame, so writing the property alone didn't stick: measured 2026-10-23, Focus off
		 * left Black Eye's focus running, the window still read Focus on, focus locked at 1.5 cm, and with LookAt off the
		 * aim drifted toward that 1.5 cm virtual target (BlackEyeLookUtils.cpp:313-335).
		 */
		void BlackEyeFocusOffInTracks(const FCamera& Camera, FLog& Log)
		{
			SetBoolTrackDefault(Camera, GetLookAtComponent(Camera.Live), Names::SetFocusDistance, false, Log);
		}

		/** Where a switch takes effect: the whole shot (defaults), or from a frame on (keys). Tick times of the focused sequence. */
		struct FWhen
		{
			bool bKeyed = false;
			FFrameNumber From;  // the playhead
			FFrameNumber Start; // the playback start: an unkeyed track keeps its old value from here to From
		};

		template <typename TrackType>
		TrackType* FindOrAddPropertyTrack(const FCamera& Camera, UObject* LiveComponent, FName Name, const FString& Path)
		{
			const FGuid Binding = ComponentBinding(Camera, LiveComponent, true);
			if (!Binding.IsValid())
			{
				return nullptr;
			}
			TrackType* Track = FindPropertyTrack<TrackType>(*Camera.MovieScene, Binding, Path);
			if (!Track)
			{
				Track = Camera.MovieScene->AddTrack<TrackType>(Binding);
				Track->SetPropertyNameAndPath(Name, Path);
				if constexpr (std::is_same_v<TrackType, UMovieSceneDoubleVectorTrack>)
				{
					Track->SetNumChannelsUsed(3);
				}
			}
			if (Track->GetAllSections().Num() == 0)
			{
				UMovieSceneSection* Section = Track->CreateNewSection();
				Section->SetRange(TRange<FFrameNumber>::All());
				Track->AddSection(*Section);
			}
			return Track;
		}

		/** Keys a bool property at When.From; an unkeyed track first keeps the old value from the playback start. */
		void KeyBool(const FCamera& Camera, UObject* LiveComponent, FName Property, bool bOld, bool bNew, const FWhen& When)
		{
			UMovieSceneBoolTrack* Track = FindOrAddPropertyTrack<UMovieSceneBoolTrack>(Camera, LiveComponent, Property, Property.ToString());
			for (UMovieSceneSection* Section : Track ? Track->GetAllSections() : TArray<UMovieSceneSection*>())
			{
				FMovieSceneBoolChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneBoolChannel>(0);
				if (!Channel)
				{
					continue;
				}
				Section->Modify();
				TMovieSceneChannelData<bool> Data = Channel->GetData();
				if (Data.GetTimes().Num() == 0)
				{
					Data.UpdateOrAddKey(When.Start, bOld);
				}
				Data.UpdateOrAddKey(When.From, bNew);
			}
		}

		/** Keys float or double channels at When.From with constant (stepped) keys, the old values kept from the start. */
		template <typename ChannelType>
		void KeyConstant(UMovieSceneSection& Section, int32 Count, const double* Old, const double* New, const FWhen& When)
		{
			const TArrayView<ChannelType*> Channels = Section.GetChannelProxy().GetChannels<ChannelType>();
			Section.Modify();
			for (int32 i = 0; i < Count && i < Channels.Num(); ++i)
			{
				auto Data = Channels[i]->GetData();
				if (Data.GetTimes().Num() == 0)
				{
					Channels[i]->AddConstantKey(When.Start, Old[i]);
				}
				const int32 Existing = Data.FindKey(When.From);
				if (Existing != INDEX_NONE)
				{
					Data.RemoveKey(Existing);
				}
				Channels[i]->AddConstantKey(When.From, New[i]);
			}
		}

		void KeyFloat(const FCamera& Camera, UObject* LiveComponent, FName Name, const FString& Path, double Old, double New, const FWhen& When)
		{
			if (UMovieSceneFloatTrack* Track = FindOrAddPropertyTrack<UMovieSceneFloatTrack>(Camera, LiveComponent, Name, Path))
			{
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					KeyConstant<FMovieSceneFloatChannel>(*Section, 1, &Old, &New, When);
				}
			}
		}

		void KeyVector(const FCamera& Camera, UObject* LiveComponent, FName Property, const FVector& Old, const FVector& New, const FWhen& When)
		{
			if (UMovieSceneDoubleVectorTrack* Track = FindOrAddPropertyTrack<UMovieSceneDoubleVectorTrack>(Camera, LiveComponent, Property, Property.ToString()))
			{
				const double OldValues[3] = { Old.X, Old.Y, Old.Z };
				const double NewValues[3] = { New.X, New.Y, New.Z };
				for (UMovieSceneSection* Section : Track->GetAllSections())
				{
					KeyConstant<FMovieSceneDoubleChannel>(*Section, 3, OldValues, NewValues, When);
				}
			}
		}

		/** The value a keyed bool property track gives at a time; unset when the track is missing or has no keys. */
		TOptional<bool> KeyedBoolAt(const FCamera& Camera, UObject* LiveComponent, FName Property, FFrameNumber Time)
		{
			if (!LiveComponent || !Camera.Sequencer)
			{
				return {};
			}
			const UMovieSceneBoolTrack* Track = FindPropertyTrack<UMovieSceneBoolTrack>(*Camera.MovieScene,
				Camera.Sequencer->GetHandleToObject(LiveComponent, false), Property.ToString());
			for (UMovieSceneSection* Section : Track ? Track->GetAllSections() : TArray<UMovieSceneSection*>())
			{
				const FMovieSceneBoolChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneBoolChannel>(0);
				bool bValue = false;
				if (Channel && Channel->GetNumKeys() > 0 && Section->GetRange().Contains(Time) && Channel->Evaluate(FFrameTime(Time), bValue))
				{
					return bValue;
				}
			}
			return {};
		}

		/**
		 * The binding a reference key points at: the subject's own binding in this sequence, made if it has none. A
		 * subject spawned by another sequence can't be referenced from here.
		 */
		FGuid SubjectBinding(const FCamera& Camera, AActor* Subject, FLog& Log)
		{
			FGuid Guid = Camera.Sequencer->FindObjectId(*Subject, Camera.Sequencer->GetFocusedTemplateID());
			if (Guid.IsValid())
			{
				return Guid;
			}
			if (Subject->HasAnyFlags(RF_Transient))
			{
				Log.Add(FString::Printf(TEXT("warning: %s is spawned by another sequence, so its reference can't be keyed here; "
				                             "the camera may lose it in renders"), *Label(Subject)));
				return FGuid();
			}
			Guid = Camera.Sequencer->GetHandleToObject(Subject, true);
			if (Guid.IsValid())
			{
				Log.Add(FString::Printf(TEXT("bound %s in the sequence, so the camera's reference to it survives renders"), *Label(Subject)));
			}
			return Guid;
		}

		/**
		 * A spawnable camera's reference to a level actor is lost when it spawns in PIE or a render (Black Eye manual,
		 * "Spawnable Black Eye Cameras in Sequencer"); a reference track keyed to the subject's binding brings it back.
		 * Level (possessable) cameras keep their references, so they get none.
		 */
		void SetReferenceTrack(const FCamera& Camera, UObject* LiveComponent, const FString& PropertyName, const FString& Path,
		                       AActor* Subject, FLog& Log)
		{
			if (!Camera.bSpawnable || !LiveComponent)
			{
				return;
			}
			const FGuid Component = ComponentBinding(Camera, LiveComponent, Subject != nullptr);
			UMovieSceneActorReferenceTrack* Track = FindPropertyTrack<UMovieSceneActorReferenceTrack>(*Camera.MovieScene, Component, Path);
			if (!Subject)
			{
				if (Track)
				{
					Camera.MovieScene->RemoveTrack(*Track);
					Log.Add(FString::Printf(TEXT("removed the %s reference track"), *Path));
				}
				return;
			}
			const FGuid SubjectGuid = SubjectBinding(Camera, Subject, Log);
			if (!SubjectGuid.IsValid() || !Component.IsValid())
			{
				return;
			}
			if (!Track)
			{
				Track = Camera.MovieScene->AddTrack<UMovieSceneActorReferenceTrack>(Component);
				Track->SetPropertyNameAndPath(FName(PropertyName), Path);
				UMovieSceneSection* Section = Track->CreateNewSection();
				Section->SetRange(TRange<FFrameNumber>::All());
				Track->AddSection(*Section);
			}
			const FMovieSceneActorReferenceKey Key{ UE::MovieScene::FRelativeObjectBindingID(SubjectGuid) };
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				FMovieSceneActorReferenceChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneActorReferenceChannel>(0);
				if (!Channel)
				{
					continue;
				}
				Section->Modify();
				Channel->SetDefault(Key);
				// Keys an older setup left (one per subject at the shot's start) point at the new subject too.
				TMovieSceneChannelData<FMovieSceneActorReferenceKey> Data = Channel->GetData();
				for (FMovieSceneActorReferenceKey& Value : Data.GetValues())
				{
					Value = Key;
				}
			}
			Log.Add(FString::Printf(TEXT("%s keyed to %s"), *Path, *Label(Subject)));
		}

		/** Keys a subject reference at When.From; an unkeyed track first keeps the old subject from the playback start. */
		void KeyReference(const FCamera& Camera, UObject* LiveComponent, const FString& Name, const FString& Path, AActor* Old, AActor* New,
		                  const FWhen& When, FLog& Log)
		{
			const FGuid OldGuid = Old ? SubjectBinding(Camera, Old, Log) : FGuid();
			const FGuid NewGuid = New ? SubjectBinding(Camera, New, Log) : FGuid();
			UMovieSceneActorReferenceTrack* Track = (OldGuid.IsValid() && NewGuid.IsValid())
				? FindOrAddPropertyTrack<UMovieSceneActorReferenceTrack>(Camera, LiveComponent, FName(Name), Path) : nullptr;
			if (!Track)
			{
				Log.Add(FString::Printf(TEXT("warning: %s could not be keyed"), *Path));
				return;
			}
			const FMovieSceneActorReferenceKey OldKey{ UE::MovieScene::FRelativeObjectBindingID(OldGuid) };
			const FMovieSceneActorReferenceKey NewKey{ UE::MovieScene::FRelativeObjectBindingID(NewGuid) };
			for (UMovieSceneSection* Section : Track->GetAllSections())
			{
				if (FMovieSceneActorReferenceChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneActorReferenceChannel>(0))
				{
					Section->Modify();
					TMovieSceneChannelData<FMovieSceneActorReferenceKey> Data = Channel->GetData();
					if (Data.GetTimes().Num() == 0)
					{
						Data.UpdateOrAddKey(When.Start, OldKey);
					}
					Data.UpdateOrAddKey(When.From, NewKey);
				}
			}
			Log.Add(FString::Printf(TEXT("%s keyed to %s from the playhead"), *Path, *Label(New)));
		}

		/** The camera's Transform track; a spawnable gets one if it has none (renders want one: DESIGN §5). */
		UMovieScene3DTransformTrack* TransformTrack(const FCamera& Camera, FLog& Log)
		{
			UMovieScene3DTransformTrack* Track = Camera.MovieScene->FindTrack<UMovieScene3DTransformTrack>(Camera.Binding);
			if (!Track && Camera.bSpawnable)
			{
				Track = Camera.MovieScene->AddTrack<UMovieScene3DTransformTrack>(Camera.Binding);
				UMovieSceneSection* Section = Track->CreateNewSection();
				Section->SetRange(TRange<FFrameNumber>::All());
				Track->AddSection(*Section);
				Log.Add(TEXT("added a Transform track (renders need one on a spawnable camera)"));
			}
			return Track;
		}

		/**
		 * The camera's Transform track drives only what no switch drives: Location while Follow is off, Rotation while
		 * LookAt is off and Follow leaves rotation alone. Channels taken back get the camera's current place as their
		 * value where they have no keys, so turning a switch off leaves the camera where it is. Scale is untouched.
		 * BE-NATIVE: Black Eye would skip the tracks' channels its own switches drive (docs.md "Camera jitters").
		 */
		void ApplyTransform(const FCamera& Camera, bool bLocation, bool bRotation, const FWhen& When, FLog& Log)
		{
			UMovieScene3DTransformTrack* Track = TransformTrack(Camera, Log);
			const USceneComponent* Root = Camera.Live ? Camera.Live->GetRootComponent() : nullptr;
			if (!Track || !Root)
			{
				return;
			}
			if (When.bKeyed)
			{
				// From the playhead: split the section there; only the part from the playhead on changes. Channel masks are
				// per section, so this is how a switch keyed mid-shot gets its own Transform channels.
				const FFrameRate TickRate = Camera.MovieScene->GetTickResolution();
				for (UMovieSceneSection* Section : TArray<UMovieSceneSection*>(Track->GetAllSections()))
				{
					const TRange<FFrameNumber> Range = Section->GetRange();
					const bool bInside = Range.Contains(When.From) && (!Range.HasLowerBound() || Range.GetLowerBoundValue() < When.From);
					if (bInside && Section->SplitSection(FQualifiedFrameTime(FFrameTime(When.From), TickRate), false))
					{
						Log.Add(TEXT("split the Transform section at the playhead"));
					}
				}
			}
			const FVector Location = Root->GetRelativeLocation();
			const FRotator Rotation = Root->GetRelativeRotation();
			const double RotationValues[3] = { Rotation.Roll, Rotation.Pitch, Rotation.Yaw }; // channels X, Y, Z
			bool bKeyedLocation = false;
			bool bKeyedRotation = false;
			for (UMovieSceneSection* Base : Track->GetAllSections())
			{
				UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Base);
				const TRange<FFrameNumber> Range = Section ? Section->GetRange() : TRange<FFrameNumber>::Empty();
				if (!Section || (When.bKeyed && (!Range.HasLowerBound() || Range.GetLowerBoundValue() < When.From)))
				{
					continue;
				}
				Section->Modify();
				if (!Section->IsActive())
				{
					// An older fix deactivated the whole section; channels now say what it drives.
					Section->SetIsActive(true);
					Log.Add(TEXT("re-activated the Transform section (its channels now follow the switches)"));
				}
				const EMovieSceneTransformChannel Kept = Section->GetMask().GetChannels()
					& (EMovieSceneTransformChannel::Scale | EMovieSceneTransformChannel::Weight);
				Section->SetMask(Kept
					| (bLocation ? EMovieSceneTransformChannel::Translation : EMovieSceneTransformChannel::None)
					| (bRotation ? EMovieSceneTransformChannel::Rotation : EMovieSceneTransformChannel::None));

				const TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
				if (Channels.Num() < 6)
				{
					continue;
				}
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (bLocation)
					{
						Channels[Axis]->GetNumKeys() > 0 ? void(bKeyedLocation = true) : Channels[Axis]->SetDefault(Location[Axis]);
					}
					if (bRotation)
					{
						Channels[3 + Axis]->GetNumKeys() > 0 ? void(bKeyedRotation = true) : Channels[3 + Axis]->SetDefault(RotationValues[Axis]);
					}
				}
			}
			Log.Add(FString::Printf(TEXT("Transform track: Location %s, Rotation %s"),
				bLocation ? TEXT("on") : TEXT("off (Follow drives it)"),
				bRotation ? TEXT("on") : TEXT("off (Black Eye drives it)")));
			if (bKeyedLocation)
			{
				Log.Add(TEXT("note: Transform Location has keys; they drive the camera now"));
			}
			if (bKeyedRotation)
			{
				Log.Add(TEXT("note: Transform Rotation has keys; they drive the camera now"));
			}
		}

		// --- Black Eye state --------------------------------------------------------------------------------------------

		bool ReadSwitch(const AActor* Camera, bool bFollow)
		{
			bool bOn = false;
			GetBool(bFollow ? GetFollowComponent(Camera) : GetLookAtComponent(Camera), bFollow ? Names::EnableFollow : Names::EnableLookAt, bOn);
			return bOn;
		}

		bool IsPivotManual(const AActor* Camera)
		{
			FString Mode;
			return GetEnumName(GetFollowComponent(Camera), Names::FinalOrientationMode, Mode) && Mode.Contains(TEXT("Manual"));
		}

		FBlackEyeSetupSubject ToSubject(const FSubjectValue& Value)
		{
			FBlackEyeSetupSubject Out;
			Out.Actor = Value.Actor;
			Out.Component = Value.Component;
			Out.Bone = Value.Bone;
			return Out;
		}

		USceneComponent* NamedComponent(AActor* Actor, const FString& Name)
		{
			if (!Actor)
			{
				return nullptr;
			}
			if (!Name.IsEmpty())
			{
				TArray<USceneComponent*> Components;
				Actor->GetComponents(Components);
				for (USceneComponent* Component : Components)
				{
					if (Component->GetName() == Name)
					{
						return Component;
					}
				}
			}
			return Actor->GetRootComponent();
		}

		/**
		 * The frame Follow measures its offset in, as Black Eye builds it for one subject (FollowComponent.cpp:141-233,
		 * Black Eye 2.0.7): the subject's bone or component, oriented by Orientation Reference Mode, then Pitch/Yaw
		 * Offset. False for setups this doesn't model (several subjects, chase mode): the offset is then left as it is.
		 * BE-NATIVE: GetTargetTransform, exposed (BlackEyeFollowComponent.cpp:141).
		 */
		bool FollowFrame(UObject* Follow, FTransform& Out)
		{
			FSubjectValue Subject;
			bool bMultiple = false;
			bool bChase = false;
			GetBool(Follow, Names::UseMultipleTargets, bMultiple);
			GetBool(Follow, Names::ChaseFollowMode, bChase);
			if (!ReadSubject(Follow, Names::Target0, Subject) || !Subject.Actor || bMultiple || bChase)
			{
				return false;
			}
			const USceneComponent* Component = NamedComponent(Subject.Actor, Subject.Component);
			if (!Component)
			{
				return false;
			}
			const FName Socket(*Subject.Bone);
			const FTransform Pose = (!Subject.Bone.IsEmpty() && Component->DoesSocketExist(Socket))
				? Component->GetSocketTransform(Socket) : Component->GetComponentTransform();
			const FQuat Rotation = Pose.GetRotation();

			FString Mode;
			GetEnumName(Follow, Names::OrientationReferenceMode, Mode);
			FVector Forward = FVector::ForwardVector;
			FVector Up = FVector::UpVector;
			if (Mode.Contains(TEXT("Heading")))
			{
				Forward = Rotation.GetForwardVector();
				Forward.Z = 0.0;
				if (Forward.SquaredLength() <= UE_KINDA_SMALL_NUMBER)
				{
					Forward = Rotation.GetUpVector();
					Forward.Z = 0.0;
				}
				Forward.Normalize();
			}
			else if (Mode.Contains(TEXT("Target")))
			{
				Forward = Rotation.GetForwardVector();
				Up = Rotation.GetUpVector();
			}
			double Pitch = 0.0;
			double Yaw = 0.0;
			GetFloat(Follow, Names::PitchOffset, Pitch);
			GetFloat(Follow, Names::YawOffset, Yaw);
			const FQuat Frame = FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat() * FQuat(FRotator(Pitch, Yaw, 0.0));
			Out = FTransform(Frame, Pose.GetLocation());
			return true;
		}

		// --- Characters -------------------------------------------------------------------------------------------------

		/** A character's mesh and the bone Follow should use: its Heading socket, else its pelvis. */
		bool FindFollowBone(AActor* Character, USkeletalMeshComponent*& OutMesh, FString& OutBone, bool& bOutHeading)
		{
			TArray<USkeletalMeshComponent*> Meshes;
			if (Character)
			{
				Character->GetComponents(Meshes);
			}
			// A MetaHuman's body is "Body"; look there first.
			Meshes.StableSort([](const USkeletalMeshComponent& A, const USkeletalMeshComponent& B) { return A.GetName() == TEXT("Body") && B.GetName() != TEXT("Body"); });
			for (USkeletalMeshComponent* Mesh : Meshes)
			{
				if (Mesh->DoesSocketExist(HeadingSocket))
				{
					OutMesh = Mesh;
					OutBone = HeadingSocket;
					bOutHeading = true;
					return true;
				}
			}
			for (USkeletalMeshComponent* Mesh : Meshes)
			{
				for (const TCHAR* Bone : PelvisBones)
				{
					if (Mesh->DoesSocketExist(Bone))
					{
						OutMesh = Mesh;
						OutBone = Bone;
						bOutHeading = false;
						return true;
					}
				}
			}
			return false;
		}

		bool FindHead(AActor* Character, USkeletalMeshComponent*& OutMesh, FString& OutBone)
		{
			TArray<USkeletalMeshComponent*> Meshes;
			if (Character)
			{
				Character->GetComponents(Meshes);
			}
			// The body first, as for Follow: a MetaHuman's Face also has a head bone, but the body drives it.
			Meshes.StableSort([](const USkeletalMeshComponent& A, const USkeletalMeshComponent& B) { return A.GetName() == TEXT("Body") && B.GetName() != TEXT("Body"); });
			for (const TCHAR* Bone : HeadBones)
			{
				for (USkeletalMeshComponent* Mesh : Meshes)
				{
					if (Mesh->DoesSocketExist(Bone))
					{
						OutMesh = Mesh;
						OutBone = Bone;
						return true;
					}
				}
			}
			return false;
		}

		/** The character's tracker: one attached to it, else one resting within TrackerRadius of its head. */
		AActor* FindTracker(AActor* Character)
		{
			if (!Character)
			{
				return nullptr;
			}
			TArray<AActor*> Attached;
			Character->GetAttachedActors(Attached, true, true);
			for (AActor* Actor : Attached)
			{
				if (IsTrackerLike(Actor))
				{
					return Actor;
				}
			}
			USkeletalMeshComponent* Mesh = nullptr;
			FString Bone;
			if (!FindHead(Character, Mesh, Bone) || !Character->GetWorld())
			{
				return nullptr;
			}
			const FVector Head = Mesh->GetSocketLocation(FName(*Bone));
			AActor* Best = nullptr;
			double BestDistance = TrackerRadius;
			for (TActorIterator<AActor> It(Character->GetWorld()); It; ++It)
			{
				if (*It != Character && IsTrackerLike(*It))
				{
					const double Distance = FVector::Dist(It->GetActorLocation(), Head);
					if (Distance < BestDistance)
					{
						BestDistance = Distance;
						Best = *It;
					}
				}
			}
			return Best;
		}

		const TCHAR* SwitchName(EBlackEyeSetupSwitch Switch)
		{
			switch (Switch)
			{
			case EBlackEyeSetupSwitch::Follow: return TEXT("Follow");
			case EBlackEyeSetupSwitch::LookAt: return TEXT("LookAt");
			default: return TEXT("Focus");
			}
		}

		/**
		 * Undo / redo of a setup step restores the template (it was Modify'd) but not the spawned copy, which was edited
		 * without Modify (EditBoth). Measured 2026-10-21: after an undo the template read LookAt on, the spawned copy off,
		 * even after scrubbing. So after a setup step is undone or redone, every spawned Black Eye camera in the open
		 * sequences is respawned from its template, as Sequencer does after changing a template (SequencerUtilities.cpp
		 * :5657-5661). A respawned camera starts with a snap (SnapFollow, BlackEyeFollowComponentBase.cpp:44).
		 */
		class FUndoWatcher : public FSelfRegisteringEditorUndoClient
		{
		public:
			virtual bool MatchesContext(const FTransactionContext& Context, const TArray<TPair<UObject*, FTransactionObjectEvent>>&) const override
			{
				// The plugin's own steps, and a script's wrapper around them (mdr_tools' blackeye.setup verb titles its
				// transaction "MDR blackeye.setup <run>"; the plugin's steps nest inside it).
				const FString Title = Context.Title.ToString();
				return Title.StartsWith(TEXT("Black Eye Setup")) || Title.Contains(TEXT("blackeye.setup"));
			}
			virtual void PostUndo(bool bSuccess) override { Respawn(); }
			virtual void PostRedo(bool bSuccess) override { Respawn(); }

		private:
			static void Respawn()
			{
				BlackEyeFastBake::RunNextTick([]()
				{
					for (const TWeakPtr<ISequencer>& Weak : BlackEyeFastBake::OpenSequencers)
					{
						const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
						ULevelSequence* Sequence = Sequencer ? Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()) : nullptr;
						if (!Sequence)
						{
							continue;
						}
						bool bAny = false;
						for (const FMovieSceneBinding& Binding : AsConst(*Sequence->GetMovieScene()).GetBindings())
						{
							const FGuid Guid = Binding.GetObjectGuid();
							if (IsBlackEyeCamera(TemplateOf(*Sequencer, Sequence, Guid)) && BoundActor(*Sequencer, Guid))
							{
								Sequencer->GetSpawnRegister().DestroySpawnedObject(Guid, Sequencer->GetFocusedTemplateID(), Sequencer->GetSharedPlaybackState());
								bAny = true;
							}
						}
						if (bAny)
						{
							Sequencer->ForceEvaluate();
						}
					}
				});
			}
		};

		TUniquePtr<FUndoWatcher> Watcher; // made on first use: GEditor doesn't exist yet when the module starts

		void Changed(const FCamera& Camera)
		{
			if (!Watcher && GEditor)
			{
				Watcher = MakeUnique<FUndoWatcher>();
			}
			Camera.Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
		}
	}

	void ShutdownUndo()
	{
		Watcher.Reset();
	}

	// --- Finding the camera -----------------------------------------------------------------------------------------------

	TArray<FString> ListCameras(const TSharedPtr<ISequencer>& Sequencer)
	{
		TArray<FString> Out;
		ULevelSequence* Sequence = Sequencer ? Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()) : nullptr;
		if (!Sequence)
		{
			return Out;
		}
		const UMovieScene* MovieScene = Sequence->GetMovieScene();
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			if (IsBlackEyeBinding(*Sequencer, Sequence, Binding.GetObjectGuid()))
			{
				Out.AddUnique(MovieScene->GetObjectDisplayName(Binding.GetObjectGuid()).ToString());
			}
		}
		return Out;
	}

	bool FindCamera(const TSharedPtr<ISequencer>& Sequencer, const FString& Name, FCamera& Out, FString& OutError)
	{
		Out = FCamera();
		ULevelSequence* Sequence = Sequencer ? Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()) : nullptr;
		if (!Sequence)
		{
			OutError = TEXT("no Level Sequence is open in Sequencer");
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		auto IsCamera = [&](const FGuid& Guid) { return Guid.IsValid() && IsBlackEyeBinding(*Sequencer, Sequence, Guid); };

		FGuid Chosen;
		if (!Name.IsEmpty())
		{
			for (const FMovieSceneBinding& Binding : AsConst(*MovieScene).GetBindings())
			{
				if (MovieScene->GetObjectDisplayName(Binding.GetObjectGuid()).ToString() == Name && IsCamera(Binding.GetObjectGuid()))
				{
					Chosen = Binding.GetObjectGuid();
				}
			}
		}
		if (!Chosen.IsValid() && Name.IsEmpty())
		{
			// 1. A selected Black Eye binding, or one of its component bindings.
			TArray<FGuid> Selected;
			Sequencer->GetSelectedObjects(Selected);
			for (const FGuid& Guid : Selected)
			{
				const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid);
				const FGuid Candidate = IsCamera(Guid) ? Guid : (Possessable && IsCamera(Possessable->GetParent()) ? Possessable->GetParent() : FGuid());
				if (Candidate.IsValid())
				{
					Chosen = Candidate;
					break;
				}
			}
		}
		if (!Chosen.IsValid() && Name.IsEmpty())
		{
			// 2. The one the camera cuts play: the cut under the playhead first.
			if (const UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack()))
			{
				const FFrameNumber Now = Sequencer->GetLocalTime().Time.FrameNumber;
				for (int32 Pass = 0; Pass < 2 && !Chosen.IsValid(); ++Pass)
				{
					for (const UMovieSceneSection* Section : Cuts->GetAllSections())
					{
						const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
						const FGuid Guid = Cut ? Cut->GetCameraBindingID().GetGuid() : FGuid();
						if ((Pass == 1 || Section->GetRange().Contains(Now)) && IsCamera(Guid))
						{
							Chosen = Guid;
							break;
						}
					}
				}
			}
		}
		if (!Chosen.IsValid() && Name.IsEmpty())
		{
			// 3. The first one.
			for (const FMovieSceneBinding& Binding : AsConst(*MovieScene).GetBindings())
			{
				if (IsCamera(Binding.GetObjectGuid()))
				{
					Chosen = Binding.GetObjectGuid();
					break;
				}
			}
		}
		if (!Chosen.IsValid())
		{
			// An edit holds its cameras in its shots: say how to get to them.
			const bool bHasShots = AsConst(*MovieScene).GetTracks().ContainsByPredicate([](const UMovieSceneTrack* T) { return T && T->IsA<UMovieSceneSubTrack>(); });
			OutError = !Name.IsEmpty() ? FString::Printf(TEXT("no Black Eye camera named %s in this sequence"), *Name)
				: bHasShots ? FString(TEXT("no Black Eye camera in this sequence itself; double-click a shot to step into it"))
				: FString(TEXT("this sequence has no Black Eye camera"));
			return false;
		}

		Out.Sequencer = Sequencer;
		Out.Sequence = Sequence;
		Out.MovieScene = MovieScene;
		Out.Binding = Chosen;
		Out.Name = MovieScene->GetObjectDisplayName(Chosen).ToString();
		Out.Live = BoundActor(*Sequencer, Chosen);
		AActor* Template = TemplateOf(*Sequencer, Sequence, Chosen);
		Out.bSpawnable = Template != nullptr;
		Out.Setup = Template ? Template : Out.Live;
		if (!Out.Live)
		{
			OutError = FString::Printf(TEXT("%s isn't spawned at the playhead; move the playhead into its shot"), *Out.Name);
			return false;
		}
		return true;
	}

	// --- State ------------------------------------------------------------------------------------------------------------

	FBlackEyeSetupState ReadState(const FCamera& Camera)
	{
		FBlackEyeSetupState State;
		// What the camera is at the playhead: the live copy, which Sequencer has evaluated (keyed switches and subject
		// references included). Its setup only when nothing is spawned.
		const AActor* Setup = Camera.Live ? Camera.Live : Camera.Setup;
		if (!Setup)
		{
			State.Message = TEXT("no camera");
			return State;
		}
		State.bValid = true;
		State.CameraBindingName = Camera.Name;
		State.bSpawnable = Camera.bSpawnable;
		State.bFollow = ReadSwitch(Setup, true);
		State.bLookAt = ReadSwitch(Setup, false);

		FSubjectValue Value;
		if (ReadSubject(GetFollowComponent(Setup), Names::Target0, Value))
		{
			State.FollowSubject = ToSubject(Value);
		}
		bool bLookAtWeightZero = false; // intended while LookAt is off (SetSwitch, LookAt off)
		if (ReadSubject(GetLookAtComponent(Setup), Names::Target0, Value))
		{
			State.LookAtSubject = ToSubject(Value);
			bLookAtWeightZero = Value.Actor && Value.bHasWeight && Value.Weight <= 0.f;
		}
		const UCineCameraComponent* Cine = Lens(const_cast<AActor*>(Setup));
		bool bBlackEyeFocus = false;
		GetBool(GetLookAtComponent(Setup), Names::SetFocusDistance, bBlackEyeFocus);
		FSubjectValue BlackEyeFocus;
		ReadSubject(GetLookAtComponent(Setup), Names::FocusTrackingTarget, BlackEyeFocus);
		if (Cine && Cine->FocusSettings.FocusMethod == ECameraFocusMethod::Tracking)
		{
			State.bFocus = true;
			State.FocusSubject.Actor = Cine->FocusSettings.TrackingFocusSettings.ActorToTrack.Get();
		}
		else if (bBlackEyeFocus && BlackEyeFocus.Actor)
		{
			// Set up the Black Eye way (its own focus on the LookAt component). Works while LookAt is on.
			State.bFocus = true;
			State.FocusSubject = ToSubject(BlackEyeFocus);
			State.Warnings.Add(TEXT("focus uses Black Eye's own focus; turning Focus on again switches it to the lens's Tracking focus"));
		}
		if (!State.FocusSubject.Actor && BlackEyeFocus.Actor)
		{
			State.FocusSubject = ToSubject(BlackEyeFocus);
		}
		if (!State.FocusSubject.Actor && Cine)
		{
			State.FocusSubject.Actor = Cine->FocusSettings.TrackingFocusSettings.ActorToTrack.Get();
		}

		// A keyed switch is what its keys say at the playhead, whatever the setup's default is.
		const FFrameNumber Now = Camera.Sequencer ? Camera.Sequencer->GetLocalTime().Time.FloorToFrame() : FFrameNumber();
		if (const TOptional<bool> Keyed = KeyedBoolAt(Camera, GetFollowComponent(Camera.Live), Names::EnableFollow, Now))
		{
			State.bFollow = *Keyed;
			State.Warnings.Add(TEXT("Follow is keyed: it shows the value at the playhead, and switching it keys it there"));
		}
		if (const TOptional<bool> Keyed = KeyedBoolAt(Camera, GetLookAtComponent(Camera.Live), Names::EnableLookAt, Now))
		{
			State.bLookAt = *Keyed;
			State.Warnings.Add(TEXT("LookAt is keyed: it shows the value at the playhead, and switching it keys it there"));
		}
		if (State.bLookAt && bLookAtWeightZero)
		{
			State.Warnings.Add(TEXT("LookAt subject weight is 0, so Black Eye ignores it (turning LookAt on sets it to 1)"));
		}
		return State;
	}

	// --- Switches ---------------------------------------------------------------------------------------------------------

	FString SetSwitch(const FCamera& Camera, EBlackEyeSetupSwitch Switch, bool bOn, bool bFromPlayhead)
	{
		FLog Log;
		const FBlackEyeSetupState Before = ReadState(Camera);
		UCineCameraComponent* LiveLens = Lens(Camera.Live);
		if (!LiveLens)
		{
			return TEXT("error: not a CineCamera");
		}
		const FTransform LensWorld = LiveLens->GetComponentTransform();

		// Whole shot (defaults), or from the playhead on (keys). A switch already keyed is always keyed at the playhead:
		// a default can't beat its keys.
		FWhen When;
		When.Start = UE::MovieScene::DiscreteInclusiveLower(Camera.MovieScene->GetPlaybackRange());
		When.From = Camera.Sequencer->GetLocalTime().Time.FloorToFrame();
		const bool bAlreadyKeyed =
			(Switch == EBlackEyeSetupSwitch::Follow && KeyedBoolAt(Camera, GetFollowComponent(Camera.Live), Names::EnableFollow, When.From).IsSet()) ||
			(Switch == EBlackEyeSetupSwitch::LookAt && KeyedBoolAt(Camera, GetLookAtComponent(Camera.Live), Names::EnableLookAt, When.From).IsSet());
		if (Switch == EBlackEyeSetupSwitch::Focus && bFromPlayhead)
		{
			// CineCameraSettings.h:239 (UE 5.8.3): FocusMethod isn't Interp, so Sequencer can't key it.
			return TEXT("error: Focus switches for the whole shot only (the lens's Focus Method can't be keyed); to change focus mid-shot, "
			            "switch the Focus subject from the playhead");
		}
		When.bKeyed = (bFromPlayhead || bAlreadyKeyed) && When.From > When.Start;
		if (bFromPlayhead && !When.bKeyed)
		{
			Log.Add(TEXT("note: the playhead is at the shot's start, so this applies to the whole shot"));
		}
		const bool bTemplateFollow = ReadSwitch(Camera.Setup, true);
		const bool bTemplateLookAt = ReadSwitch(Camera.Setup, false);

		const FScopedTransaction Transaction(FText::Format(LOCTEXT("SetSwitch", "Black Eye Setup: {0} {1}"),
			FText::FromString(SwitchName(Switch)), bOn ? LOCTEXT("On", "on") : LOCTEXT("Off", "off")));
		Camera.Sequence->Modify();
		Camera.MovieScene->Modify();

		bool bFollow = Before.bFollow;
		bool bLookAt = Before.bLookAt;

		if (Switch == EBlackEyeSetupSwitch::Follow)
		{
			if (bOn && !Before.FollowSubject.Actor)
			{
				return TEXT("error: Follow has no subject; pick one first (Use Selected, or Fill from Character). For a plain camera, use All off");
			}
			// Keep the camera where it is: measure the offset from here, in the frame Follow will use. Black Eye's own
			// Details toggle does the same (BlackEyeFollowComponent.cpp:308-321), but only on a Details edit.
			FVector Offset = FVector::ZeroVector;
			FTransform Frame;
			const bool bSolved = bOn && FollowFrame(GetFollowComponent(Camera.Live), Frame);
			if (bSolved)
			{
				Offset = Frame.InverseTransformPositionNoScale(Camera.Live->GetActorLocation());
			}
			FVector OldOffset = FVector::ZeroVector;
			GetVector(GetFollowComponent(Camera.Setup), Names::FollowOffset, OldOffset);
			EditBoth(Camera, [&](AActor& Actor, bool bSetup)
			{
				UObject* Follow = GetFollowComponent(&Actor);
				ModifyIf(Follow, bSetup);
				if (When.bKeyed && bSetup)
				{
					// Keyed from the playhead: the keys carry the switch, its weight and offset; the setup keeps the shot's
					// start. Only the orientation mode below is the setup's (it matters only while Follow runs).
					if (bOn && !bLookAt)
					{
						SetEnumByName(Follow, Names::FinalOrientationMode, TEXT("FMPM_Manual"));
					}
					return;
				}
				SetBool(Follow, Names::EnableFollow, bOn);
				// A freshly spawned camera snaps Follow once even while it is off: the snap flag set in the constructor
				// (BlackEyeFollowComponentBase.cpp:44) bypasses bEnableFollow (:160-164). Measured 2026-10-21: with Follow
				// off the spawned camera sat at its Follow position until a scrub put it back on its track. Weight 0 makes
				// the subject invalid (BlackEyeWeightedTarget.h:33-35), so the snap has nothing to move to; the subject stays.
				// BE-NATIVE: skip the spawn snap while Follow is off (BlackEyeFollowComponentBase.cpp:160-164).
				FSubjectValue Subject;
				if (ReadSubject(Follow, Names::Target0, Subject) && Subject.Actor && Subject.bHasWeight)
				{
					Subject.Weight = bOn ? 1.f : 0.f;
					WriteSubject(Follow, Names::Target0, Subject);
				}
				if (bSolved)
				{
					SetVector(Follow, Names::FollowOffset, Offset);
				}
				if (bOn && !bLookAt)
				{
					// LookAt is off, so rotation is the user's: Follow must not turn the camera.
					SetEnumByName(Follow, Names::FinalOrientationMode, TEXT("FMPM_Manual"));
				}
			});
			UObject* LiveFollow = GetFollowComponent(Camera.Live);
			if (When.bKeyed)
			{
				KeyBool(Camera, LiveFollow, Names::EnableFollow, bTemplateFollow, bOn, When);
				KeyFloat(Camera, LiveFollow, TEXT("Weight"), TEXT("Target_0.Weight"), bTemplateFollow ? 1.0 : 0.0, bOn ? 1.0 : 0.0, When);
				if (bSolved)
				{
					KeyVector(Camera, LiveFollow, Names::FollowOffset, OldOffset, Offset, When);
				}
				Log.Add(FString::Printf(TEXT("Follow keyed %s from the playhead (with its subject weight%s)"), bOn ? TEXT("on") : TEXT("off"),
					bSolved ? TEXT(" and Follow Offset") : TEXT("")));
			}
			else
			{
				SetBoolTrackDefault(Camera, LiveFollow, Names::EnableFollow, bOn, Log);
				if (bSolved)
				{
					SetVectorTrackDefault(Camera, LiveFollow, Names::FollowOffset, Offset, Log);
				}
			}
			if (bSolved)
			{
				Log.Add(FString::Printf(TEXT("Follow Offset measured from where the camera is: %s"), *Offset.ToCompactString()));
			}
			else if (bOn)
			{
				Log.Add(TEXT("Follow Offset kept (several subjects or chase mode: not measured)"));
			}
			if (bOn)
			{
				SetReferenceTrack(Camera, LiveFollow, TEXT("Actor"), TEXT("Target_0.Actor"), Before.FollowSubject.Actor, Log);
				if (!bLookAt)
				{
					Log.Add(TEXT("Follow orientation set to Manual, so the camera's rotation stays yours"));
				}
			}
			bFollow = bOn;
		}
		else if (Switch == EBlackEyeSetupSwitch::LookAt)
		{
			if (bOn && !Before.LookAtSubject.Actor)
			{
				return TEXT("error: LookAt has no subject; pick one first (Use Selected, or Fill from Character). For a plain camera, use All off");
			}
			// Off: hand the aim to the actor. The root takes the lens's current world rotation and the lens's own relative
			// rotation goes to zero, so the view doesn't move and the Transform track's Rotation now aims the camera.
			// BE-NATIVE: with LookAt off Black Eye still rebuilds the lens every tick (BlackEyeLookAtComponent.cpp:522);
			// natively it would leave the lens to the actor.
			const FRotator Aim = LensWorld.Rotator();
			EditBoth(Camera, [&](AActor& Actor, bool bSetup)
			{
				UObject* LookAt = GetLookAtComponent(&Actor);
				ModifyIf(LookAt, bSetup);
				if (!(When.bKeyed && bSetup))
				{
					SetBool(LookAt, Names::EnableLookAt, bOn); // keyed: the key carries it, the setup keeps the shot's start
				}
				// Black Eye's own focus locks for good with LookAt off (skill ref lookat-off-focus.md); the Focus switch
				// uses the lens's Tracking focus instead, so Black Eye never writes focus.
				SetBool(LookAt, Names::SetFocusDistance, false);
				// Weight 1 while on, 0 while off. Off still snaps: every snap request (piloting release, camera cut, spawn)
				// holds composition, and a held LookAt aims at its real subjects whatever bEnableLookAt says
				// (BlackEyeLookUtils.cpp:295, via BlackEyeCameraSubsystem.cpp:1163 on a pilot release). Weight 0 makes the
				// subject invalid (BlackEyeWeightedTarget.h:33-35), so a held LookAt has nothing to aim at; the subject stays.
				// BE-NATIVE: don't use the subjects while LookAt is off, held or not (BlackEyeLookUtils.cpp:295).
				FSubjectValue Subject;
				if (!(When.bKeyed && bSetup) && ReadSubject(LookAt, Names::Target0, Subject) && Subject.Actor && Subject.bHasWeight)
				{
					Subject.Weight = bOn ? 1.f : 0.f;
					WriteSubject(LookAt, Names::Target0, Subject);
				}
				if (!bOn)
				{
					USceneComponent* Root = Actor.GetRootComponent();
					UCineCameraComponent* Cine = Lens(&Actor);
					ModifyIf(Root, bSetup);
					ModifyIf(Cine, bSetup);
					if (Root)
					{
						Root->SetRelativeRotation(Aim);
					}
					if (Cine && Cine != Root)
					{
						Cine->SetRelativeRotation(FRotator::ZeroRotator);
					}
					if (bFollow)
					{
						UObject* Follow = GetFollowComponent(&Actor);
						ModifyIf(Follow, bSetup);
						SetEnumByName(Follow, Names::FinalOrientationMode, TEXT("FMPM_Manual"));
					}
				}
			});
			UObject* LiveLookAt = GetLookAtComponent(Camera.Live);
			BlackEyeFocusOffInTracks(Camera, Log);
			if (When.bKeyed)
			{
				KeyBool(Camera, LiveLookAt, Names::EnableLookAt, bTemplateLookAt, bOn, When);
				KeyFloat(Camera, LiveLookAt, TEXT("Weight"), TEXT("Target_0.Weight"), bTemplateLookAt ? 1.0 : 0.0, bOn ? 1.0 : 0.0, When);
				Log.Add(FString::Printf(TEXT("LookAt keyed %s from the playhead (with its subject weight)"), bOn ? TEXT("on") : TEXT("off")));
			}
			else
			{
				SetBoolTrackDefault(Camera, LiveLookAt, Names::EnableLookAt, bOn, Log);
			}
			if (bOn)
			{
				SetReferenceTrack(Camera, LiveLookAt, TEXT("Actor"), TEXT("Target_0.Actor"), Before.LookAtSubject.Actor, Log);
			}
			else
			{
				Log.Add(FString::Printf(TEXT("aim handed to the camera's rotation: %s"), *Aim.ToCompactString()));
				if (bFollow)
				{
					Log.Add(TEXT("Follow orientation set to Manual, so Follow moves the camera but doesn't turn it"));
				}
			}
			bLookAt = bOn;
		}
		else
		{
			AActor* Subject = Before.FocusSubject.Actor;
			if (bOn && !Subject)
			{
				return TEXT("error: Focus has no subject; pick one first (Use Selected, or Fill from Character)");
			}
			// The lens works out CurrentFocusDistance only when something views it (UCineCameraComponent::GetCameraView,
			// CineCameraComponent.cpp:595, UE 5.8.3); resolve it now so Off holds the focus the camera has (Fast Bake trap 4.19).
			FMinimalViewInfo View;
			LiveLens->GetCameraView(0.f, View);
			// The first sane distance: what the lens resolves, the focus subject's depth along the lens axis, the manual
			// distance, else 10 m. Black Eye's lock leaves the lens at the 1.5 cm floor (skill ref lookat-off-focus.md), and
			// holding that kept the lock and, with LookAt off, an aim point 1.5 cm ahead (measured 2026-10-23).
			constexpr float SaneFocus = 20.f; // cm: clear of the Plate/Pedestal guard (lens offset + 10 cm)
			const float SubjectDepth = Subject
				? FVector::DotProduct(Subject->GetActorLocation() - LensWorld.GetLocation(), LensWorld.GetUnitAxis(EAxis::X)) : 0.f;
			const float Candidates[] = { LiveLens->CurrentFocusDistance, SubjectDepth, LiveLens->FocusSettings.ManualFocusDistance };
			float Distance = 1000.f;
			for (const float Candidate : Candidates)
			{
				if (Candidate > SaneFocus)
				{
					Distance = Candidate;
					break;
				}
			}
			EditBoth(Camera, [&](AActor& Actor, bool bSetup)
			{
				UCineCameraComponent* Cine = Lens(&Actor);
				UObject* LookAt = GetLookAtComponent(&Actor);
				ModifyIf(Cine, bSetup);
				ModifyIf(LookAt, bSetup);
				// The lens's own Tracking focus, worked out fresh every frame, never Black Eye's: Black Eye's locks when
				// LookAt is off (skill ref lookat-off-focus.md) and only runs while LookAt runs.
				// BE-NATIVE: Black Eye's focus block would run whether or not LookAt does (BlackEyeLookUtils.cpp:611-620).
				SetBool(LookAt, Names::SetFocusDistance, false);
				if (!Cine)
				{
					return;
				}
				if (bOn)
				{
					Cine->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
					Cine->FocusSettings.TrackingFocusSettings.ActorToTrack = Subject;
					// LookAt off aims at a point ManualFocusDistance ahead; far keeps that aim steady (lookat-off-focus.md §4A).
					Cine->FocusSettings.ManualFocusDistance = 100000.f;
				}
				else
				{
					Cine->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
					Cine->FocusSettings.ManualFocusDistance = Distance;
				}
			});
			// The sequence's own focus tracks would otherwise put Black Eye's focus and the old distance back every frame.
			BlackEyeFocusOffInTracks(Camera, Log);
			SetFloatTrackDefault(Camera, LiveLens, TEXT("FocusSettings.ManualFocusDistance"), bOn ? 100000.f : Distance, Log);
			if (bOn)
			{
				SetReferenceTrack(Camera, LiveLens, FocusActorPropertyName, FocusActorPropertyPath, Subject, Log);
				Log.Add(FString::Printf(TEXT("focus tracks %s"), *Label(Subject)));
			}
			else
			{
				Log.Add(FString::Printf(TEXT("focus set by hand, held at %.1f cm"), Distance));
			}
		}

		// The Transform track drives what no switch drives.
		const bool bManual = IsPivotManual(Camera.Setup);
		ApplyTransform(Camera, !bFollow, !bLookAt && (!bFollow || bManual), When, Log);

		Changed(Camera);
		Log.Lines.Insert(FString::Printf(TEXT("%s %s on %s%s"), SwitchName(Switch), bOn ? TEXT("on") : TEXT("off"), *Camera.Name,
			When.bKeyed ? TEXT(", from the playhead") : TEXT("")), 0);
		return Log.Join();
	}

	FString AllOff(const FCamera& Camera)
	{
		// Every switch's off path, whatever the switches read: a camera that never had a subject reads off already, but
		// Black Eye still aims its lens (LookAt off runs, BlackEyeLookAtComponent.cpp:522) and the Transform track's
		// channels may still be masked. Off needs no subject. The steps nest into this one transaction.
		const FScopedTransaction Transaction(LOCTEXT("AllOff", "Black Eye Setup: all off"));
		TArray<FString> Lines;
		for (const EBlackEyeSetupSwitch Switch : { EBlackEyeSetupSwitch::Follow, EBlackEyeSetupSwitch::LookAt, EBlackEyeSetupSwitch::Focus })
		{
			Lines.Add(SetSwitch(Camera, Switch, false));
		}
		return FString::Printf(TEXT("all off on %s: the camera is yours (Transform track Location and Rotation, manual focus)\n"), *Camera.Name)
			+ FString::Join(Lines, TEXT("\n"));
	}

	FString SetSubject(const FCamera& Camera, EBlackEyeSetupSwitch Switch, const FBlackEyeSetupSubject& Subject)
	{
		FLog Log;
		const FScopedTransaction Transaction(FText::Format(LOCTEXT("SetSubject", "Black Eye Setup: {0} subject"), FText::FromString(SwitchName(Switch))));
		Camera.Sequence->Modify();
		Camera.MovieScene->Modify();

		FSubjectValue Value;
		Value.Actor = Subject.Actor;
		Value.Component = Subject.Component;
		Value.Bone = Subject.Bone;
		Value.Weight = 1.f;

		if (Switch == EBlackEyeSetupSwitch::Focus)
		{
			// Bones can't be tracked by the lens: a bone subject focuses on that point as it is now, as an offset.
			FVector Offset = FVector::ZeroVector;
			if (Subject.Actor && !Subject.Bone.IsEmpty())
			{
				if (const USceneComponent* Component = NamedComponent(Subject.Actor, Subject.Component))
				{
					Offset = Subject.Actor->GetActorTransform().InverseTransformPosition(Component->GetSocketLocation(FName(*Subject.Bone)));
					Log.Add(TEXT("note: the lens tracks actors, not bones; focus holds the bone's offset as it is now (use a tracker to follow it)"));
				}
			}
			// The switch stays as it was. Focus set up the Black Eye way (its own focus on the LookAt component) moves to
			// the lens's Tracking focus with the new subject; Black Eye's focus target itself is left alone, unused.
			const bool bFocusOn = ReadState(Camera).bFocus;
			EditBoth(Camera, [&](AActor& Actor, bool bSetup)
			{
				UCineCameraComponent* Cine = Lens(&Actor);
				UObject* LookAt = GetLookAtComponent(&Actor);
				ModifyIf(Cine, bSetup);
				ModifyIf(LookAt, bSetup);
				SetBool(LookAt, Names::SetFocusDistance, false);
				if (Cine)
				{
					Cine->FocusSettings.TrackingFocusSettings.ActorToTrack = Subject.Actor.Get();
					Cine->FocusSettings.TrackingFocusSettings.RelativeOffset = Offset;
					if (bFocusOn)
					{
						Cine->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
					}
				}
			});
			BlackEyeFocusOffInTracks(Camera, Log);
			SetReferenceTrack(Camera, Lens(Camera.Live), FocusActorPropertyName, FocusActorPropertyPath, Subject.Actor, Log);
		}
		else
		{
			const bool bFollow = Switch == EBlackEyeSetupSwitch::Follow;
			const FBlackEyeSetupState Now = ReadState(Camera);
			if (bFollow ? !Now.bFollow : !Now.bLookAt)
			{
				Value.Weight = 0.f; // the switch is off: keep the snaps away (see SetSwitch, Follow and LookAt)
			}
			EditBoth(Camera, [&](AActor& Actor, bool bSetup)
			{
				UObject* Component = bFollow ? GetFollowComponent(&Actor) : GetLookAtComponent(&Actor);
				ModifyIf(Component, bSetup);
				WriteSubject(Component, Names::Target0, Value);
			});
			UObject* LiveComponent = bFollow ? GetFollowComponent(Camera.Live) : GetLookAtComponent(Camera.Live);
			SetReferenceTrack(Camera, LiveComponent, TEXT("Actor"), TEXT("Target_0.Actor"), Subject.Actor, Log);
		}
		Changed(Camera);
		FString Where = Label(Subject.Actor);
		if (!Subject.Component.IsEmpty() || !Subject.Bone.IsEmpty())
		{
			Where += FString::Printf(TEXT(" > %s > %s"), Subject.Component.IsEmpty() ? TEXT("root") : *Subject.Component, *Subject.Bone);
		}
		Log.Lines.Insert(FString::Printf(TEXT("%s subject: %s"), SwitchName(Switch), *Where), 0);
		return Log.Join();
	}

	FString SetSubjectFromPlayhead(const FCamera& Camera, EBlackEyeSetupSwitch Switch, const FBlackEyeSetupSubject& Subject)
	{
		FWhen When;
		When.Start = UE::MovieScene::DiscreteInclusiveLower(Camera.MovieScene->GetPlaybackRange());
		When.From = Camera.Sequencer->GetLocalTime().Time.FloorToFrame();
		When.bKeyed = true;
		const FBlackEyeSetupState Before = ReadState(Camera);
		const FBlackEyeSetupSubject& Old = Switch == EBlackEyeSetupSwitch::Follow ? Before.FollowSubject
		                                 : Switch == EBlackEyeSetupSwitch::LookAt ? Before.LookAtSubject : Before.FocusSubject;
		if (!Subject.Actor)
		{
			return TEXT("error: no subject");
		}
		if (When.From <= When.Start || !Old.Actor)
		{
			return SetSubject(Camera, Switch, Subject) + TEXT("\nnote: applied to the whole shot (the playhead is at its start, or there was no subject before)");
		}
		// Only the actor can be keyed: Component Name and Bone Name are plain strings (BlackEyeSimpleTarget.h:50, :59), and the
		// lens tracks actors only. BE-NATIVE: keyable component/bone, or one subject reference per key.
		if (Switch == EBlackEyeSetupSwitch::Focus ? !Subject.Bone.IsEmpty() : (Old.Component != Subject.Component || Old.Bone != Subject.Bone))
		{
			return Switch == EBlackEyeSetupSwitch::Focus
				? FString(TEXT("error: focus from the playhead needs an actor subject (a tracker), not a bone"))
				: FString::Printf(TEXT("error: from the playhead the new subject must use the same component and bone as the old one (%s > %s): "
				                       "Black Eye can't key them. Trackers for trackers, or the same socket on the same rig"),
				                  Old.Component.IsEmpty() ? TEXT("root") : *Old.Component, Old.Bone.IsEmpty() ? TEXT("-") : *Old.Bone);
		}

		FLog Log;
		const FScopedTransaction Transaction(FText::Format(LOCTEXT("SetSubjectHere", "Black Eye Setup: {0} subject from the playhead"),
			FText::FromString(SwitchName(Switch))));
		Camera.Sequence->Modify();
		Camera.MovieScene->Modify();
		if (Switch == EBlackEyeSetupSwitch::Focus)
		{
			UCineCameraComponent* LiveLens = Lens(Camera.Live);
			KeyReference(Camera, LiveLens, FocusActorPropertyName, FocusActorPropertyPath, Old.Actor, Subject.Actor, When, Log);
			if (LiveLens)
			{
				LiveLens->FocusSettings.TrackingFocusSettings.ActorToTrack = Subject.Actor.Get(); // the view, now
			}
		}
		else
		{
			UObject* LiveComponent = Switch == EBlackEyeSetupSwitch::Follow ? GetFollowComponent(Camera.Live) : GetLookAtComponent(Camera.Live);
			KeyReference(Camera, LiveComponent, TEXT("Actor"), TEXT("Target_0.Actor"), Old.Actor, Subject.Actor, When, Log);
			FSubjectValue Live;
			if (ReadSubject(LiveComponent, Names::Target0, Live))
			{
				Live.Actor = Subject.Actor; // the view, now
				WriteSubject(LiveComponent, Names::Target0, Live);
			}
		}
		Changed(Camera);
		Log.Lines.Insert(FString::Printf(TEXT("%s subject: %s from the playhead (before it: %s)"), SwitchName(Switch), *Label(Subject.Actor),
			*Label(Old.Actor)), 0);
		return Log.Join();
	}

	FBlackEyeSetupSubject SubjectFromActor(EBlackEyeSetupSwitch Switch, AActor* Actor, FString& OutNote)
	{
		FBlackEyeSetupSubject Out;
		Out.Actor = Actor;
		if (!Actor || IsTrackerLike(Actor))
		{
			return Out;
		}
		if (Switch == EBlackEyeSetupSwitch::Follow)
		{
			USkeletalMeshComponent* Mesh = nullptr;
			FString Bone;
			bool bHeading = false;
			if (FindFollowBone(Actor, Mesh, Bone, bHeading))
			{
				Out.Component = Mesh->GetName();
				Out.Bone = Bone;
				if (!bHeading)
				{
					OutNote = FString::Printf(TEXT("%s has no Heading socket, so Follow uses %s; Subject Heading may point the wrong way on it"),
						*Label(Actor), *Bone);
				}
			}
			return Out;
		}
		if (AActor* Tracker = FindTracker(Actor))
		{
			Out = FBlackEyeSetupSubject();
			Out.Actor = Tracker;
			return Out;
		}
		USkeletalMeshComponent* Mesh = nullptr;
		FString Bone;
		if (FindHead(Actor, Mesh, Bone))
		{
			Out.Component = Mesh->GetName();
			Out.Bone = Bone;
			OutNote = FString::Printf(TEXT("%s has no tracker; %s uses its %s bone"), *Label(Actor), SwitchName(Switch), *Bone);
		}
		return Out;
	}

	FString Autofill(const FCamera& Camera, AActor* Character)
	{
		if (!Character)
		{
			return TEXT("error: select a character in the level first");
		}
		if (IsTrackerLike(Character) && Character->GetAttachParentActor())
		{
			Character = Character->GetAttachParentActor();
		}
		const FScopedTransaction Transaction(LOCTEXT("Autofill", "Black Eye Setup: fill from character"));
		TArray<FString> Lines;
		for (const EBlackEyeSetupSwitch Switch : { EBlackEyeSetupSwitch::Follow, EBlackEyeSetupSwitch::LookAt, EBlackEyeSetupSwitch::Focus })
		{
			FString Note;
			const FBlackEyeSetupSubject Subject = SubjectFromActor(Switch, Character, Note);
			Lines.Add(SetSubject(Camera, Switch, Subject));
			if (!Note.IsEmpty())
			{
				Lines.Add(TEXT("note: ") + Note);
			}
		}
		return FString::Join(Lines, TEXT("\n"));
	}
}

// --- Library ------------------------------------------------------------------------------------------------------------

namespace
{
	bool CameraFor(ULevelSequence* Sequence, const FString& Name, BlackEyeSetup::FCamera& Out, FString& OutError)
	{
		const TSharedPtr<ISequencer> Sequencer = BlackEyeFastBake::OpenSequencer(Sequence, OutError);
		return Sequencer && BlackEyeSetup::FindCamera(Sequencer, Name, Out, OutError);
	}
}

FBlackEyeSetupState UBlackEyeSetupLibrary::GetSetupState(ULevelSequence* Sequence, const FString& CameraBindingName)
{
	BlackEyeSetup::FCamera Camera;
	FString Error;
	if (!CameraFor(Sequence, CameraBindingName, Camera, Error))
	{
		FBlackEyeSetupState State;
		State.Message = Error;
		return State;
	}
	return BlackEyeSetup::ReadState(Camera);
}

FString UBlackEyeSetupLibrary::SetSetupSwitch(ULevelSequence* Sequence, const FString& CameraBindingName, EBlackEyeSetupSwitch Switch, bool bOn,
                                              bool bFromPlayhead)
{
	BlackEyeSetup::FCamera Camera;
	FString Error;
	return CameraFor(Sequence, CameraBindingName, Camera, Error) ? BlackEyeSetup::SetSwitch(Camera, Switch, bOn, bFromPlayhead) : TEXT("error: ") + Error;
}

FString UBlackEyeSetupLibrary::SetSetupAllOff(ULevelSequence* Sequence, const FString& CameraBindingName)
{
	BlackEyeSetup::FCamera Camera;
	FString Error;
	return CameraFor(Sequence, CameraBindingName, Camera, Error) ? BlackEyeSetup::AllOff(Camera) : TEXT("error: ") + Error;
}

FString UBlackEyeSetupLibrary::SetSetupSubject(ULevelSequence* Sequence, const FString& CameraBindingName, EBlackEyeSetupSwitch Switch,
                                               const FBlackEyeSetupSubject& Subject, bool bFromPlayhead)
{
	BlackEyeSetup::FCamera Camera;
	FString Error;
	if (!CameraFor(Sequence, CameraBindingName, Camera, Error))
	{
		return TEXT("error: ") + Error;
	}
	return bFromPlayhead ? BlackEyeSetup::SetSubjectFromPlayhead(Camera, Switch, Subject) : BlackEyeSetup::SetSubject(Camera, Switch, Subject);
}

FString UBlackEyeSetupLibrary::AutofillSetup(ULevelSequence* Sequence, const FString& CameraBindingName, AActor* Character)
{
	BlackEyeSetup::FCamera Camera;
	FString Error;
	return CameraFor(Sequence, CameraBindingName, Camera, Error) ? BlackEyeSetup::Autofill(Camera, Character) : TEXT("error: ") + Error;
}

#undef LOCTEXT_NAMESPACE
