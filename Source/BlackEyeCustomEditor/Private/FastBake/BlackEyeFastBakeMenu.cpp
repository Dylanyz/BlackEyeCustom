// SPDX-License-Identifier: Apache-2.0
//
// Fast Bake in Sequencer: right-click a Black Eye camera's binding > Black Eye Fast Bake > Bake / Lock / Unlock.
//
// Two rules keep the menu safe (a crash taught both, 2026-10-06):
// - Building the menu has no side effects. It finds the binding through the Sequencer that is already open, never by
//   opening an asset editor: opening one replaced the Sequencer that owned the open menu, and Slate then used it.
// - Actions run on the next tick, after the menu has closed, because a bake may open the shot as Sequencer's root.

#include "BlackEyeFastBakeInternal.h"

#include "BlackEyeContract.h"
#include "Containers/Ticker.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "LevelSequence.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "Sections/MovieSceneSubSection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Editor.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "BlackEyeFastBake"

namespace BlackEyeFastBake
{
	FDelegateHandle MenuExtenderHandle;
	FDelegateHandle SequencerCreatedHandle;
	TArray<TWeakPtr<ISequencer>> OpenSequencers; // every Sequencer created, so the menu can find the one it is in

	/** Where a right-clicked camera is bound: the Sequencer showing it, its focused sequence, and the binding name. */
	struct FBindingContext
	{
		TWeakPtr<ISequencer> Sequencer;
		TWeakObjectPtr<ULevelSequence> Sequence;
		FString Name;
	};

