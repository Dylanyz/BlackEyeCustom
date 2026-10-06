// SPDX-License-Identifier: Apache-2.0
//
// Fast Bake in Sequencer: right-click a Black Eye camera's binding > Black Eye Fast Bake > Bake / Lock / Unlock.

#include "BlackEyeFastBakeInternal.h"

#include "BlackEyeContract.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ISequencerModule.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Modules/ModuleManager.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "MovieScene.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "BlackEyeFastBake"

namespace BlackEyeFastBake
{
	FDelegateHandle MenuExtenderHandle;

	/** The focused sequence's binding that holds this object, by display name (what BakeShot takes). */
	FString BindingNameFor(ULevelSequence* Sequence, const UObject* Object)
	{
		FString Error;
		TSharedPtr<ISequencer> Sequencer = OpenSequencer(Sequence, Error);
		if (!Sequencer || !Object)
		{
			return FString();
		}
		const UMovieScene* MovieScene = Sequence->GetMovieScene();
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			for (const TWeakObjectPtr<>& Bound : Sequencer->FindBoundObjects(Binding.GetObjectGuid(), Sequencer->GetFocusedTemplateID()))
			{
				if (Bound.Get() == Object)
				{
					return MovieScene->GetObjectDisplayName(Binding.GetObjectGuid()).ToString();
				}
			}
		}
		return FString();
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

	void BuildMenu(FMenuBuilder& MenuBuilder, TWeakObjectPtr<UObject> WeakCamera)
	{
		ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetFocusedLevelSequence();
		const FString Name = BindingNameFor(Sequence, WeakCamera.Get());
		if (!Sequence || Name.IsEmpty())
		{
			return;
		}
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

		MenuBuilder.AddMenuEntry(
			Baked ? LOCTEXT("Rebake", "Re-bake and lock") : LOCTEXT("Bake", "Bake and lock"),
			FText::Format(LOCTEXT("BakeTip", "Solve this Black Eye camera offline over the whole playback range, write it as keys on its twin "
			                                 "camera ({0}_Bake) and make the shot play the twin. One undo step.\n\n{1}"), FText::FromString(Name), Status),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([WeakSequence, Name]()
			{
				FBlackEyeFastBakeOptions Options;
				Options.CameraBindingName = Name;
				const FBlackEyeFastBakeReport Report = UBlackEyeFastBakeLibrary::BakeShot(WeakSequence.Get(), Options);
				Notify(Report.bSuccess
					? FText::Format(LOCTEXT("BakeDone", "Baked {0}: {1} frames in {2}s ({3}x realtime){4}"), FText::FromString(Name),
						Report.NumFrames, FText::AsNumber(Report.TotalSeconds), FText::AsNumber(FMath::RoundToInt(Report.SpeedVsRealtime)),
						Report.bLocked ? LOCTEXT("AndLocked", ", locked") : FText())
					: FText::Format(LOCTEXT("BakeFailed", "Fast Bake failed for {0}: {1}"), FText::FromString(Name), FText::FromString(Report.Message)),
					Report.bSuccess);
			})));

		if (Baked)
		{
			MenuBuilder.AddMenuEntry(
				Baked->bLocked ? LOCTEXT("Unlock", "Unlock (play the live camera)") : LOCTEXT("Lock", "Lock (play the bake)"),
				Status,
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WeakSequence, Name, bLock = !Baked->bLocked]()
				{
					const int32 Changed = SetLocked(WeakSequence.Get(), Name, bLock);
					Notify(FText::Format(bLock ? LOCTEXT("LockedN", "{0} locked: {1} camera cut(s) play the bake")
					                           : LOCTEXT("UnlockedN", "{0} unlocked: {1} camera cut(s) play the live camera"),
					                     FText::FromString(Name), Changed), Changed > 0);
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

	void RegisterMenus()
	{
		ISequencerModule& Sequencer = FModuleManager::LoadModuleChecked<ISequencerModule>("Sequencer");
		TArray<FAssetEditorExtender>& Delegates = Sequencer.GetObjectBindingContextMenuExtensibilityManager()->GetExtenderDelegates();
		Delegates.Add(FAssetEditorExtender::CreateStatic(&ExtendBindingMenu));
		MenuExtenderHandle = Delegates.Last().GetHandle();
	}

	void UnregisterMenus()
	{
		if (ISequencerModule* Sequencer = FModuleManager::GetModulePtr<ISequencerModule>("Sequencer"))
		{
			Sequencer->GetObjectBindingContextMenuExtensibilityManager()->GetExtenderDelegates().RemoveAll(
				[](const FAssetEditorExtender& Delegate) { return Delegate.GetHandle() == MenuExtenderHandle; });
		}
	}
}

#undef LOCTEXT_NAMESPACE
