// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Bake an edit (DESIGN section 6, "Bake an edit"): an edit cuts between angle shots on a Cinematic Shot track and
// shows a fraction of each. Bake every Black Eye camera it shows, keyed only on the frames it shows plus handles.
// Entry points: Sequencer toolbar > Bake Edit, Content Browser right-click on a Level Sequence, and the console
// command BlackEyeCustom.FastBake.BakeEdit. The bake of each shot is BakeShot with Ranges.

#include "BlackEyeFastBakeInternal.h"

#include "BlackEyeContract.h"
#include "CineCameraActor.h"
#include "ContentBrowserMenuContexts.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "Misc/ConfigCacheIni.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneSubSection.h"
#include "SequencerToolMenuContext.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "BlackEyeFastBakeEdit"

namespace BlackEyeFastBake
{
	extern TArray<TWeakPtr<ISequencer>> OpenSequencers; // BlackEyeFastBakeMenu.cpp

	/** What the dialog asks, remembered per user between sessions. */
	struct FEditBakeSettings
	{
		enum EMode : int32 { MasterLikeRender = 0, MasterHandles = 1, ShotByShot = 2 };
		enum ELock : int32 { NoLock = 0, LockAll = 1, LockBaked = 2 };

		int32 Mode = MasterLikeRender;
		int32 HandleFrames = 24;   // each side; Like a render: after each cut only
		int32 WarmUpFrames = 60;   // not with Like a render, which settles at each cut instead
		bool bWriteDirect = true;  // twin written as data; false: through Sequencer, inside each shot
		bool bRefreshSetup = false;
		bool bKeepOtherKeys = true;
		int32 Lock = LockAll;

		static constexpr const TCHAR* Section = TEXT("BlackEyeCustom.FastBake.Edit");

		bool FromMaster() const { return Mode != ShotByShot; }
		bool SettleAtCut() const { return Mode == MasterLikeRender; }

		void Load()
		{
			GConfig->GetInt(Section, TEXT("Mode"), Mode, GEditorPerProjectIni);
			GConfig->GetInt(Section, TEXT("HandleFrames"), HandleFrames, GEditorPerProjectIni);
			GConfig->GetInt(Section, TEXT("WarmUpFrames"), WarmUpFrames, GEditorPerProjectIni);
			GConfig->GetBool(Section, TEXT("bWriteDirect"), bWriteDirect, GEditorPerProjectIni);
			GConfig->GetBool(Section, TEXT("bRefreshSetup"), bRefreshSetup, GEditorPerProjectIni);
			GConfig->GetBool(Section, TEXT("bKeepOtherKeys"), bKeepOtherKeys, GEditorPerProjectIni);
			GConfig->GetInt(Section, TEXT("Lock"), Lock, GEditorPerProjectIni);
			Mode = FMath::Clamp(Mode, 0, 2);
			Lock = FMath::Clamp(Lock, 0, 2);
		}

		void Save() const
		{
			GConfig->SetInt(Section, TEXT("Mode"), Mode, GEditorPerProjectIni);
			GConfig->SetInt(Section, TEXT("HandleFrames"), HandleFrames, GEditorPerProjectIni);
			GConfig->SetInt(Section, TEXT("WarmUpFrames"), WarmUpFrames, GEditorPerProjectIni);
			GConfig->SetBool(Section, TEXT("bWriteDirect"), bWriteDirect, GEditorPerProjectIni);
			GConfig->SetBool(Section, TEXT("bRefreshSetup"), bRefreshSetup, GEditorPerProjectIni);
			GConfig->SetBool(Section, TEXT("bKeepOtherKeys"), bKeepOtherKeys, GEditorPerProjectIni);
			GConfig->SetInt(Section, TEXT("Lock"), Lock, GEditorPerProjectIni);
		}

		/** The bake options every camera of the batch shares; name and ranges come from each plan. */
		FBlackEyeFastBakeOptions Options() const
		{
			FBlackEyeFastBakeOptions Options;
			Options.WarmUpFrames = SettleAtCut() ? 0 : WarmUpFrames;
			Options.bSettleEachRange = SettleAtCut();
			Options.bKeepOtherKeys = bKeepOtherKeys;
			Options.bLockAfterBake = Lock != NoLock;
			Options.bLockBakedFramesOnly = Lock == LockBaked;
			Options.bRefreshTwinSetup = bRefreshSetup;
			return Options;
		}
	};

	namespace Edit
	{
		/** A binding's class without spawning it: a spawnable's template, else a possessable's class. */
		const UClass* BindingClass(ULevelSequence* Sequence, const FGuid& Guid, const TSharedRef<UE::MovieScene::FSharedPlaybackState>& State)
		{
			if (const UObject* Template = MovieSceneHelpers::GetObjectTemplate(Sequence, Guid, State))
			{
				return Template->GetClass();
			}
			const FMovieScenePossessable* Possessable = Sequence->GetMovieScene()->FindPossessable(Guid);
			return Possessable ? Possessable->GetPossessedObjectClass() : nullptr;
		}

		/** A camera cut of a shot that plays a Black Eye camera (directly, or through its twin when locked). */
		struct FCutCamera
		{
			FGuid Camera;
			TRange<FFrameNumber> Range; // shot ticks
		};

		TArray<FCutCamera> CutCameras(ULevelSequence* Shot)
		{
			TArray<FCutCamera> Out;
			const UMovieScene* MovieScene = Shot->GetMovieScene();
			const UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
			const UClass* BlackEye = BlackEyeContract::GetCameraBaseClass();
			if (!Cuts || !BlackEye || Cuts->IsEvalDisabled())
			{
				return Out;
			}
			const TArray<FTwin> Twins = FindTwins(*MovieScene);
			const TSharedRef<UE::MovieScene::FSharedPlaybackState> State = MovieSceneHelpers::CreateTransientSharedPlaybackState(GWorld, Shot);
			for (const UMovieSceneSection* Section : Cuts->GetAllSections())
			{
				const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
				if (!Cut || !Cut->IsActive())
				{
					continue;
				}
				FGuid Camera = Cut->GetCameraBindingID().GetGuid();
				for (const FTwin& Twin : Twins)
				{
					if (Twin.Alive.Contains(Camera) || Twin.Stale.Contains(Camera))
					{
						Camera = Twin.Camera;
					}
				}
				const UClass* Class = Camera.IsValid() ? BindingClass(Shot, Camera, State) : nullptr;
				if (Class && Class->IsChildOf(BlackEye))
				{
					Out.Add({ Camera, Cut->GetRange() });
				}
			}
			return Out;
		}

		/** One cinematic shot section's view of a camera: shot ticks, inside one camera cut. */
		struct FUse
		{
			TWeakObjectPtr<ULevelSequence> Shot;
			FGuid Camera;
			TRange<FFrameNumber> Ticks;
			TRange<FFrameNumber> CutTicks;
			TWeakObjectPtr<UMovieSceneSubSection> Section; // the section showing the shot
		};