	FBindingContext FindBinding(const UObject* Object)
	{
		FBindingContext Out;
		OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
		for (const TWeakPtr<ISequencer>& Weak : OpenSequencers)
		{
			const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
			ULevelSequence* Focused = Sequencer ? Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()) : nullptr;
			if (!Focused || !Object)
			{
				continue;
			}
			const UMovieScene* MovieScene = Focused->GetMovieScene();
			for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
			{
				for (const TWeakObjectPtr<>& Bound : Sequencer->FindBoundObjects(Binding.GetObjectGuid(), Sequencer->GetFocusedTemplateID()))
				{
					if (Bound.Get() == Object)
					{
						Out.Sequencer = Sequencer;
						Out.Sequence = Focused;
						Out.Name = MovieScene->GetObjectDisplayName(Binding.GetObjectGuid()).ToString();
						return Out;
					}
				}
			}
		}
		return Out;
	}

	void Notify(const FText& Text, bool bSuccess)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = bSuccess ? 4.f : 8.f;
		Info.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	void RunNextTick(TFunction<void()> Action)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Action = MoveTemp(Action)](float)
		{
			Action();
			return false; // once
		}));
	}

	/** Re-opens the sequence the user was in and focuses back into the shot, if the bake had to open the shot alone. */
	void RestoreView(ULevelSequence* Root, ULevelSequence* Shot)
	{
		UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Root || !AssetEditors || Root == Shot)
		{
			return;
		}
		AssetEditors->OpenEditorForAsset(Root);
		IAssetEditorInstance* Editor = AssetEditors->FindEditorForAsset(Root, false);
		TSharedPtr<ISequencer> Sequencer = Editor ? static_cast<ILevelSequenceEditorToolkit*>(Editor)->GetSequencer() : nullptr;
		if (!Sequencer)
		{
			return;
		}
		for (UMovieSceneTrack* Track : Root->GetMovieScene()->GetTracks())
		{
			if (UMovieSceneSubTrack* Sub = Cast<UMovieSceneSubTrack>(Track))
			{
				for (UMovieSceneSection* Section : Sub->GetAllSections())
				{
					UMovieSceneSubSection* SubSection = Cast<UMovieSceneSubSection>(Section);
					if (SubSection && SubSection->GetSequence() == Shot)
					{
						Sequencer->FocusSequenceInstance(*SubSection);
						return;
					}
				}
			}
		}
	}

	void BuildMenu(FMenuBuilder& MenuBuilder, TWeakObjectPtr<UObject> WeakCamera)
	{
		const FBindingContext Context = FindBinding(WeakCamera.Get());
		ULevelSequence* Sequence = Context.Sequence.Get();
		if (!Sequence || Context.Name.IsEmpty())
		{
			MenuBuilder.AddMenuEntry(LOCTEXT("NoBinding", "Camera binding not found"), FText(), FSlateIcon(), FUIAction());
			return;
		}
		const FString Name = Context.Name;
		const FBlackEyeBakeInfo* Baked = nullptr;
		const TArray<FBlackEyeBakeInfo> Infos = GetBakeInfo(Sequence);
		for (const FBlackEyeBakeInfo& Info : Infos)
		{
			if (Info.CameraBindingName == Name)
			{
				Baked = &Info;
			}
		}
		const FText Status = Baked
			? FText::Format(LOCTEXT("Baked", "{0}. Twin: {1}. {2}"), Baked->bLocked ? LOCTEXT("IsLocked", "Locked: the shot plays the bake")
			                                                                      : LOCTEXT("IsUnlocked", "Unlocked: the shot plays the live camera"),
			                FText::FromString(Baked->TwinBindingName), FText::FromString(Baked->Info))
			: LOCTEXT("NotBaked", "Not baked yet.");
		const TWeakObjectPtr<ULevelSequence> WeakSequence(Sequence);
		const TWeakPtr<ISequencer> WeakSequencer = Context.Sequencer;

		MenuBuilder.AddMenuEntry(
			Baked ? LOCTEXT("Rebake", "Re-bake and lock") : LOCTEXT("Bake", "Bake and lock"),
			FText::Format(LOCTEXT("BakeTip", "Solve this Black Eye camera offline over the shot's whole playback range, write it as keys on "
			                                 "its twin camera ({0}_Bake) and make the shot play the twin. One undo step. If the shot is open "
			                                 "inside an edit, it is opened on its own for the bake, then you are put back.\n\n{1}"),
			              FText::FromString(Name), Status),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([WeakSequence, WeakSequencer, Name]()
			{
				RunNextTick([WeakSequence, WeakSequencer, Name]()
				{
					ULevelSequence* Shot = WeakSequence.Get();
					const TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();
					ULevelSequence* Root = Sequencer ? Cast<ULevelSequence>(Sequencer->GetRootMovieSceneSequence()) : nullptr;
					FBlackEyeFastBakeOptions Options;
					Options.CameraBindingName = Name;
					const FBlackEyeFastBakeReport Report = UBlackEyeFastBakeLibrary::BakeShot(Shot, Options);
					RestoreView(Root, Shot);
					Notify(Report.bSuccess
						? FText::Format(LOCTEXT("BakeDone", "Baked {0}: {1} frames in {2}s ({3}x realtime){4}"), FText::FromString(Name),
							Report.NumFrames, FText::AsNumber(FMath::RoundToInt(Report.TotalSeconds)), FText::AsNumber(FMath::RoundToInt(Report.SpeedVsRealtime)),
							Report.bLocked ? LOCTEXT("AndLocked", ", locked") : FText())
						: FText::Format(LOCTEXT("BakeFailed", "Fast Bake failed for {0}: {1}"), FText::FromString(Name), FText::FromString(Report.Message)),
						Report.bSuccess);
				});
			})));

		if (Baked)
		{
			MenuBuilder.AddMenuEntry(
				Baked->bLocked ? LOCTEXT("Unlock", "Unlock (play the live camera)") : LOCTEXT("Lock", "Lock (play the bake)"),
				Status,
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WeakSequence, WeakSequencer, Name, bLock = !Baked->bLocked]()
				{
					RunNextTick([WeakSequence, WeakSequencer, Name, bLock]()
					{
						const int32 Changed = SetLocked(WeakSequence.Get(), Name, bLock);
						if (const TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin())
						{
							Sequencer->NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
						}
						Notify(FText::Format(bLock ? LOCTEXT("LockedN", "{0} locked: {1} camera cut(s) play the bake")
						                           : LOCTEXT("UnlockedN", "{0} unlocked: {1} camera cut(s) play the live camera"),
						                     FText::FromString(Name), Changed), Changed > 0);
					});
				})));
		}
	}

	TSharedRef<FExtender> ExtendBindingMenu(const TSharedRef<FUICommandList>, const TArray<UObject*> Objects)
	{
		TSharedRef<FExtender> Extender = MakeShared<FExtender>();
		UObject* Camera = Objects.Num() == 1 ? Objects[0] : nullptr;
		if (BlackEyeContract::IsBlackEyeCamera(Cast<AActor>(Camera)))
		{
			Extender->AddMenuExtension("ObjectBindingActions", EExtensionHook::After, nullptr,
				FMenuExtensionDelegate::CreateLambda([WeakCamera = TWeakObjectPtr<UObject>(Camera)](FMenuBuilder& MenuBuilder)
				{
					MenuBuilder.AddSubMenu(LOCTEXT("SubMenu", "Black Eye Fast Bake"),
						LOCTEXT("SubMenuTip", "Bake this Black Eye camera to keys, and lock or unlock the bake"),
						FNewMenuDelegate::CreateLambda([WeakCamera](FMenuBuilder& Sub) { BuildMenu(Sub, WeakCamera); }));
				}));
		}
		return Extender;
	}

	/**
	 * `BlackEyeCustom.FastBake.MenuTest` builds the submenu, off screen, for every Black Eye camera in every open
	 * Sequencer's focused sequence, exactly as hovering it does, and logs what it found. No clicks needed.
	 */
	FAutoConsoleCommand GMenuTest(TEXT("BlackEyeCustom.FastBake.MenuTest"),
		TEXT("Build the Fast Bake binding submenu for every Black Eye camera in the open Sequencers, and log the result."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			int32 Built = 0;
			for (const TWeakPtr<ISequencer>& Weak : TArray<TWeakPtr<ISequencer>>(OpenSequencers))
			{
				const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
				const UMovieSceneSequence* Focused = Sequencer ? Sequencer->GetFocusedMovieSceneSequence() : nullptr;
				if (!Focused)
				{
					continue;
				}
				for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(Focused->GetMovieScene())->GetBindings())
				{
					for (const TWeakObjectPtr<>& Bound : Sequencer->FindBoundObjects(Binding.GetObjectGuid(), Sequencer->GetFocusedTemplateID()))
					{
						if (BlackEyeContract::IsBlackEyeCamera(Cast<AActor>(Bound.Get())))
						{
							FMenuBuilder Menu(true, nullptr);
							BuildMenu(Menu, Bound);
							const FBindingContext Context = FindBinding(Bound.Get());
							UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] menu test: %s in %s -> binding '%s'"),
								*Bound->GetName(), *Focused->GetName(), *Context.Name);
							++Built;
						}
					}
				}
			}
			UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] menu test: built %d menu(s), %d Sequencer(s) open"), Built, OpenSequencers.Num());
		}));

	void RegisterMenus()
	{
		ISequencerModule& Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>("Sequencer");
		TArray<FAssetEditorExtender>& Delegates = Sequencer.GetObjectBindingContextMenuExtensibilityManager()->GetExtenderDelegates();
		Delegates.Add(FAssetEditorExtender::CreateStatic(&ExtendBindingMenu));
		MenuExtenderHandle = Delegates.Last().GetHandle();
		SequencerCreatedHandle = Sequencer.RegisterOnSequencerCreated(FOnSequencerCreated::FDelegate::CreateLambda(
			[](TSharedRef<ISequencer> Created) { OpenSequencers.Add(Created); }));
	}

	void UnregisterMenus()
	{
		if (ISequencerModule* Sequencer = FModuleManager::GetModulePtr<ISequencerModule>("Sequencer"))
		{
			Sequencer->GetObjectBindingContextMenuExtensibilityManager()->GetExtenderDelegates().RemoveAll(
				[](const FAssetEditorExtender& Delegate) { return Delegate.GetHandle() == MenuExtenderHandle; });
			Sequencer->UnregisterOnSequencerCreated(SequencerCreatedHandle);
		}
		OpenSequencers.Reset();
	}
}

#undef LOCTEXT_NAMESPACE
