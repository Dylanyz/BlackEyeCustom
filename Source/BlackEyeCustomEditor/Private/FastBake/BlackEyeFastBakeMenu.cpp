// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Fast Bake in Sequencer: right-click a Black Eye camera's binding > Black Eye Fast Bake > Bake..., which opens the
// Bake window (BlackEyeFastBakeEdit.cpp) with that camera picked.
//
// Two rules keep the menu safe (a crash taught both, 2026-10-06):
// - Building the menu has no side effects. It finds the binding through the Sequencer that is already open, never by
//   opening an asset editor: opening one replaced the Sequencer that owned the open menu, and Slate then used it.
// - Actions run on the next tick, after the menu has closed.

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
#include "Editor.h"
#include "Styling/AppStyle.h"
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

	/** Runs once, after Delay seconds (0 = next tick), outside whatever Slate or Sequencer is doing now. */
	void RunLater(TFunction<void()> Action, float Delay = 0.f)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Action = MoveTemp(Action)](float)
		{
			Action();
			return false; // once
		}), Delay);
	}

	void RunNextTick(TFunction<void()> Action)
	{
		RunLater(MoveTemp(Action));
	}

	// A Sequencer opened this tick isn't ready to be edited: its track editors and tree are set up over the next
	// frames, and editing its sequence in the same tick crashed (Sequencer asserted while the bake's transaction closed,
	// after "Unable to find a track editor for track type MovieSceneFloatTrack", 2026-10-06). So any step that follows
	// opening a sequence waits this long AND this many frames: a ticker delay alone is measured in delta time
	// (Ticker.cpp:16, 114), so the long frame in which a big shot first spawns satisfies it after a single frame.
	constexpr double SequencerSettleSeconds = 0.5;
	constexpr int32 SequencerSettleFrames = 3;

	/** Opens a sequence as Sequencer's root, then runs Then once that Sequencer has settled. */
	void OpenThen(ULevelSequence* Sequence, TFunction<void()> Then)
	{
		UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (AssetEditors && Sequence)
		{
			AssetEditors->OpenEditorForAsset(Sequence);
		}
		const double Start = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Then = MoveTemp(Then), Start, Frames = 0](float) mutable
			{
				if (++Frames < SequencerSettleFrames || FPlatformTime::Seconds() - Start < SequencerSettleSeconds)
				{
					return true; // keep waiting
				}
				Then();
				return false;
			}));
	}

	/**
	 * `BlackEyeCustom.FastBake.Bake <binding>`: the camera menu's Bake without the window, with its last settings and
	 * lock on, on the focused sequence of the open Sequencer.
	 */
	FAutoConsoleCommand GBakeCommand(TEXT("BlackEyeCustom.FastBake.Bake"),
		TEXT("Bake and lock a Black Eye camera of the focused sequence, as the camera menu's Bake window would with its last settings. Arg: camera binding name (optional)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
			for (const TWeakPtr<ISequencer>& Weak : OpenSequencers)
			{
				const TSharedPtr<ISequencer> Sequencer = Weak.Pin();
				if (Sequencer && Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()))
				{
					OpenBakeForCamera(Sequencer, Args.Num() ? Args[0] : FString(), true);
					return;
				}
			}
			UE_LOG(LogBlackEyeCustom, Warning, TEXT("[BlackEyeCustom] no open Sequencer to bake from"));
		}));

	/**
	 * One entry, the same Bake window as the toolbar's, with this camera picked and lock on. Everything else (which bake
	 * camera, lock / unlock, how to bake) lives in that window, so there is one place to change it.
	 */
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
			? FText::Format(LOCTEXT("Baked", "{0}. Bake camera: {1}. {2}"), Baked->bLocked ? LOCTEXT("IsLocked", "Locked: the shot plays the bake")
			                                                                            : LOCTEXT("IsUnlocked", "Unlocked: the shot plays the live camera"),
			                FText::FromString(Baked->TwinBindingName), FText::FromString(Baked->Info))
			: LOCTEXT("NotBaked", "Not baked yet.");
		const TWeakPtr<ISequencer> WeakSequencer = Context.Sequencer;

		MenuBuilder.AddMenuEntry(
			LOCTEXT("Bake", "Bake..."),
			FText::Format(LOCTEXT("BakeTip", "Opens the Black Eye Fast Bake window with {0} picked and lock on: bake it, choose its bake "
			                                 "camera, lock or unlock it.\n\n{1}"), FText::FromString(Name), Status),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Lock"),
			FUIAction(FExecuteAction::CreateLambda([WeakSequencer, Name]()
			{
				// After the menu has closed: the window is modal.
				RunNextTick([WeakSequencer, Name]() { OpenBakeForCamera(WeakSequencer.Pin(), Name, false); });
			})));
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
						LOCTEXT("SubMenuTip", "Bake this Black Eye camera to keys, choose its bake camera, lock or unlock it"),
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
