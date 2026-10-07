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
#include "Sections/MovieSceneSubSection.h"
#include "SequencerToolMenuContext.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Widgets/Input/SButton.h"
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
		int32 HandleFrames = 24;
		int32 WarmUpFrames = 60;
		bool bKeepOtherKeys = true;
		bool bLock = true;

		static constexpr const TCHAR* Section = TEXT("BlackEyeCustom.FastBake.Edit");

		void Load()
		{
			GConfig->GetInt(Section, TEXT("HandleFrames"), HandleFrames, GEditorPerProjectIni);
			GConfig->GetInt(Section, TEXT("WarmUpFrames"), WarmUpFrames, GEditorPerProjectIni);
			GConfig->GetBool(Section, TEXT("bKeepOtherKeys"), bKeepOtherKeys, GEditorPerProjectIni);
			GConfig->GetBool(Section, TEXT("bLock"), bLock, GEditorPerProjectIni);
		}

		void Save() const
		{
			GConfig->SetInt(Section, TEXT("HandleFrames"), HandleFrames, GEditorPerProjectIni);
			GConfig->SetInt(Section, TEXT("WarmUpFrames"), WarmUpFrames, GEditorPerProjectIni);
			GConfig->SetBool(Section, TEXT("bKeepOtherKeys"), bKeepOtherKeys, GEditorPerProjectIni);
			GConfig->SetBool(Section, TEXT("bLock"), bLock, GEditorPerProjectIni);
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
					if (Twin.Twin == Camera || Twin.Stale.Contains(Camera))
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
		};

		/**
		 * Walks an edit's Cinematic Shot tracks. A sequence whose camera cuts play a Black Eye camera is a shot; any
		 * other is a nested edit, walked in turn. Sub tracks are never followed: inside a shot they hold its scene.
		 * Window is the part of Sequence that is seen, in its ticks.
		 */
		void Collect(ULevelSequence* Sequence, const TRange<FFrameNumber>& Window, int32 Depth, TArray<ULevelSequence*>& Path, TArray<FUse>& Out)
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
					if (!Inner || !Sub->IsActive() || Shots->IsRowEvalDisabled(Sub->GetRowIndex()))
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
							Out.Add({ Inner, Cut.Camera, Ticks, Cut.Range });
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

	TArray<FBlackEyeShotBakePlan> GetEditBakePlan(ULevelSequence* EditSequence, int32 HandleFrames)
	{
		using namespace Edit;
		TArray<FBlackEyeShotBakePlan> Plans;
		if (!EditSequence)
		{
			return Plans;
		}
		TArray<FUse> Uses;
		TArray<ULevelSequence*> Path{ EditSequence };
		Collect(EditSequence, EditSequence->GetMovieScene()->GetPlaybackRange(), 0, Path, Uses);

		for (const FUse& Use : Uses)
		{
			ULevelSequence* Shot = Use.Shot.Get();
			const UMovieScene* MovieScene = Shot->GetMovieScene();
			const FString Name = MovieScene->GetObjectDisplayName(Use.Camera).ToString();
			FBlackEyeShotBakePlan* Plan = Plans.FindByPredicate([&](const FBlackEyeShotBakePlan& P) { return P.Shot == Shot && P.CameraBindingName == Name; });
			if (!Plan)
			{
				Plan = &Plans.AddDefaulted_GetRef();
				Plan->Shot = Shot;
				Plan->CameraBindingName = Name;
			}
			++Plan->NumUses;

			auto ToDisplay = [MovieScene](FFrameNumber Tick, bool bCeil)
			{
				const FFrameTime T = FFrameRate::TransformTime(FFrameTime(Tick), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate());
				return bCeil ? T.CeilToFrame().Value : T.FloorToFrame().Value;
			};
			// Handles widen the range, but never past the camera cut: outside it the camera isn't the one shown.
			int32 Start = ToDisplay(Use.Ticks.GetLowerBoundValue(), false) - FMath::Max(0, HandleFrames);
			int32 End = ToDisplay(Use.Ticks.GetUpperBoundValue(), true) + FMath::Max(0, HandleFrames);
			if (Use.CutTicks.HasLowerBound())
			{
				Start = FMath::Max(Start, ToDisplay(Use.CutTicks.GetLowerBoundValue(), false));
			}
			if (Use.CutTicks.HasUpperBound())
			{
				End = FMath::Min(End, ToDisplay(Use.CutTicks.GetUpperBoundValue(), true));
			}
			Plan->Ranges.Add({ Start, End });
		}
		for (FBlackEyeShotBakePlan& Plan : Plans)
		{
			Plan.Ranges = NormalizeRanges(MoveTemp(Plan.Ranges));
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

		/** A batch in flight: one shot camera at a time, each opened alone first (BakeShot needs it as root). */
		struct FBatch
		{
			TWeakObjectPtr<ULevelSequence> EditSequence;
			TOptional<FQualifiedFrameTime> EditTime; // the edit's playhead, put back at the end
			TArray<FBlackEyeShotBakePlan> Plans;
			FEditBakeSettings Settings;
			int32 Next = 0;
			int32 Baked = 0;
			int32 KeyedFrames = 0;
			double Seconds = 0.0;
			TArray<FString> Failures;
			bool bCancelled = false;
		};

		void Finish(const TSharedRef<FBatch>& Batch)
		{
			const FText Summary = FText::Format(
				LOCTEXT("EditDone", "Bake Edit {0}: {1} of {2} camera(s) baked, {3} frames keyed, {4}s{5}{6}"),
				FText::FromString(Batch->EditSequence.IsValid() ? Batch->EditSequence->GetName() : FString()),
				Batch->Baked, Batch->Plans.Num(), Batch->KeyedFrames, FText::AsNumber(FMath::RoundToInt(Batch->Seconds)),
				Batch->bCancelled ? LOCTEXT("Cancelled", ". Cancelled") : FText(),
				Batch->Failures.Num() ? FText::FromString(TEXT(". Failed: ") + FString::Join(Batch->Failures, TEXT("; "))) : FText());
			UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] %s"), *Summary.ToString());
			const bool bOk = Batch->Failures.Num() == 0 && !Batch->bCancelled;
			ULevelSequence* EditSequence = Batch->EditSequence.Get();
			if (!EditSequence)
			{
				Notify(Summary, bOk);
				return;
			}
			// Back to the edit, at the frame the user left it.
			OpenThen(EditSequence, [Batch, Summary, bOk]()
			{
				if (TSharedPtr<ISequencer> Sequencer = FindRootSequencer(Batch->EditSequence.Get()))
				{
					if (Batch->EditTime.IsSet())
					{
						Sequencer->SetGlobalTime(Batch->EditTime->Time);
					}
					Sequencer->ForceEvaluate();
				}
				Notify(Summary, bOk);
			});
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
				FBlackEyeFastBakeOptions Options;
				Options.CameraBindingName = Plan.CameraBindingName;
				Options.Ranges = Plan.Ranges;
				Options.WarmUpFrames = Batch->Settings.WarmUpFrames;
				Options.bKeepOtherKeys = Batch->Settings.bKeepOtherKeys;
				Options.bLockAfterBake = Batch->Settings.bLock;
				Options.ProgressNote = FString::Printf(TEXT(" (%d of %d)"), Index + 1, Batch->Plans.Num());
				const FBlackEyeFastBakeReport Report = UBlackEyeFastBakeLibrary::BakeShot(Plan.Shot, Options);
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

		void StartBatch(ULevelSequence* EditSequence, const FEditBakeSettings& Settings)
		{
			TSharedRef<FBatch> Batch = MakeShared<FBatch>();
			Batch->EditSequence = EditSequence;
			Batch->Settings = Settings;
			Batch->Plans = GetEditBakePlan(EditSequence, Settings.HandleFrames);
			if (TSharedPtr<ISequencer> Sequencer = FindRootSequencer(EditSequence))
			{
				Batch->EditTime = Sequencer->GetGlobalTime();
			}
			if (Batch->Plans.Num() == 0)
			{
				Notify(FText::Format(LOCTEXT("NothingToBake", "{0} shows no shot with a Black Eye camera on its camera cuts"),
				                     FText::FromString(EditSequence->GetName())), false);
				return;
			}
			RunNextTick([Batch]() { BakeNext(Batch); });
		}

		/** The dialog: handles, warm-up, keep, lock, and what that would bake. Starts the batch on Bake. */
		void OpenDialog(ULevelSequence* EditSequence)
		{
			if (!EditSequence)
			{
				return;
			}
			TSharedRef<FEditBakeSettings> Settings = MakeShared<FEditBakeSettings>();
			Settings->Load();
			const TWeakObjectPtr<ULevelSequence> WeakEdit(EditSequence);
			bool bBake = false;

			// The summary follows the handles; the plan is re-made only when they change.
			struct FPlanCache { int32 Handles = INDEX_NONE; TArray<FBlackEyeShotBakePlan> Plans; };
			TSharedRef<FPlanCache> Cache = MakeShared<FPlanCache>();
			auto CurrentPlans = [WeakEdit, Settings, Cache]() -> const TArray<FBlackEyeShotBakePlan>&
			{
				if (Cache->Handles != Settings->HandleFrames)
				{
					Cache->Handles = Settings->HandleFrames;
					Cache->Plans = GetEditBakePlan(WeakEdit.Get(), Settings->HandleFrames);
				}
				return Cache->Plans;
			};
			auto Summary = [CurrentPlans]()
			{
				const TArray<FBlackEyeShotBakePlan>& Plans = CurrentPlans();
				if (Plans.Num() == 0)
				{
					return LOCTEXT("NoShots", "No shot in this edit plays a Black Eye camera on its camera cuts.\n"
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

			auto Spin = [](int32* Value, const FText& Tip)
			{
				return SNew(SSpinBox<int32>)
					.MinValue(0).MaxValue(100000).MinSliderValue(0).MaxSliderValue(240)
					.Value_Lambda([Value]() { return *Value; })
					.OnValueChanged_Lambda([Value](int32 V) { *Value = V; })
					.ToolTipText(Tip);
			};
			auto Check = [](bool* Value, const FText& Label, const FText& Tip)
			{
				return SNew(SCheckBox)
					.IsChecked_Lambda([Value]() { return *Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([Value](ECheckBoxState S) { *Value = S == ECheckBoxState::Checked; })
					.ToolTipText(Tip)
					[ SNew(STextBlock).Text(Label) ];
			};
			auto Row = [](const FText& Label, TSharedRef<SWidget> Widget)
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(Label) ]
					+ SHorizontalBox::Slot().FillWidth(0.55f)[ Widget ];
			};

			TSharedRef<SWindow> Window = SNew(SWindow)
				.Title(FText::Format(LOCTEXT("DialogTitle", "Black Eye Fast Bake: {0}"), FText::FromString(EditSequence->GetName())))
				.ClientSize(FVector2D(620, 480))
				.SupportsMinimize(false).SupportsMaximize(false);
			TWeakPtr<SWindow> WeakWindow = Window;

			Window->SetContent(
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(12)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
					[
						SNew(STextBlock).AutoWrapText(true)
						.Text(LOCTEXT("DialogIntro", "Bakes every Black Eye camera this edit shows, keyed only on the frames the edit uses "
						                             "plus handles, onto each camera's twin. Each shot is opened on its own for its bake; "
						                             "you are put back in the edit at the end."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
					[
						Row(LOCTEXT("Handles", "Handles (frames each side)"), Spin(&Settings->HandleFrames,
							LOCTEXT("HandlesTip", "Keyed frames before and after each section the edit uses, for trimming the edit "
							                      "later without a re-bake. Never past the shot's camera cut.")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
					[
						Row(LOCTEXT("WarmUp", "Warm-up (frames, not keyed)"), Spin(&Settings->WarmUpFrames,
							LOCTEXT("WarmUpTip", "Frames played before each range's handle, unkeyed, so the camera arrives moving and "
							                     "lagging as it does in playback rather than parked on its subject. Ranges closer than "
							                     "this are baked in one run.")))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 2)
					[
						Check(&Settings->bKeepOtherKeys, LOCTEXT("Keep", "Keep each twin's keys outside these ranges"),
							LOCTEXT("KeepTip", "On: other edits that use the same shots keep their baked frames. "
							                   "Off: each twin holds only this edit's frames."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
					[
						Check(&Settings->bLock, LOCTEXT("LockAll", "Lock the shots (they play the bake)"),
							LOCTEXT("LockAllTip", "Point each shot's camera cuts at its twin afterwards."))
					]
					+ SVerticalBox::Slot().FillHeight(1.f).Padding(0, 10)
					[
						SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder")).Padding(8)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()[ SNew(STextBlock).Text_Lambda(Summary) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
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
			if (bBake && WeakEdit.IsValid())
			{
				Settings->Save();
				StartBatch(WeakEdit.Get(), *Settings);
			}
		}

		/** The open Sequencer's focused sequence (the console command and agents' way in). */
		ULevelSequence* FocusedSequence()
		{
			OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
			for (const TWeakPtr<ISequencer>& Weak : OpenSequencers)
			{
				const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
				if (ULevelSequence* Focused = Sequencer ? Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()) : nullptr)
				{
					return Focused;
				}
			}
			return nullptr;
		}

		/**
		 * `BlackEyeCustom.FastBake.BakeEdit [handles] [warmup] [keep 0|1] [lock 0|1]`: the dialog's Bake, without the
		 * dialog, on the focused sequence of the open Sequencer. Omitted arguments use the dialog's last values.
		 * `BlackEyeCustom.FastBake.EditPlan [handles]` only logs what it would bake.
		 */
		FAutoConsoleCommand GBakeEditCommand(TEXT("BlackEyeCustom.FastBake.BakeEdit"),
			TEXT("Bake every Black Eye camera the focused edit shows, keyed only where it shows them. Args: [handles] [warmup] [keep 0|1] [lock 0|1]."),
			FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
			{
				ULevelSequence* EditSequence = FocusedSequence();
				if (!EditSequence)
				{
					UE_LOG(LogBlackEyeCustom, Warning, TEXT("[BlackEyeCustom] no open Sequencer to bake an edit from"));
					return;
				}
				FEditBakeSettings Settings;
				Settings.Load();
				if (Args.IsValidIndex(0)) { Settings.HandleFrames = FCString::Atoi(*Args[0]); }
				if (Args.IsValidIndex(1)) { Settings.WarmUpFrames = FCString::Atoi(*Args[1]); }
				if (Args.IsValidIndex(2)) { Settings.bKeepOtherKeys = FCString::Atoi(*Args[2]) != 0; }
				if (Args.IsValidIndex(3)) { Settings.bLock = FCString::Atoi(*Args[3]) != 0; }
				StartBatch(EditSequence, Settings);
			}));

		FAutoConsoleCommand GEditPlanCommand(TEXT("BlackEyeCustom.FastBake.EditPlan"),
			TEXT("Log what Bake Edit would bake for the focused sequence. Arg: [handles]."),
			FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
			{
				ULevelSequence* EditSequence = FocusedSequence();
				FEditBakeSettings Settings;
				Settings.Load();
				const int32 Handles = Args.IsValidIndex(0) ? FCString::Atoi(*Args[0]) : Settings.HandleFrames;
				const TArray<FBlackEyeShotBakePlan> Plans = GetEditBakePlan(EditSequence, Handles);
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] edit plan %s, handles %d: %d camera(s)"),
					EditSequence ? *EditSequence->GetName() : TEXT("(none)"), Handles, Plans.Num());
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
						const TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();
						ULevelSequence* Focused = Sequencer ? Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()) : nullptr;
						const TWeakObjectPtr<ULevelSequence> WeakFocused(Focused);
						RunNextTick([WeakFocused]() { OpenDialog(WeakFocused.Get()); }); // after the click has finished
					})),
					LOCTEXT("BakeEditLabel", "Bake Edit"),
					LOCTEXT("BakeEditTip", "Black Eye Fast Bake: bake every Black Eye camera this edit shows, keyed only where it shows "
					                       "them, with handles. Opens a window to set the handles first."),
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
						RunNextTick([Asset]() { OpenDialog(Cast<ULevelSequence>(Asset.TryLoad())); });
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

#undef LOCTEXT_NAMESPACE
