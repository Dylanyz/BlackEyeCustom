// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Bake Edit from the master (DESIGN section 6): every shot an edit shows is baked while the edit stays open as
// Sequencer's root. Nothing is opened, so the cost of reopening a big master (minutes, measured) is never paid.
// Each shot is evaluated alone through the root instance's OverrideRootSequence, the mechanism behind Sequencer's
// "Evaluate Sub Sequences In Isolation", which reaches the shot's frames outside its section too (handles).

#include "BlackEyeFastBakeInternal.h"

#include "CineCameraActor.h"
#include "Editor.h"
#include "EntitySystem/MovieSceneSequenceInstance.h"
#include "Evaluation/MovieSceneEvaluationTemplateInstance.h"
#include "Evaluation/MovieSceneSequenceHierarchy.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "Misc/ScopedSlowTask.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneSubSection.h"
#include "SequencerSettings.h"

#define LOCTEXT_NAMESPACE "BlackEyeFastBakeMaster"

namespace BlackEyeFastBake
{
	ACineCameraActor* SpawnScratchCamera()
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return nullptr;
		}
		// Ours alone and never saved: kept out of the level's undo history and the outliner.
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		FActorSpawnParameters Params;
		Params.ObjectFlags = RF_Transient;
		Params.bHideFromSceneOutliner = true;
		Params.bTemporaryEditorActor = true;
		Params.Name = MakeUniqueObjectName(World->GetCurrentLevel(), ACineCameraActor::StaticClass(), TEXT("BlackEyeFastBake_Scratch"));
		return World->SpawnActor<ACineCameraActor>(Params);
	}

	void DestroyScratchCamera(ACineCameraActor* Scratch)
	{
		if (Scratch && Scratch->GetWorld())
		{
			TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
			Scratch->GetWorld()->DestroyActor(Scratch, false, false); // as SequencerUtilities.cpp:3590-3595
		}
	}

	namespace Master
	{
		using UE::MovieScene::FSequenceInstance;

		/** What Sequencer itself would have the root instance evaluate (Sequencer.cpp:853-856). */
		FMovieSceneSequenceID SequencerOverride(ISequencer& Sequencer)
		{
			const USequencerSettings* Settings = Sequencer.GetSequencerSettings();
			return Settings && Settings->ShouldEvaluateSubSequencesInIsolation() ? Sequencer.GetFocusedTemplateID() : MovieSceneSequenceID::Root;
		}

		/**
		 * Evaluate only this instance from now on, with root time mapped into it, unclamped by its section
		 * (MovieSceneSequenceUpdaters.cpp:707-733). Switching unlinks what the master itself animates (:680-690);
		 * it comes back when the override is put back.
		 */
		void SetOverride(ISequencer& Sequencer, FMovieSceneSequenceIDRef ID)
		{
			if (FSequenceInstance* Root = Sequencer.GetEvaluationTemplate().FindInstance(MovieSceneSequenceID::Root))
			{
				Root->OverrideRootSequence(ID);
			}
		}

		/** One shot of the plan, reached under the master. */
		struct FShot
		{
			ULevelSequence* Shot = nullptr;
			TWeakObjectPtr<UMovieSceneSubSection> Section;
			TArray<int32> Plans;
			FMovieSceneSequenceID ID;
			FMovieSceneTimeTransform ShotToRoot;
		};

		/**
		 * The shot's instance in the compiled hierarchy: the one of the planned section if found, else any. Its root to
		 * shot transform must be linear (no time warp) to be inverted for stepping in shot frames.
		 */
		bool Resolve(ISequencer& Sequencer, FShot& Shot, FString& OutError)
		{
			const FMovieSceneSequenceHierarchy* Hierarchy = Sequencer.GetEvaluationTemplate().GetHierarchy();
			if (!Hierarchy)
			{
				OutError = TEXT("the open edit has no compiled shots");
				return false;
			}
			const FMovieSceneSubSequenceData* Found = nullptr;
			const UMovieSceneSubSection* Section = Shot.Section.Get();
			for (const TPair<FMovieSceneSequenceID, FMovieSceneSubSequenceData>& Pair : Hierarchy->AllSubSequenceData())
			{
				if (Pair.Value.GetLoadedSequence() == Shot.Shot && (!Found || (Section && Pair.Value.DeterministicSequenceID == Section->GetSequenceID())))
				{
					Found = &Pair.Value;
					Shot.ID = Pair.Key;
				}
			}
			if (!Found)
			{
				OutError = TEXT("not found under the open edit");
				return false;
			}
			if (!Found->RootToSequenceTransform.IsLinear())
			{
				OutError = TEXT("reached through a time warp or loop; bake it shot by shot");
				return false;
			}
			Shot.ShotToRoot = Found->RootToSequenceTransform.AsLinear().Inverse();
			return true;
		}

		struct FRun
		{
			TSharedRef<ISequencer> Sequencer;
			TArray<FBlackEyeShotBakePlan> Plans;
			FMasterBakeSettings Settings;
			TFunction<void(const FMasterBakeResult&)> Done;
			FMasterBakeResult Result;
			TArray<FShot> Shots;
			TArray<FBakeOutput> Outputs; // per plan
			TArray<bool> Baked;          // per plan
			FQualifiedFrameTime Time;
			FMovieSceneSequenceID Focus;

			FRun(TSharedRef<ISequencer> InSequencer) : Sequencer(InSequencer) {}

			FBlackEyeFastBakeOptions OptionsFor(int32 Plan) const
			{
				FBlackEyeFastBakeOptions Options = Settings.Options;
				Options.CameraBindingName = Plans[Plan].CameraBindingName;
				Options.Ranges = Plans[Plan].Ranges;
				Options.ProgressNote = FString::Printf(TEXT(" (%d of %d)"), Plan + 1, Plans.Num());
				return Options;
			}

			void Fail(int32 Plan, const FString& Why)
			{
				Result.Failures.Add(FString::Printf(TEXT("%s %s: %s"), *GetNameSafe(Plans[Plan].Shot), *Plans[Plan].CameraBindingName, *Why));
			}
		};

		void Finish(const TSharedRef<FRun>& Run)
		{
			ISequencer& Sequencer = *Run->Sequencer;
			SetOverride(Sequencer, SequencerOverride(Sequencer));
			Sequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::MovieSceneStructureItemsChanged);
			Sequencer.SetGlobalTime(Run->Time.Time);
			Sequencer.ForceEvaluate();
			Run->Done(Run->Result);
		}

		/** Writes each plan's twin as data, all in one transaction: the whole bake is one undo. */
		void WriteDirect(const TSharedRef<FRun>& Run)
		{
			ISequencer& Sequencer = *Run->Sequencer;
			SetOverride(Sequencer, SequencerOverride(Sequencer));
			ACineCameraActor* Scratch = SpawnScratchCamera();
			{
				const FScopedTransaction Transaction(LOCTEXT("BakeEditMaster", "Black Eye Bake Edit"));
				for (int32 i = 0; i < Run->Plans.Num(); ++i)
				{
					if (!Run->Baked[i])
					{
						continue;
					}
					FBlackEyeFastBakeReport Report;
					if (WriteTwinDirect(Run->Plans[i].Shot, Run->OptionsFor(i), Run->Outputs[i], Scratch, &Sequencer, Report))
					{
						++Run->Result.Baked;
						Run->Result.KeyedFrames += Run->Outputs[i].Samples.Num();
					}
					else
					{
						Run->Fail(i, Report.Message);
					}
				}
			}
			DestroyScratchCamera(Scratch);
			Finish(Run);
		}

		/**
		 * The tested writer: Sequencer steps into the shot within the edit (no reopening), keeps evaluating it alone so
		 * handle frames outside the section resolve, writes each twin through its spawned copy, and steps back out.
		 */
		void WriteInside(const TSharedRef<FRun>& Run, int32 ShotIndex)
		{
			ISequencer& Sequencer = *Run->Sequencer;
			if (ShotIndex >= Run->Shots.Num())
			{
				Finish(Run);
				return;
			}
			const FShot& Shot = Run->Shots[ShotIndex];
			UMovieSceneSubSection* Section = Shot.Section.Get();
			const bool bAny = Shot.Plans.ContainsByPredicate([&Run](int32 i) { return Run->Baked[i]; });
			if (!bAny || !Section)
			{
				for (int32 i : Shot.Plans)
				{
					if (Run->Baked[i])
					{
						Run->Fail(i, TEXT("its section is gone"));
					}
				}
				RunNextTick([Run, ShotIndex]() { WriteInside(Run, ShotIndex + 1); });
				return;
			}
			Sequencer.FocusSequenceInstance(*Section);
			// Focus changes what Sequencer shows; edit on the next tick, as after opening (BlackEyeFastBakeMenu.cpp).
			RunNextTick([Run, ShotIndex]()
			{
				ISequencer& Sequencer = *Run->Sequencer;
				SetOverride(Sequencer, Sequencer.GetFocusedTemplateID());
				for (int32 i : Run->Shots[ShotIndex].Plans)
				{
					if (!Run->Baked[i])
					{
						continue;
					}
					FBlackEyeFastBakeReport Report;
					Run->Outputs[i].Sequencer = Run->Sequencer;
					if (WriteTwin(Run->Plans[i].Shot, Run->OptionsFor(i), Run->Outputs[i], Report))
					{
						++Run->Result.Baked;
						Run->Result.KeyedFrames += Run->Outputs[i].Samples.Num();
					}
					else
					{
						Run->Fail(i, Report.Message);
					}
				}
				Sequencer.PopToSequenceInstance(Run->Focus);
				SetOverride(Sequencer, SequencerOverride(Sequencer));
				RunNextTick([Run, ShotIndex]() { WriteInside(Run, ShotIndex + 1); });
			});
		}
	}

	void RunMasterBake(TSharedRef<ISequencer> Sequencer, TArray<FBlackEyeShotBakePlan> Plans, const FMasterBakeSettings& Settings,
	                   TFunction<void(const FMasterBakeResult&)> Done)
	{
		using namespace Master;
		TSharedRef<FRun> Run = MakeShared<FRun>(Sequencer);
		Run->Plans = MoveTemp(Plans);
		Run->Settings = Settings;
		Run->Done = MoveTemp(Done);
		Run->Outputs.SetNum(Run->Plans.Num());
		Run->Baked.Init(false, Run->Plans.Num());
		Run->Time = Sequencer->GetGlobalTime();
		Run->Focus = Sequencer->GetFocusedTemplateID();

		for (int32 i = 0; i < Run->Plans.Num(); ++i)
		{
			FShot* Shot = Run->Shots.FindByPredicate([&](const FShot& S) { return S.Shot == Run->Plans[i].Shot; });
			if (!Shot)
			{
				Shot = &Run->Shots.AddDefaulted_GetRef();
				Shot->Shot = Run->Plans[i].Shot;
				Shot->Section = Run->Plans[i].FirstSection;
			}
			Shot->Plans.Add(i);
		}

		// Bake every camera first, one override per shot; nothing is written while stepping (trap 4.8).
		const double T0 = FPlatformTime::Seconds();
		{
			FScopedSlowTask Task(static_cast<float>(Run->Shots.Num()), LOCTEXT("MasterTask", "Black Eye Bake Edit, from the master"));
			Task.MakeDialog(true);
			for (FShot& Shot : Run->Shots)
			{
				Task.EnterProgressFrame(1.f, FText::FromString(GetNameSafe(Shot.Shot)));
				FString Error;
				if (!Resolve(*Sequencer, Shot, Error))
				{
					for (int32 i : Shot.Plans)
					{
						Run->Fail(i, Error);
					}
					continue;
				}
				SetOverride(*Sequencer, Shot.ID);
				FBakeTarget Target;
				Target.Sequencer = Sequencer;
				Target.SequenceID = Shot.ID;
				Target.ShotToRoot = Shot.ShotToRoot;
				Target.bRestoreView = false;
				for (int32 i : Shot.Plans)
				{
					const FBlackEyeFastBakeReport Report = RunBakeIn(Target, Shot.Shot, Run->OptionsFor(i), Run->Outputs[i]);
					Run->Baked[i] = Report.bSuccess;
					if (!Report.bSuccess)
					{
						Run->Result.bCancelled = Report.Message == TEXT("cancelled");
						if (!Run->Result.bCancelled)
						{
							Run->Fail(i, Report.Message);
						}
					}
					if (Run->Result.bCancelled || Task.ShouldCancel())
					{
						Run->Result.bCancelled = true;
						break;
					}
				}
				if (Run->Result.bCancelled)
				{
					break;
				}
			}
		}
		Run->Result.Seconds = FPlatformTime::Seconds() - T0;

		if (Run->Result.bCancelled)
		{
			Finish(Run); // nothing written: a cancelled bake leaves every twin as it was
		}
		else if (Settings.bWriteDirect)
		{
			WriteDirect(Run);
		}
		else
		{
			SetOverride(*Sequencer, SequencerOverride(*Sequencer));
			RunNextTick([Run]() { WriteInside(Run, 0); });
		}
	}
}

#undef LOCTEXT_NAMESPACE