		/**
		 * Walks an edit's Cinematic Shot tracks. A sequence whose camera cuts play a Black Eye camera is a shot; any
		 * other is a nested edit, walked in turn. Sub tracks are never followed: inside a shot they hold its scene.
		 * Window is the part of Sequence that is seen, in its ticks. Only, when not empty, limits the top level to those
		 * sections (Bake Edit on a selection).
		 */
		void Collect(ULevelSequence* Sequence, const TRange<FFrameNumber>& Window, int32 Depth, TArray<ULevelSequence*>& Path, TArray<FUse>& Out,
		             TConstArrayView<const UMovieSceneSubSection*> Only = {})
		{
			for (const UMovieSceneTrack* Track : Sequence->GetMovieScene()->GetTracks())
			{
				const UMovieSceneCinematicShotTrack* Shots = Cast<UMovieSceneCinematicShotTrack>(Track);
				if (!Shots || Shots->IsEvalDisabled())
				{
					continue;
				}
				for (const UMovieSceneSection* Section : Shots->GetAllSections())
				{
					const UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section);
					ULevelSequence* Inner = Sub ? Cast<ULevelSequence>(Sub->GetSequence()) : nullptr;
					if (!Inner || !Sub->IsActive() || Shots->IsRowEvalDisabled(Sub->GetRowIndex()) || (Only.Num() && !Only.Contains(Sub)))
					{
						continue;
					}
					// A section on a lower row can be partly hidden by one above it; it is baked whole, which only
					// costs frames (DESIGN section 6).
					const TRange<FFrameNumber> Seen = TRange<FFrameNumber>::Intersection(Sub->GetRange(), Window);
					if (Seen.IsEmpty() || !Seen.HasLowerBound() || !Seen.HasUpperBound())
					{
						continue;
					}
					// Section range to inner ticks: offset, time scale, loops and the inner tick resolution.
					const TRange<FFrameTime> InnerTime = Sub->OuterToInnerTransform().ComputeTraversedHull(Seen);
					if (InnerTime.IsEmpty() || !InnerTime.HasLowerBound() || !InnerTime.HasUpperBound())
					{
						continue;
					}
					const TRange<FFrameNumber> InnerWindow(InnerTime.GetLowerBoundValue().FloorToFrame(), InnerTime.GetUpperBoundValue().CeilToFrame());

					const TArray<FCutCamera> Cameras = CutCameras(Inner);
					for (const FCutCamera& Cut : Cameras)
					{
						const TRange<FFrameNumber> Ticks = TRange<FFrameNumber>::Intersection(InnerWindow, Cut.Range);
						if (!Ticks.IsEmpty())
						{
							Out.Add({ Inner, Cut.Camera, Ticks, Cut.Range, const_cast<UMovieSceneSubSection*>(Sub) });
						}
					}
					if (Cameras.Num() == 0 && Depth < 8 && !Path.Contains(Inner))
					{
						Path.Push(Inner);
						Collect(Inner, InnerWindow, Depth + 1, Path, Out);
						Path.Pop();
					}
				}
			}
		}
	}

	TArray<FShotCamera> ListShotCameras(ULevelSequence* Shot)
	{
		TArray<FShotCamera> Out;
		const UMovieScene* MovieScene = Shot ? Shot->GetMovieScene() : nullptr;
		const UClass* BlackEye = BlackEyeContract::GetCameraBaseClass();
		if (!MovieScene || !BlackEye)
		{
			return Out;
		}
		// Data only: binding classes from spawnable templates or possessable classes, the cuts, the twin tags.
		const TSharedRef<UE::MovieScene::FSharedPlaybackState> State = MovieSceneHelpers::CreateTransientSharedPlaybackState(GWorld, Shot);
		const TArray<Edit::FCutCamera> Cuts = Edit::CutCameras(Shot);
		const TArray<FTwin> Twins = FindTwins(*MovieScene);
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			const FGuid Guid = Binding.GetObjectGuid();
			const UClass* Class = Edit::BindingClass(Shot, Guid, State);
			if (!Class || !Class->IsChildOf(BlackEye))
			{
				continue;
			}
			FShotCamera& Camera = Out.AddDefaulted_GetRef();
			Camera.Camera = Guid;
			Camera.Name = MovieScene->GetObjectDisplayName(Guid).ToString();
			Camera.bOnCut = Cuts.ContainsByPredicate([&Guid](const Edit::FCutCamera& Cut) { return Cut.Camera == Guid; });
			for (const FTwin& Twin : Twins)
			{
				if (Twin.Camera == Guid)
				{
					Camera.Twins = Twin.Alive;
					Camera.DefaultTwin = DefaultTwin(*MovieScene, Twin);
				}
			}
		}
		Out.StableSort([](const FShotCamera& A, const FShotCamera& B) { return A.bOnCut && !B.bOnCut; });
		return Out;
	}

	TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* EditSequence, int32 HandleFrames, TConstArrayView<const UMovieSceneSubSection*> Only)
	{
		return GetEditBakePlan(EditSequence, HandleFrames, HandleFrames, false, Only);
	}

	TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* EditSequence, int32 HeadFrames, int32 TailFrames, bool bSettleAtCut,
	                                              TConstArrayView<const UMovieSceneSubSection*> Only)
	{
		using namespace Edit;
		TArray<FBlackEyeShotBakePlan> Plans;
		if (!EditSequence)
		{
			return Plans;
		}
		TArray<FUse> Uses;
		TArray<ULevelSequence*> Path{ EditSequence };
		// A whole edit is seen within its playback range; a section picked by hand is baked whole, even past it.
		Collect(EditSequence, Only.Num() ? TRange<FFrameNumber>::All() : EditSequence->GetMovieScene()->GetPlaybackRange(), 0, Path, Uses, Only);

		// Per plan: the shot frames each use shows, and how far handles may reach (its camera cut: outside it that
		// camera isn't the one shown).
		struct FSpan { int32 Start; int32 End; int32 Min; int32 Max; };
		TArray<TArray<FSpan>> Spans;
		for (const FUse& Use : Uses)
		{
			ULevelSequence* Shot = Use.Shot.Get();
			const UMovieScene* MovieScene = Shot->GetMovieScene();
			const FString Name = MovieScene->GetObjectDisplayName(Use.Camera).ToString();
			int32 Index = Plans.IndexOfByPredicate([&](const FBlackEyeShotBakePlan& P) { return P.Shot == Shot && P.CameraBindingName == Name; });
			if (Index == INDEX_NONE)
			{
				Index = Plans.Num();
				FBlackEyeShotBakePlan& Plan = Plans.AddDefaulted_GetRef();
				Plan.Shot = Shot;
				Plan.CameraBindingName = Name;
				Plan.FirstSection = Use.Section;
				Spans.AddDefaulted();
			}
			++Plans[Index].NumUses;

			auto ToDisplay = [MovieScene](FFrameNumber Tick, bool bCeil)
			{
				const FFrameTime T = FFrameRate::TransformTime(FFrameTime(Tick), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate());
				return bCeil ? T.CeilToFrame().Value : T.FloorToFrame().Value;
			};
			Spans[Index].Add({ ToDisplay(Use.Ticks.GetLowerBoundValue(), false), ToDisplay(Use.Ticks.GetUpperBoundValue(), true),
				Use.CutTicks.HasLowerBound() ? ToDisplay(Use.CutTicks.GetLowerBoundValue(), false) : TNumericLimits<int32>::Lowest(),
				Use.CutTicks.HasUpperBound() ? ToDisplay(Use.CutTicks.GetUpperBoundValue(), true) : TNumericLimits<int32>::Max() });
		}

		for (int32 p = 0; p < Plans.Num(); ++p)
		{
			TArray<FSpan>& Shown = Spans[p];
			TArray<FBlackEyeBakeRange>& Ranges = Plans[p].Ranges;
			if (!bSettleAtCut)
			{
				for (const FSpan& S : Shown)
				{
					Ranges.Add({ FMath::Max(S.Start - FMath::Max(0, HeadFrames), S.Min), FMath::Min(S.End + FMath::Max(0, TailFrames), S.Max) });
				}
				Ranges = NormalizeRanges(MoveTemp(Ranges));
				continue;
			}
			// Like a render: every span starts on its cut, where it will open with a snap. Spans showing the same frames
			// merge; tail handles never run into the next span's start, which must stay a cut of its own.
			Shown.Sort([](const FSpan& A, const FSpan& B) { return A.Start < B.Start; });
			TArray<FSpan> Merged;
			for (const FSpan& S : Shown)
			{
				if (Merged.Num() && S.Start < Merged.Last().End)
				{
					Merged.Last().End = FMath::Max(Merged.Last().End, S.End);
					Merged.Last().Max = FMath::Max(Merged.Last().Max, S.Max);
				}
				else
				{
					Merged.Add(S);
				}
			}
			for (int32 i = 0; i < Merged.Num(); ++i)
			{
				int32 End = FMath::Min(Merged[i].End + FMath::Max(0, TailFrames), Merged[i].Max);
				if (Merged.IsValidIndex(i + 1))
				{
					End = FMath::Min(End, Merged[i + 1].Start);
				}
				Ranges.Add({ Merged[i].Start, FMath::Max(End, Merged[i].End) });
			}
			Ranges = NormalizeRanges(MoveTemp(Ranges), false);
		}
		return Plans;
	}

	namespace Edit
	{
		int32 Frames(const FBlackEyeShotBakePlan& Plan)
		{
			int32 N = 0;
			for (const FBlackEyeBakeRange& R : Plan.Ranges)
			{
				N += R.EndFrame - R.StartFrame;
			}
			return N;
		}

		int32 ShotFrames(const ULevelSequence* Shot)
		{
			const UMovieScene* MovieScene = Shot->GetMovieScene();
			const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
			return FFrameRate::TransformTime(FFrameTime(Playback.Size<FFrameNumber>()), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate()).RoundToFrame().Value;
		}

		/** Sequencer for this sequence if it is open as root, else null. */
		TSharedPtr<ISequencer> FindRootSequencer(ULevelSequence* Sequence)
		{
			UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			IAssetEditorInstance* Editor = (AssetEditors && Sequence) ? AssetEditors->FindEditorForAsset(Sequence, false) : nullptr;
			TSharedPtr<ISequencer> Sequencer = Editor ? static_cast<ILevelSequenceEditorToolkit*>(Editor)->GetSequencer() : nullptr;
			return (Sequencer && Sequencer->GetRootMovieSceneSequence() == Sequence) ? Sequencer : nullptr;
		}

		/**
		 * The window's shot controls: one shot, from a single selected shot section, or the shot Sequencer has open with
		 * nothing selected. Which of its Black Eye cameras to bake and lock, and into which bake camera. Read from the
		 * shot's data only (bindings, cuts, tags), nothing spawned or evaluated, so it is listed as the window opens.
		 */
		struct FShotTarget
		{
			TWeakObjectPtr<ULevelSequence> Shot;
			TWeakObjectPtr<UMovieSceneSubSection> Section; // the selected section showing it; null: the shot itself is open
			TRange<FFrameNumber> SectionTicks = TRange<FFrameNumber>::All(); // shot ticks that section shows
			TArray<FShotCamera> Cameras;
			int32 Camera = 0;        // into Cameras
			int32 Twin = INDEX_NONE; // into the camera's Twins; INDEX_NONE: create a new bake camera
			bool bSectionOnly = true;
			double MsListed = 0.0;

			const FShotCamera* Current() const { return Cameras.IsValidIndex(Camera) ? &Cameras[Camera] : nullptr; }

			FGuid TwinGuid() const
			{
				const FShotCamera* C = Current();
				return C && C->Twins.IsValidIndex(Twin) ? C->Twins[Twin] : FGuid();
			}

			/** Lock and unlock act here: the section's frames, or the whole shot. */
			TRange<FFrameNumber> LockSpan() const
			{
				return Section.IsValid() && bSectionOnly ? SectionTicks : TRange<FFrameNumber>::All();
			}

			/** The default bake camera of the current camera: the one its cuts play, else the newest, else a new one. */
			void PickDefaultTwin()
			{
				const FShotCamera* C = Current();
				Twin = C ? C->Twins.IndexOfByKey(C->DefaultTwin) : INDEX_NONE;
			}

			/** This shot's plan entry for the chosen camera and bake camera, from the edit's plan or the shot's own cuts. */
			TArray<FBlackEyeShotBakePlan> Plan(TArray<FBlackEyeShotBakePlan> EditPlans, bool bSettleAtCut) const
			{
				ULevelSequence* ShotSequence = Shot.Get();
				const FShotCamera* C = Current();
				if (!ShotSequence || !C)
				{
					return {};
				}
				FBlackEyeShotBakePlan* Found = EditPlans.FindByPredicate([&](const FBlackEyeShotBakePlan& P) { return P.Shot == ShotSequence && P.CameraBindingName == C->Name; });
				FBlackEyeShotBakePlan Plan;
				if (Found)
				{
					Plan = *Found;
				}
				else
				{
					// The shot open on its own, or a camera the section's cuts don't play: the camera's cuts within the
					// shot's playback range (the section's frames when there is one), else all of it.
					const UMovieScene* MovieScene = ShotSequence->GetMovieScene();
					const TRange<FFrameNumber> Window = TRange<FFrameNumber>::Intersection(MovieScene->GetPlaybackRange(), SectionTicks);
					auto ToDisplay = [MovieScene](FFrameNumber Tick, bool bCeil)
					{
						const FFrameTime T = FFrameRate::TransformTime(FFrameTime(Tick), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate());
						return bCeil ? T.CeilToFrame().Value : T.FloorToFrame().Value;
					};
					auto Add = [&](const TRange<FFrameNumber>& Ticks)
					{
						if (!Ticks.IsEmpty() && Ticks.HasLowerBound() && Ticks.HasUpperBound())
						{
							Plan.Ranges.Add({ ToDisplay(Ticks.GetLowerBoundValue(), false), ToDisplay(Ticks.GetUpperBoundValue(), true) });
						}
					};
					for (const FCutCamera& Cut : CutCameras(ShotSequence))
					{
						if (Cut.Camera == C->Camera)
						{
							Add(TRange<FFrameNumber>::Intersection(Cut.Range, Window));
						}
					}
					if (Plan.Ranges.Num() == 0)
					{
						Add(Window);
					}
					Plan.Ranges = NormalizeRanges(MoveTemp(Plan.Ranges), !bSettleAtCut);
					Plan.Shot = ShotSequence;
					Plan.CameraBindingName = C->Name;
					Plan.NumUses = 1;
					Plan.FirstSection = Section;
				}
				Plan.TwinBinding = TwinGuid();
				Plan.bCreateNewTwin = !Plan.TwinBinding.IsValid();
				return Plan.Ranges.Num() ? TArray<FBlackEyeShotBakePlan>{ Plan } : TArray<FBlackEyeShotBakePlan>();
			}
		};

		/** The shot controls for a scope, or null when it isn't about one shot (FShotTarget). */
		TSharedPtr<FShotTarget> MakeShotTarget(ULevelSequence* Edit, TConstArrayView<TWeakObjectPtr<UMovieSceneSubSection>> Selected)
		{
			TSharedRef<FShotTarget> Target = MakeShared<FShotTarget>();
			UMovieSceneSubSection* Section = Selected.Num() == 1 ? Selected[0].Get() : nullptr;
			ULevelSequence* Shot = Section ? Cast<ULevelSequence>(Section->GetSequence()) : (Selected.Num() == 0 ? Edit : nullptr);
			if (!Shot)
			{
				return nullptr;
			}
			const double T0 = FPlatformTime::Seconds();
			Target->Cameras = ListShotCameras(Shot);
			Target->MsListed = 1000.0 * (FPlatformTime::Seconds() - T0);
			UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] Bake Edit: %d Black Eye camera(s) in %s listed in %.2f ms"),
				Target->Cameras.Num(), *Shot->GetName(), Target->MsListed);
			if (Target->Cameras.Num() == 0)
			{
				return nullptr; // a nested edit, or a sequence without a Black Eye camera
			}
			Target->Shot = Shot;
			Target->Section = Section;
			if (Section)
			{
				const TRange<FFrameTime> Inner = Section->OuterToInnerTransform().ComputeTraversedHull(Section->GetRange());
				if (!Inner.IsEmpty() && Inner.HasLowerBound() && Inner.HasUpperBound())
				{
					Target->SectionTicks = TRange<FFrameNumber>(Inner.GetLowerBoundValue().FloorToFrame(), Inner.GetUpperBoundValue().CeilToFrame());
				}
			}
			Target->Camera = FMath::Max(0, Target->Cameras.IndexOfByPredicate([](const FShotCamera& C) { return C.bOnCut; }));
			Target->PickDefaultTwin();
			return Target;
		}

		/**
		 * What one Bake Edit covers: the whole edit, or the sections selected in it. Every entry point builds one and
		 * hands it to the same dialog and batch, so the two ways in never drift apart.
		 */
		struct FBakeScope
		{
			TWeakObjectPtr<ULevelSequence> Edit;                    // the sequence holding the Cinematic Shot track
			TArray<TWeakObjectPtr<UMovieSceneSubSection>> Selected; // its selected shot sections, maybe none
			TWeakObjectPtr<ULevelSequence> ReturnTo;                // the Sequencer's root when started, reopened at the end
			bool bSelectedOnly = false;
			TSharedPtr<FShotTarget> Target;                         // one shot's controls, when the scope is about one shot
			TWeakPtr<ISequencer> Sequencer;                         // where it was opened from, refreshed after lock / unlock

			/** The shot controls decide what is baked: one selected section's shot, or the shot open with nothing selected. */
			bool TargetBakes() const
			{
				return Target.IsValid() && (Target->Section.IsValid() ? bSelectedOnly : true);
			}

			void EnsureTarget()
			{
				if (!Target.IsValid())
				{
					Target = MakeShotTarget(Edit.Get(), Selected);
				}
			}

			TArray<FBlackEyeShotBakePlan> Plan(const FEditBakeSettings& Settings) const
			{
				return Plan(Settings.SettleAtCut() ? 0 : Settings.HandleFrames, Settings.HandleFrames, Settings.SettleAtCut());
			}

			TArray<FBlackEyeShotBakePlan> Plan(int32 HeadFrames, int32 TailFrames, bool bSettleAtCut) const
			{
				TArray<const UMovieSceneSubSection*> Only;
				for (const TWeakObjectPtr<UMovieSceneSubSection>& Section : Selected)
				{
					if (bSelectedOnly && Section.IsValid())
					{
						Only.Add(Section.Get());
					}
				}
				if (bSelectedOnly && Only.Num() == 0)
				{
					return {}; // every selected section is gone
				}
				TArray<FBlackEyeShotBakePlan> Plans = GetEditBakePlan(Edit.Get(), HeadFrames, TailFrames, bSettleAtCut, Only);
				return TargetBakes() ? Target->Plan(MoveTemp(Plans), bSettleAtCut) : Plans;
			}

			FString Describe() const
			{
				const FString Name = Edit.IsValid() ? Edit->GetName() : FString();
				if (TargetBakes() && !Target->Section.IsValid())
				{
					return FString::Printf(TEXT("shot %s"), *Name);
				}
				return bSelectedOnly ? FString::Printf(TEXT("%d selected section(s) of %s"), Selected.Num(), *Name) : Name;
			}
		};

		/** The shot sections selected in a Sequencer's focused sequence. */
		TArray<TWeakObjectPtr<UMovieSceneSubSection>> SelectedShotSections(ISequencer& Sequencer)
		{
			TArray<TWeakObjectPtr<UMovieSceneSubSection>> Out;
			const UMovieSceneSequence* Focused = Sequencer.GetFocusedMovieSceneSequence();
			TArray<UMovieSceneSection*> Selected;
			Sequencer.GetSelectedSections(Selected);
			for (UMovieSceneSection* Section : Selected)
			{
				UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section);
				if (Sub && Sub->GetTypedOuter<UMovieSceneCinematicShotTrack>() && Focused && Sub->GetTypedOuter<UMovieScene>() == Focused->GetMovieScene())
				{
					Out.Add(Sub);
				}
			}
			return Out;
		}

		/** A Sequencer's scope: its focused sequence and selection; it returns to its root. */
		FBakeScope ScopeOf(ISequencer& Sequencer)
		{
			FBakeScope Scope;
			Scope.Edit = Cast<ULevelSequence>(Sequencer.GetFocusedMovieSceneSequence());
			Scope.ReturnTo = Cast<ULevelSequence>(Sequencer.GetRootMovieSceneSequence());
			Scope.Selected = SelectedShotSections(Sequencer);
			Scope.bSelectedOnly = Scope.Selected.Num() > 0;
			Scope.Sequencer = Sequencer.AsShared();
			return Scope;
		}

		FText Summary(const FString& Scope, const TCHAR* How, int32 Baked, int32 Total, int32 Keyed, double Seconds, bool bCancelled,
		              const TArray<FString>& Failures)
		{
			return FText::Format(
				LOCTEXT("EditDone", "Bake Edit ({1}), {0}: {2} of {3} camera(s) baked, {4} frames keyed, {5}s{6}{7}"),
				FText::FromString(Scope), FText::FromString(How), Baked, Total, Keyed, FText::AsNumber(FMath::RoundToInt(Seconds)),
				bCancelled ? LOCTEXT("Cancelled", ". Cancelled") : FText(),
				Failures.Num() ? FText::FromString(TEXT(". Failed: ") + FString::Join(Failures, TEXT("; "))) : FText());
		}

		/** A batch in flight, shot by shot: one shot camera at a time, each opened alone first (BakeShot needs it as root). */
		struct FBatch
		{
			FBakeScope Scope;
			TOptional<FQualifiedFrameTime> EditTime; // the root's playhead, put back at the end
			TArray<FBlackEyeShotBakePlan> Plans;
			FEditBakeSettings Settings;
			ACineCameraActor* Scratch = nullptr;     // the direct writer's, made on first use
			int32 Next = 0;
			int32 Baked = 0;
			int32 KeyedFrames = 0;
			double Seconds = 0.0;
			TArray<FString> Failures;
			bool bCancelled = false;
		};

		void Finish(const TSharedRef<FBatch>& Batch)
		{
			DestroyScratchCamera(Batch->Scratch);
			Batch->Scratch = nullptr;
			const FText Text = Summary(Batch->Scope.Describe(), TEXT("shot by shot"), Batch->Baked, Batch->Plans.Num(), Batch->KeyedFrames,
				Batch->Seconds, Batch->bCancelled, Batch->Failures);
			UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] %s"), *Text.ToString());
			const bool bOk = Batch->Failures.Num() == 0 && !Batch->bCancelled;
			ULevelSequence* ReturnTo = Batch->Scope.ReturnTo.Get();
			if (!ReturnTo)
			{
				Notify(Text, bOk);
				return;
			}
			// Back to where the user was, at the frame they left.
			OpenThen(ReturnTo, [Batch, Text, bOk]()
			{
				if (TSharedPtr<ISequencer> Sequencer = FindRootSequencer(Batch->Scope.ReturnTo.Get()))
				{
					if (Batch->EditTime.IsSet())
					{
						Sequencer->SetGlobalTime(Batch->EditTime->Time);
					}
					Sequencer->ForceEvaluate();
				}
				Notify(Text, bOk);
			});
		}

		/** One shot camera, with the twin written as data instead of through its spawned copy (WriteTwinDirect). */
		FBlackEyeFastBakeReport BakeShotDirect(FBatch& Batch, ULevelSequence* Shot, const FBlackEyeFastBakeOptions& Options)
		{
			FBakeOutput Bake;
			FBlackEyeFastBakeReport Report = RunBake(Shot, Options, Bake);
			if (!Report.bSuccess)
			{
				return Report;
			}
			if (!Batch.Scratch)
			{
				Batch.Scratch = SpawnScratchCamera();
			}
			{
				const FScopedTransaction Transaction(LOCTEXT("FastBakeDirect", "Black Eye Fast Bake"));
				Report.bSuccess = WriteTwinDirect(Shot, Options, Bake, Batch.Scratch, Bake.Sequencer.Get(), Report);
			}
			if (Bake.Sequencer)
			{
				Bake.Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
				Bake.Sequencer->ForceEvaluate();
			}
			return Report;
		}

		void BakeNext(const TSharedRef<FBatch>& Batch)
		{
			if (Batch->bCancelled || Batch->Next >= Batch->Plans.Num())
			{
				Finish(Batch);
				return;
			}
			const int32 Index = Batch->Next++;
			ULevelSequence* Shot = Batch->Plans[Index].Shot;
			if (!Shot)
			{
				Batch->Failures.Add(TEXT("a shot was unloaded"));
				RunNextTick([Batch]() { BakeNext(Batch); });
				return;
			}
			OpenThen(Shot, [Batch, Index]()
			{
				const FBlackEyeShotBakePlan& Plan = Batch->Plans[Index];
				FBlackEyeFastBakeOptions Options = Batch->Settings.Options();
				Options.CameraBindingName = Plan.CameraBindingName;
				Options.Ranges = Plan.Ranges;
				Options.ProgressNote = FString::Printf(TEXT(" (%d of %d)"), Index + 1, Batch->Plans.Num());
				const FBlackEyeFastBakeReport Report = Batch->Settings.bWriteDirect
					? BakeShotDirect(*Batch, Plan.Shot, Options)
					: UBlackEyeFastBakeLibrary::BakeShot(Plan.Shot, Options);
				Batch->Seconds += Report.TotalSeconds;
				if (Report.bSuccess)
				{
					++Batch->Baked;
					Batch->KeyedFrames += Report.NumFrames;
				}
				else
				{
					Batch->bCancelled = Report.Message == TEXT("cancelled");
					if (!Batch->bCancelled)
					{
						Batch->Failures.Add(FString::Printf(TEXT("%s %s: %s"), *Plan.Shot->GetName(), *Plan.CameraBindingName, *Report.Message));
					}
				}
				RunNextTick([Batch]() { BakeNext(Batch); });
			});
		}

		/** The open Sequencer showing this edit: focused on it, else holding it as root. */
		TSharedPtr<ISequencer> SequencerShowing(ULevelSequence* Edit)
		{
			OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
			for (const TWeakPtr<ISequencer>& Weak : OpenSequencers)
			{
				const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
				if (Sequencer && Sequencer->GetFocusedMovieSceneSequence() == Edit)
				{
					return Sequencer;
				}
			}
			return FindRootSequencer(Edit);
		}

		/**
		 * From the master: the edit stays open and every shot is baked inside it (BlackEyeFastBakeMaster.cpp). Only an edit
		 * that isn't open yet (the Content Browser entry) is opened, once.
		 */
		void StartMaster(const FBakeScope& Scope, const FEditBakeSettings& Settings, bool bOpened = false)
		{
			ULevelSequence* EditSequence = Scope.Edit.Get();
			const TSharedPtr<ISequencer> Sequencer = SequencerShowing(EditSequence);
			if (!Sequencer)
			{
				if (bOpened || !EditSequence)
				{
					Notify(FText::Format(LOCTEXT("MasterNotOpen", "{0}: could not open it in Sequencer"), FText::FromString(Scope.Describe())), false);
					return;
				}
				OpenThen(EditSequence, [Scope, Settings]() { StartMaster(Scope, Settings, true); });
				return;
			}
			TArray<FBlackEyeShotBakePlan> Plans = Scope.Plan(Settings);
			if (Plans.Num() == 0)
			{
				Notify(FText::Format(LOCTEXT("NothingToBake", "{0}: no shot with a Black Eye camera on its camera cuts"),
				                     FText::FromString(Scope.Describe())), false);
				return;
			}
			FMasterBakeSettings Master;
			Master.bWriteDirect = Settings.bWriteDirect;
			Master.Options = Settings.Options();
			const int32 Total = Plans.Num();
			const FString Describe = Scope.Describe();
			const TCHAR* How = Settings.SettleAtCut() ? TEXT("from the master, like a render") : TEXT("from the master, with handles");
			RunMasterBake(Sequencer.ToSharedRef(), MoveTemp(Plans), Master, [Describe, How, Total](const FMasterBakeResult& Result)
			{
				const FText Text = Summary(Describe, How, Result.Baked, Total, Result.KeyedFrames, Result.Seconds, Result.bCancelled, Result.Failures);
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] %s"), *Text.ToString());
				Notify(Text, Result.Failures.Num() == 0 && !Result.bCancelled);
			});
		}

		void StartBatch(const FBakeScope& InScope, const FEditBakeSettings& Settings)
		{
			FBakeScope Scope = InScope;
			Scope.EnsureTarget();
			if (Settings.FromMaster())
			{
				RunNextTick([Scope, Settings]() { StartMaster(Scope, Settings); });
				return;
			}
			TSharedRef<FBatch> Batch = MakeShared<FBatch>();
			Batch->Scope = Scope;
			if (!Batch->Scope.ReturnTo.IsValid())
			{
				Batch->Scope.ReturnTo = Scope.Edit;
			}
			Batch->Settings = Settings;
			Batch->Plans = Scope.Plan(Settings);
			if (TSharedPtr<ISequencer> Sequencer = FindRootSequencer(Batch->Scope.ReturnTo.Get()))
			{
				Batch->EditTime = Sequencer->GetGlobalTime();
			}
			if (Batch->Plans.Num() == 0)
			{
				Notify(FText::Format(LOCTEXT("NothingToBake", "{0}: no shot with a Black Eye camera on its camera cuts"),
				                     FText::FromString(Scope.Describe())), false);
				return;
			}
			RunNextTick([Batch]() { BakeNext(Batch); });
		}

		/**
		 * The window's shot controls (FShotTarget): which Black Eye camera, which bake camera (or a new one), and lock /
		 * unlock for the selected section or the whole shot, acting at once, one undo step each.
		 */
		TSharedRef<SWidget> MakeShotBox(const TSharedRef<FBakeScope>& Scope)
		{
			const TSharedPtr<FShotTarget> Target = Scope->Target;
			if (!Target.IsValid())
			{
				return SNullWidget::NullWidget;
			}
			struct FOptions
			{
				TArray<TSharedPtr<FString>> Cameras;
				TArray<TSharedPtr<FString>> Twins;
				TSharedPtr<SComboBox<TSharedPtr<FString>>> TwinCombo;
				FString Status;
			};
			const TSharedRef<FOptions> Options = MakeShared<FOptions>();
			const UMovieScene* MovieScene = Target->Shot.IsValid() ? Target->Shot->GetMovieScene() : nullptr;
			for (const FShotCamera& C : Target->Cameras)
			{
				Options->Cameras.Add(MakeShared<FString>(C.bOnCut ? C.Name + TEXT("   (on the camera cut)") : C.Name));
			}
			// The bake cameras of the current camera, and "Create new" last.
			auto RebuildTwins = [Target, Options, MovieScene]()
			{
				Options->Twins.Reset();
				if (const FShotCamera* C = Target->Current())
				{
					for (const FGuid& Twin : C->Twins)
					{
						const FString Name = MovieScene ? MovieScene->GetObjectDisplayName(Twin).ToString() : Twin.ToString();
						Options->Twins.Add(MakeShared<FString>(Twin == C->DefaultTwin ? Name + TEXT("   (current)") : Name));
					}
				}
				Options->Twins.Add(MakeShared<FString>(TEXT("Create new +")));
				if (Options->TwinCombo)
				{
					Options->TwinCombo->RefreshOptions();
				}
			};
			RebuildTwins();
			auto RefreshStatus = [Target, Options]()
			{
				const FShotCamera* C = Target->Current();
				const ULevelSequence* Shot = Target->Shot.Get();
				Options->Status = C && Shot ? DescribeCutPlay(*Shot->GetMovieScene(), C->Camera, Target->LockSpan()) : FString();
			};
			RefreshStatus();
			auto Text = [](TSharedPtr<FString> Item) -> TSharedRef<SWidget> { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); };

			auto SetLock = [Scope, Target, RefreshStatus, RebuildTwins](bool bLock)
			{
				const FShotCamera* C = Target->Current();
				const int32 Changed = C ? SetShotCameraLock(Target->Shot.Get(), C->Camera, Target->TwinGuid(), bLock, Target->LockSpan()) : 0;
				// Which bake camera is "current" may have changed; the list's order doesn't.
				const TArray<FShotCamera> Fresh = ListShotCameras(Target->Shot.Get());
				if (Fresh.Num() == Target->Cameras.Num())
				{
					Target->Cameras = Fresh;
					RebuildTwins();
				}
				if (const TSharedPtr<ISequencer> Sequencer = Scope->Sequencer.Pin())
				{
					Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
					Sequencer->ForceEvaluate();
				}
				RefreshStatus();
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] %s %s: %d camera cut section(s) changed"),
					bLock ? TEXT("lock") : TEXT("unlock"), *GetNameSafe(Target->Shot.Get()), Changed);
				return FReply::Handled();
			};
			auto Button = [](const FName& Icon, const FText& Label, const FText& Tip, TFunction<FReply()> OnClick, TAttribute<bool> Enabled)
			{
				return SNew(SButton)
					.IsEnabled(Enabled)
					.ToolTipText(Tip)
					.OnClicked_Lambda([OnClick]() { return OnClick(); })
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)[ SNew(SImage).Image(FAppStyle::GetBrush(Icon)) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(Label) ]
					];
			};
			auto Row = [](const FText& Label, TSharedRef<SWidget> Widget)
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(0.3f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(Label) ]
					+ SHorizontalBox::Slot().FillWidth(0.7f)[ Widget ];
			};

			const FText Title = FText::Format(Target->Section.IsValid() ? LOCTEXT("ShotFromSection", "This shot: {0} (the selected section)")
			                                                            : LOCTEXT("ShotItself", "This shot: {0}"),
				FText::FromString(GetNameSafe(Target->Shot.Get())));

			return SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder")).Padding(8)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(Title).Font(FAppStyle::GetFontStyle("BoldFont")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 2)
				[
					Row(LOCTEXT("ShotCamera", "Black Eye camera"),
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&Options->Cameras)
						.OnGenerateWidget_Lambda(Text)
						.OnSelectionChanged_Lambda([Target, Options, RebuildTwins, RefreshStatus](TSharedPtr<FString> Item, ESelectInfo::Type)
						{
							const int32 Index = Options->Cameras.IndexOfByKey(Item);
							if (Index != INDEX_NONE && Index != Target->Camera)
							{
								Target->Camera = Index;
								Target->PickDefaultTwin();
								RebuildTwins();
								RefreshStatus();
							}
						})
						[ SNew(STextBlock).Text_Lambda([Target]() { const FShotCamera* C = Target->Current(); return FText::FromString(C ? C->Name : FString()); }) ])
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
				[
					Row(LOCTEXT("ShotTwin", "Bake camera"),
						SAssignNew(Options->TwinCombo, SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&Options->Twins)
						.OnGenerateWidget_Lambda(Text)
						.OnSelectionChanged_Lambda([Target, Options](TSharedPtr<FString> Item, ESelectInfo::Type)
						{
							const int32 Index = Options->Twins.IndexOfByKey(Item);
							if (Index != INDEX_NONE)
							{
								Target->Twin = Index == Options->Twins.Num() - 1 ? INDEX_NONE : Index;
							}
						})
						.ToolTipText(LOCTEXT("ShotTwinTip", "The plain camera this Black Eye camera is baked into. A camera can have several; "
						                                    "\"Create new +\" bakes into a new one beside them."))
						[
							SNew(STextBlock).Text_Lambda([Target, MovieScene]()
							{
								const FGuid Twin = Target->TwinGuid();
								return Twin.IsValid() && MovieScene ? MovieScene->GetObjectDisplayName(Twin) : LOCTEXT("CreateNew", "Create new +");
							})
						])
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						Button("Icons.Lock", LOCTEXT("LockNow", "Lock"),
							LOCTEXT("LockNowTip", "The camera cut plays the bake camera (here, or in the whole shot)."),
							[SetLock]() { return SetLock(true); },
							TAttribute<bool>::CreateLambda([Target]() { return Target->TwinGuid().IsValid(); }))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
					[
						Button("Icons.Unlock", LOCTEXT("UnlockNow", "Unlock"),
							LOCTEXT("UnlockNowTip", "The camera cut plays the live Black Eye camera again (here, or in the whole shot)."),
							[SetLock]() { return SetLock(false); },
							TAttribute<bool>::CreateLambda([Target]() { return Target->Current() && Target->Current()->Twins.Num() > 0; }))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsEnabled(Target->Section.IsValid())
						.IsChecked_Lambda([Target]() { return Target->Section.IsValid() && Target->bSectionOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([Target, RefreshStatus](ECheckBoxState S) { Target->bSectionOnly = S == ECheckBoxState::Checked; RefreshStatus(); })
						.ToolTipText(LOCTEXT("SectionOnlyTip", "On: lock and unlock change only the frames this section shows (the shot's camera cut "
						                                       "is split there). Off: the whole shot, in every edit that uses it."))
						[ SNew(STextBlock).Text(LOCTEXT("SectionOnly", "Only this section")) ]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SNew(STextBlock).AutoWrapText(true)
					.Text_Lambda([Options]() { return FText::Format(LOCTEXT("NowPlays", "Now plays: {0}"), FText::FromString(Options->Status)); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(FText::Format(LOCTEXT("ShotNote", "Baking uses this camera and bake camera when it bakes just this shot ({0}). "
					                                       "Cameras found in {1} ms."),
						Target->Section.IsValid() ? LOCTEXT("ShotNoteSection", "\"Selected sections\"") : LOCTEXT("ShotNoteOpen", "the shot open here"),
						FText::FromString(FString::Printf(TEXT("%.2f"), Target->MsListed))))
				]
			];
		}

		/** The dialog: how to bake, the twin, lock, and what that would bake. Starts the batch on Bake. */
		void OpenDialog(const FBakeScope& InScope)
		{
			ULevelSequence* EditSequence = InScope.Edit.Get();
			if (!EditSequence)
			{
				return;
			}
			TSharedRef<FEditBakeSettings> Settings = MakeShared<FEditBakeSettings>();
			Settings->Load();
			TSharedRef<FBakeScope> Scope = MakeShared<FBakeScope>(InScope);
			Scope->EnsureTarget();
			bool bBake = false;

			// The summary follows the settings; the plan is re-made only when what it depends on changes.
			struct FPlanCache { int32 Handles = INDEX_NONE; int32 Mode = INDEX_NONE; bool bSelectedOnly = false; int32 Camera = INDEX_NONE; int32 Twin = INDEX_NONE;
			                    TArray<FBlackEyeShotBakePlan> Plans; };
			TSharedRef<FPlanCache> Cache = MakeShared<FPlanCache>();
			auto CurrentPlans = [Scope, Settings, Cache]() -> const TArray<FBlackEyeShotBakePlan>&
			{
				const int32 Camera = Scope->Target ? Scope->Target->Camera : INDEX_NONE;
				const int32 Twin = Scope->Target ? Scope->Target->Twin : INDEX_NONE;
				if (Cache->Handles != Settings->HandleFrames || Cache->Mode != Settings->Mode || Cache->bSelectedOnly != Scope->bSelectedOnly
					|| Cache->Camera != Camera || Cache->Twin != Twin)
				{
					Cache->Handles = Settings->HandleFrames;
					Cache->Mode = Settings->Mode;
					Cache->bSelectedOnly = Scope->bSelectedOnly;
					Cache->Camera = Camera;
					Cache->Twin = Twin;
					Cache->Plans = Scope->Plan(*Settings);
				}
				return Cache->Plans;
			};
			auto PlanSummary = [CurrentPlans]()
			{
				const TArray<FBlackEyeShotBakePlan>& Plans = CurrentPlans();
				if (Plans.Num() == 0)
				{
					return LOCTEXT("NoShots", "Nothing here plays a Black Eye camera on its camera cuts.\n"
					                          "An edit is a sequence with a Cinematic Shot track; open the edit, not a shot.");
				}
				FString Lines;
				int32 Keyed = 0, Whole = 0;
				TSet<const ULevelSequence*> Shots;
				for (const FBlackEyeShotBakePlan& Plan : Plans)
				{
					const int32 N = Frames(Plan);
					Keyed += N;
					if (!Shots.Contains(Plan.Shot))
					{
						Shots.Add(Plan.Shot);
						Whole += ShotFrames(Plan.Shot);
					}
					Lines += FString::Printf(TEXT("%s  /  %s:  %d use(s), %d range(s), %d frames\n"),
						*Plan.Shot->GetName(), *Plan.CameraBindingName, Plan.NumUses, Plan.Ranges.Num(), N);
				}
				return FText::FromString(FString::Printf(TEXT("%s\n%d camera(s) in %d shot(s): %d frames keyed, %.0f%% of the shots' %d frames."),
					*Lines, Plans.Num(), Shots.Num(), Keyed, Whole ? 100.0 * Keyed / Whole : 0.0, Whole));
			};

			auto Spin = [](int32* Value, const FText& Tip, TAttribute<bool> Enabled)
			{
				return SNew(SSpinBox<int32>)
					.MinValue(0).MaxValue(100000).MinSliderValue(0).MaxSliderValue(240)
					.IsEnabled(Enabled)
					.Value_Lambda([Value]() { return *Value; })
					.OnValueChanged_Lambda([Value](int32 V) { *Value = V; })
					.ToolTipText(Tip);
			};
			auto Row = [](TAttribute<FText> Label, TSharedRef<SWidget> Widget)
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(Label) ]
					+ SHorizontalBox::Slot().FillWidth(0.55f)[ Widget ];
			};
			auto Heading = [](const FText& Text)
			{
				return SNew(STextBlock).Text(Text).Font(FAppStyle::GetFontStyle("BoldFont"));
			};
			// A radio button, its plain-words explanation under it, indented by Indent.
			auto Choice = [](TFunction<bool()> IsOn, TFunction<void()> TurnOn, const FText& Label, const FText& Explain, float Indent,
			                 TAttribute<bool> Enabled)
			{
				return SNew(SVerticalBox).IsEnabled(Enabled)
					+ SVerticalBox::Slot().AutoHeight().Padding(Indent, 4, 0, 0)
					[
						SNew(SCheckBox)
						.Style(FAppStyle::Get(), "RadioButton")
						.IsChecked_Lambda([IsOn]() { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([TurnOn](ECheckBoxState) { TurnOn(); })
						[ SNew(STextBlock).Text(Label) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(Indent + 22, 0, 0, 2)
					[
						SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(Explain)
					];
			};
			auto IntChoice = [Choice](int32* Value, int32 Option, const FText& Label, const FText& Explain, float Indent = 0.f,
			                          TAttribute<bool> Enabled = true)
			{
				return Choice([Value, Option]() { return *Value == Option; }, [Value, Option]() { *Value = Option; }, Label, Explain, Indent, Enabled);
			};
			auto BoolChoice = [Choice](bool* Value, bool bOption, const FText& Label, const FText& Explain, float Indent = 0.f,
			                           TAttribute<bool> Enabled = true)
			{
				return Choice([Value, bOption]() { return *Value == bOption; }, [Value, bOption]() { *Value = bOption; }, Label, Explain, Indent, Enabled);
			};

			// Whole edit or selection: shown only when something is selected.
			auto ScopeChoice = [Scope](bool bSelected, const FText& Label)
			{
				return SNew(SCheckBox)
					.Style(FAppStyle::Get(), "RadioButton")
					.IsChecked_Lambda([Scope, bSelected]() { return Scope->bSelectedOnly == bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([Scope, bSelected](ECheckBoxState) { Scope->bSelectedOnly = bSelected; })
					[ SNew(STextBlock).Text(Label) ];
			};
			const EVisibility ScopeVisibility = Scope->Selected.Num() ? EVisibility::Visible : EVisibility::Collapsed;

			int32* Mode = &Settings->Mode;
			const TAttribute<bool> HasHeadHandles = TAttribute<bool>::CreateLambda([Settings]() { return !Settings->SettleAtCut(); });
			const TAttribute<bool> IsDirect = TAttribute<bool>::CreateLambda([Settings]() { return Settings->bWriteDirect; });
			const TAttribute<FText> HandlesLabel = TAttribute<FText>::CreateLambda([Settings]()
			{
				return Settings->SettleAtCut() ? LOCTEXT("HandlesAfter", "Extra frames after each cut")
				                               : LOCTEXT("HandlesBoth", "Handles (frames each side)");
			});

			TSharedRef<SWindow> Window = SNew(SWindow)
				.Title(FText::Format(LOCTEXT("DialogTitle", "Black Eye Fast Bake: {0}"), FText::FromString(EditSequence->GetName())))
				.ClientSize(FVector2D(700, 860))
				.SupportsMinimize(false).SupportsMaximize(false);
			TWeakPtr<SWindow> WeakWindow = Window;

			Window->SetContent(
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(12)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().FillHeight(1.f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[
								SNew(STextBlock).AutoWrapText(true)
								.Text(LOCTEXT("DialogIntro", "Records every Black Eye camera this edit shows into a plain \"twin\" camera, "
								                             "so the edit plays the same way every time."))
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[
								SNew(SHorizontalBox).Visibility(ScopeVisibility)
								+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
								[ ScopeChoice(true, FText::Format(LOCTEXT("ScopeSelected", "Selected sections ({0})"), Scope->Selected.Num())) ]
								+ SHorizontalBox::Slot().AutoWidth()
								[ ScopeChoice(false, LOCTEXT("ScopeWhole", "Whole edit")) ]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[
								MakeShotBox(Scope)
							]

							// 1. How to bake
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)[ Heading(LOCTEXT("HowHeading", "How to bake")) ]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock).AutoWrapText(true).Margin(FMargin(0, 4, 0, 0))
								.Text(LOCTEXT("MasterExplain", "From the master (new, fast): stays in this edit. For each shot, Sequencer plays just "
								                               "that shot inside the edit, the camera is stepped frame by frame and recorded. Nothing "
								                               "is opened or closed, so there's no wait for a big edit to reopen."))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								IntChoice(Mode, FEditBakeSettings::MasterLikeRender, LOCTEXT("LikeRender", "Like a render"),
									LOCTEXT("LikeRenderExplain", "At every cut the camera starts already settled on its subject, just like a final "
									                             "render. Only the frames the edit shows are baked, plus extra frames after each cut. "
									                             "If you later make a shot start earlier, bake again."), 16.f)
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								IntChoice(Mode, FEditBakeSettings::MasterHandles, LOCTEXT("WithHandles", "With handles"),
									LOCTEXT("WithHandlesExplain", "Also bakes frames before each cut, so you can trim the edit later without "
									                              "baking again. The catch: the camera settles at the start of the handle, so at the "
									                              "cut it's already moving and lagging, not settled like a render."), 16.f)
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								IntChoice(Mode, FEditBakeSettings::ShotByShot, LOCTEXT("ShotByShot", "Shot by shot (the old way)"),
									LOCTEXT("ShotByShotExplain", "Opens each shot on its own, bakes it, then opens this edit again. Same result "
									                             "as \"With handles\", but reopening a big edit can freeze the editor for minutes."))
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 2)
							[
								Row(HandlesLabel, Spin(&Settings->HandleFrames,
									LOCTEXT("HandlesTip", "Keyed frames beyond what the edit shows, for trimming later without a re-bake. "
									                      "Never past the shot's camera cut."), true))
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
							[
								Row(LOCTEXT("WarmUp", "Warm-up (frames, not keyed)"), Spin(&Settings->WarmUpFrames,
									LOCTEXT("WarmUpTip", "Frames played before each range, unkeyed, so the camera arrives moving as it does in "
									                     "playback. Not used by \"Like a render\", which settles at the cut instead."), HasHeadHandles))
							]

							// 2. The twin
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 0)[ Heading(LOCTEXT("TwinHeading", "The twin")) ]
							+ SVerticalBox::Slot().AutoHeight()
							[
								BoolChoice(&Settings->bWriteDirect, true, LOCTEXT("Direct", "Write it directly (new)"),
									LOCTEXT("DirectExplain", "Writes the twin straight into each shot's data. No stepping into shots."))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								BoolChoice(&Settings->bWriteDirect, false, LOCTEXT("Inside", "Write it inside each shot (the old way, tested)"),
									LOCTEXT("InsideExplain", "Sequencer steps into each shot, sets the twin up on a live copy and saves it. "
									                         "Slower; always copies the camera setup too."))
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
							[
								BoolChoice(&Settings->bRefreshSetup, false, LOCTEXT("KeysOnly", "Keys only"),
									LOCTEXT("KeysOnlyExplain", "Only the motion and lens keys change. A twin keeps its camera setup (a new "
									                           "twin always gets one)."), 16.f, IsDirect)
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								BoolChoice(&Settings->bRefreshSetup, true, LOCTEXT("Setup", "Keys and camera setup"),
									LOCTEXT("SetupExplain", "Also copies the Black Eye camera's lens, filmback and extra components again. "
									                        "Use it after changing the camera's setup."), 16.f, IsDirect)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
							[
								BoolChoice(&Settings->bKeepOtherKeys, true, LOCTEXT("Keep", "Keep other baked frames"),
									LOCTEXT("KeepExplain", "Frames baked before (for another edit) stay. Only these frames are replaced."))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								BoolChoice(&Settings->bKeepOtherKeys, false, LOCTEXT("Replace", "Replace them all"),
									LOCTEXT("ReplaceExplain", "The twin keeps only this bake."))
							]

							// 3. Lock
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 0)[ Heading(LOCTEXT("LockHeading", "Afterwards, the shots play")) ]
							+ SVerticalBox::Slot().AutoHeight()
							[
								IntChoice(&Settings->Lock, FEditBakeSettings::LockAll, LOCTEXT("LockAll", "The bake, everywhere (lock)"),
									LOCTEXT("LockAllExplain", "Each shot's camera cut plays its twin."))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								IntChoice(&Settings->Lock, FEditBakeSettings::LockBaked, LOCTEXT("LockBaked", "The bake only where baked"),
									LOCTEXT("LockBakedExplain", "Only the frames baked now play the twin; the rest of each shot keeps the live "
									                            "Black Eye camera. The shot's camera cut is split at those frames."))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								IntChoice(&Settings->Lock, FEditBakeSettings::NoLock, LOCTEXT("NoLock", "The live Black Eye camera (don't lock)"),
									LOCTEXT("NoLockExplain", "The twins are baked but not used until you lock them."))
							]

							// What it would bake
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 4)[ Heading(LOCTEXT("PlanHeading", "What gets baked")) ]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder")).Padding(8)
								[ SNew(STextBlock).AutoWrapText(true).Text_Lambda(PlanSummary) ]
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 10, 0, 0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
						[
							SNew(SButton).Text(LOCTEXT("BakeButton", "Bake"))
							.IsEnabled_Lambda([CurrentPlans]() { return CurrentPlans().Num() > 0; })
							.OnClicked_Lambda([&bBake, WeakWindow]()
							{
								bBake = true;
								if (TSharedPtr<SWindow> W = WeakWindow.Pin()) { W->RequestDestroyWindow(); }
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
						[
							SNew(SButton).Text(LOCTEXT("CancelButton", "Cancel"))
							.OnClicked_Lambda([WeakWindow]()
							{
								if (TSharedPtr<SWindow> W = WeakWindow.Pin()) { W->RequestDestroyWindow(); }
								return FReply::Handled();
							})
						]
					]
				]);

			FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
			if (bBake && Scope->Edit.IsValid())
			{
				Settings->Save();
				StartBatch(*Scope, *Settings);
			}
		}

		/** The first open Sequencer's scope: its focused sequence and selection (the console commands' way in). */
		TOptional<FBakeScope> OpenSequencerScope()
		{
			OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
			for (const TWeakPtr<ISequencer>& Weak : OpenSequencers)
			{
				const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
				if (Sequencer && Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()))
				{
					return ScopeOf(*Sequencer);
				}
			}
			return {};
		}

		/**
		 * `BlackEyeCustom.FastBake.BakeEdit [handles] [warmup] [keep 0|1] [lock 0|1|2] [selected 0|1] [mode 0|1|2] [direct 0|1]
		 * [setup 0|1]`: the dialog's Bake, without the dialog, on the open Sequencer's focused sequence. lock: 0 none, 1 all,
		 * 2 only the baked frames. mode: 0 from the master like a render, 1 from the master with handles, 2 shot by shot.
		 * direct: twins written as data. setup: re-copy an existing twin's camera setup. Omitted arguments use the dialog's
		 * last values; `selected` defaults to 1 when shot sections are selected, as the dialog does.
		 * `BlackEyeCustom.FastBake.EditPlan [handles] [selected 0|1] [mode 0|1|2]` only logs what it would bake.
		 */
		FAutoConsoleCommand GBakeEditCommand(TEXT("BlackEyeCustom.FastBake.BakeEdit"),
			TEXT("Bake every Black Eye camera the focused edit (or its selected sections) shows, keyed only where shown. Args: [handles] [warmup] [keep 0|1] [lock 0|1|2] [selected 0|1] [mode 0|1|2] [direct 0|1] [setup 0|1]."),
			FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
			{
				TOptional<FBakeScope> Scope = OpenSequencerScope();
				if (!Scope)
				{
					UE_LOG(LogBlackEyeCustom, Warning, TEXT("[BlackEyeCustom] no open Sequencer to bake an edit from"));
					return;
				}
				FEditBakeSettings Settings;
				Settings.Load();
				if (Args.IsValidIndex(0)) { Settings.HandleFrames = FCString::Atoi(*Args[0]); }
				if (Args.IsValidIndex(1)) { Settings.WarmUpFrames = FCString::Atoi(*Args[1]); }
				if (Args.IsValidIndex(2)) { Settings.bKeepOtherKeys = FCString::Atoi(*Args[2]) != 0; }
				if (Args.IsValidIndex(3)) { Settings.Lock = FMath::Clamp(FCString::Atoi(*Args[3]), 0, 2); }
				if (Args.IsValidIndex(4)) { Scope->bSelectedOnly = FCString::Atoi(*Args[4]) != 0 && Scope->Selected.Num() > 0; }
				if (Args.IsValidIndex(5)) { Settings.Mode = FMath::Clamp(FCString::Atoi(*Args[5]), 0, 2); }
				if (Args.IsValidIndex(6)) { Settings.bWriteDirect = FCString::Atoi(*Args[6]) != 0; }
				if (Args.IsValidIndex(7)) { Settings.bRefreshSetup = FCString::Atoi(*Args[7]) != 0; }
				StartBatch(*Scope, Settings);
			}));

		FAutoConsoleCommand GEditPlanCommand(TEXT("BlackEyeCustom.FastBake.EditPlan"),
			TEXT("Log what Bake Edit would bake for the focused sequence (or its selected sections). Args: [handles] [selected 0|1] [mode 0|1|2]."),
			FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
			{
				TOptional<FBakeScope> Scope = OpenSequencerScope();
				if (!Scope)
				{
					UE_LOG(LogBlackEyeCustom, Warning, TEXT("[BlackEyeCustom] no open Sequencer"));
					return;
				}
				FEditBakeSettings Settings;
				Settings.Load();
				if (Args.IsValidIndex(0)) { Settings.HandleFrames = FCString::Atoi(*Args[0]); }
				if (Args.IsValidIndex(1)) { Scope->bSelectedOnly = FCString::Atoi(*Args[1]) != 0 && Scope->Selected.Num() > 0; }
				if (Args.IsValidIndex(2)) { Settings.Mode = FMath::Clamp(FCString::Atoi(*Args[2]), 0, 2); }
				const TArray<FBlackEyeShotBakePlan> Plans = Scope->Plan(Settings);
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] edit plan %s, handles %d, mode %d: %d camera(s)"), *Scope->Describe(),
					Settings.HandleFrames, Settings.Mode, Plans.Num());
				for (const FBlackEyeShotBakePlan& Plan : Plans)
				{
					FString Ranges;
					for (const FBlackEyeBakeRange& R : Plan.Ranges)
					{
						Ranges += FString::Printf(TEXT(" [%d,%d)"), R.StartFrame, R.EndFrame);
					}
					UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] edit plan   %s / %s: %d use(s), %d frames:%s"),
						*Plan.Shot->GetPathName(), *Plan.CameraBindingName, Plan.NumUses, Frames(Plan), *Ranges);
				}
			}));

		FDelegateHandle ToolMenusStartup;
		const FName MenuOwner(TEXT("BlackEyeFastBakeEdit"));

		void ExtendMenus()
		{
			const FToolMenuOwnerScoped Owner(MenuOwner);

			// Sequencer toolbar: Bake Edit, on whatever sequence that Sequencer has focused when clicked.
			UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("Sequencer.MainToolBar"));
			Toolbar->AddDynamicSection(TEXT("BlackEyeFastBakeEdit"), FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
			{
				const USequencerToolMenuContext* Context = Menu->FindContext<USequencerToolMenuContext>();
				if (!Context)
				{
					return;
				}
				const TWeakPtr<ISequencer> WeakSequencer = Context->WeakSequencer;
				FToolMenuSection& Section = Menu->AddSection(TEXT("BlackEyeFastBakeEdit"));
				Section.AddEntry(FToolMenuEntry::InitToolBarButton(TEXT("BlackEyeBakeEdit"),
					FUIAction(FExecuteAction::CreateLambda([WeakSequencer]()
					{
						// The scope is read at click time: the focused sequence and the shot sections selected in it.
						if (const TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin())
						{
							const FBakeScope Scope = ScopeOf(*Sequencer);
							RunNextTick([Scope]() { OpenDialog(Scope); }); // after the click has finished
						}
					})),
					LOCTEXT("BakeEditLabel", "Bake Edit"),
					LOCTEXT("BakeEditTip", "Black Eye Fast Bake: bake every Black Eye camera this edit shows, keyed only where it shows "
					                       "them, with handles. With shot sections selected, bakes just those (the window lets you "
					                       "switch to the whole edit). Opens a window to set the handles first."),
					FSlateIconFinder::FindIconForClass(ACineCameraActor::StaticClass())));
			}));

			// Content Browser: right-click a Level Sequence.
			UToolMenu* AssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(ULevelSequence::StaticClass());
			AssetMenu->AddDynamicSection(TEXT("BlackEyeFastBakeEdit"), FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
			{
				const UContentBrowserAssetContextMenuContext* Context = Menu->FindContext<UContentBrowserAssetContextMenuContext>();
				if (!Context || Context->SelectedAssets.Num() != 1)
				{
					return;
				}
				const FSoftObjectPath Asset = Context->SelectedAssets[0].GetSoftObjectPath();
				FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
				Section.AddMenuEntry(TEXT("BlackEyeBakeEdit"),
					LOCTEXT("BakeEditAsset", "Black Eye: Bake Edit..."),
					LOCTEXT("BakeEditAssetTip", "Bake every Black Eye camera this edit shows, keyed only where it shows them, with handles."),
					FSlateIconFinder::FindIconForClass(ACineCameraActor::StaticClass()),
					FUIAction(FExecuteAction::CreateLambda([Asset]()
					{
						RunNextTick([Asset]()
						{
							FBakeScope Scope;
							Scope.Edit = Cast<ULevelSequence>(Asset.TryLoad());
							OpenDialog(Scope);
						});
					})));
			}));
		}
	}

	void RegisterEditMenus()
	{
		Edit::ToolMenusStartup = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&Edit::ExtendMenus));
	}

	void UnregisterEditMenus()
	{
		UToolMenus::UnRegisterStartupCallback(Edit::ToolMenusStartup);
		UToolMenus::UnregisterOwner(Edit::MenuOwner);
	}
}

TArray<FBlackEyeShotBakePlan> UBlackEyeFastBakeLibrary::GetEditBakePlan(ULevelSequence* Edit, int32 HandleFrames)
{
	return BlackEyeFastBake::GetEditBakePlan(Edit, HandleFrames);
}

TArray<FBlackEyeShotBakePlan> UBlackEyeFastBakeLibrary::GetSectionsBakePlan(const TArray<UMovieSceneSubSection*>& Sections, int32 HandleFrames)
{
	ULevelSequence* Edit = Sections.Num() && Sections[0] ? Sections[0]->GetTypedOuter<ULevelSequence>() : nullptr;
	TArray<const UMovieSceneSubSection*> Only;
	for (const UMovieSceneSubSection* Section : Sections)
	{
		if (Section && Section->GetTypedOuter<ULevelSequence>() == Edit)
		{
			Only.Add(Section);
		}
	}
	return Only.Num() ? BlackEyeFastBake::GetEditBakePlan(Edit, HandleFrames, Only) : TArray<FBlackEyeShotBakePlan>();
}

#undef LOCTEXT_NAMESPACE
