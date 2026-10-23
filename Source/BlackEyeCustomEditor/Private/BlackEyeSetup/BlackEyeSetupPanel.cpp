// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// The Black Eye Setup window: Sequencer toolbar > Black Eye Setup. Three switches (Follow, LookAt, Focus), each with
// its subject, for one Black Eye camera of the sequence that Sequencer shows; plus console commands for scripts.
// Actions run on the next tick, after the click has finished (the rule BlackEyeFastBakeMenu.cpp learned).

#include "BlackEyeSetupInternal.h"

#include "BlackEyeContract.h"
#include "FastBake/BlackEyeFastBakeInternal.h"

#include "Camera/CameraComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ISequencer.h"
#include "LevelSequence.h"
#include "SequencerToolMenuContext.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateIconFinder.h"
#include "ToolMenus.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "BlackEyeSetup"

namespace BlackEyeFastBake
{
	extern TArray<TWeakPtr<ISequencer>> OpenSequencers; // BlackEyeFastBakeMenu.cpp
}

namespace BlackEyeSetup
{
	namespace
	{
		/** The Sequencer showing a Level Sequence, most recently created first. */
		TSharedPtr<ISequencer> ActiveSequencer()
		{
			BlackEyeFastBake::OpenSequencers.RemoveAll([](const TWeakPtr<ISequencer>& S) { return !S.IsValid(); });
			for (int32 i = BlackEyeFastBake::OpenSequencers.Num() - 1; i >= 0; --i)
			{
				const TSharedPtr<ISequencer> Sequencer = BlackEyeFastBake::OpenSequencers[i].Pin();
				if (Sequencer && Cast<ULevelSequence>(Sequencer->GetFocusedMovieSceneSequence()))
				{
					return Sequencer;
				}
			}
			return nullptr;
		}

		AActor* SelectedActor()
		{
			USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
			if (!Selection)
			{
				return nullptr;
			}
			for (FSelectionIterator It(*Selection); It; ++It)
			{
				AActor* Actor = Cast<AActor>(*It);
				if (Actor && !BlackEyeContract::IsBlackEyeCamera(Actor))
				{
					return Actor;
				}
			}
			return nullptr;
		}

		FString Describe(const FBlackEyeSetupSubject& Subject)
		{
			if (!Subject.Actor)
			{
				return TEXT("(no subject)");
			}
			FString Out = Subject.Actor->GetActorLabel();
			if (!Subject.Bone.IsEmpty())
			{
				Out += FString::Printf(TEXT("  >  %s  >  %s"), Subject.Component.IsEmpty() ? TEXT("root") : *Subject.Component, *Subject.Bone);
			}
			return Out;
		}

		void LogResult(const FString& Result)
		{
			TArray<FString> Lines;
			Result.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] setup %s"), *Line);
			}
		}
	}

	/** The window's content. It re-reads the camera a few times a second, so edits made elsewhere show up. */
	class SBlackEyeSetupPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SBlackEyeSetupPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, TSharedPtr<ISequencer> InSequencer)
		{
			Sequencer = InSequencer;
			Refresh();
			RegisterActiveTimer(0.4f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
			{
				Refresh();
				return EActiveTimerReturnType::Continue;
			}));

			ChildSlot
			[
				SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder")).Padding(12)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[ SNew(STextBlock).Text(LOCTEXT("Camera", "Camera")).Font(FAppStyle::GetFontStyle("BoldFont")) ]
						+ SHorizontalBox::Slot().FillWidth(1.f)
						[
							SAssignNew(CameraCombo, SComboBox<TSharedPtr<FString>>)
							.OptionsSource(&CameraNames)
							.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(*Item)); })
							.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type Info)
							{
								if (Item && Info != ESelectInfo::Direct)
								{
									CameraName = *Item;
									Refresh();
								}
							})
							[ SNew(STextBlock).Text_Lambda([this]() { return FText::FromString(State.bValid ? State.CameraBindingName : FString(TEXT("(none)"))); }) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bFromPlayhead ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { bFromPlayhead = NewState == ECheckBoxState::Checked; })
						.ToolTipText(LOCTEXT("FromPlayheadTip", "Off: a switch or subject applies to the whole shot.\nOn: from the playhead on only. "
						                                        "Follow and LookAt are keyed there and the Transform section is split there; a subject "
						                                        "is keyed there (same component and bone as before, e.g. tracker to tracker). Focus "
						                                        "on/off is always the whole shot: the lens's Focus Method can't be keyed."))
						[ SNew(STextBlock).Text(LOCTEXT("FromPlayhead", "From the playhead on (keyed)")) ]
					]
					+ SVerticalBox::Slot().AutoHeight()[ Row(EBlackEyeSetupSwitch::Follow, LOCTEXT("Follow", "Follow"),
						LOCTEXT("FollowTip", "On: the camera follows its subject (Follow Offset measured from where it is now; the Transform track's "
						                     "Location is switched off).\nOff: the camera stays put and its position is yours (Transform track Location).")) ]
					+ SVerticalBox::Slot().AutoHeight()[ Row(EBlackEyeSetupSwitch::LookAt, LOCTEXT("LookAt", "LookAt"),
						LOCTEXT("LookAtTip", "On: the camera aims at its subject (the Transform track's Rotation is switched off).\nOff: the aim stays "
						                     "where it is and becomes yours (Transform track Rotation); Follow then only moves the camera.")) ]
					+ SVerticalBox::Slot().AutoHeight()[ Row(EBlackEyeSetupSwitch::Focus, LOCTEXT("Focus", "Focus"),
						LOCTEXT("FocusTip", "On: the lens focuses on its subject every frame (CineCamera Tracking focus; works with LookAt off).\n"
						                    "Off: focus is set by hand, held where it is now.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("Fill", "Fill all from selected character"))
						.ToolTipText(LOCTEXT("FillTip", "Select a character in the level. Follow: its Heading socket (else pelvis). LookAt and "
						                                "Focus: its tracker (attached to it, or resting at its head; else its head bone). "
						                                "The switches stay as they are."))
						.IsEnabled_Lambda([this]() { return State.bValid; })
						.OnClicked_Lambda([this]()
						{
							Run([](const FCamera& Camera) { return Autofill(Camera, SelectedActor()); });
							return FReply::Handled();
						})
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("AllOff", "All off (plain CineCamera)"))
						.ToolTipText(LOCTEXT("AllOffTip", "Follow, LookAt and Focus off, even when they already read off or have no subject. "
						                                  "The camera stays where it is; the Transform track moves and aims it, focus is manual. "
						                                  "Subjects are kept (weight 0), so a switch can come back on later."))
						.IsEnabled_Lambda([this]() { return State.bValid; })
						.OnClicked_Lambda([this]()
						{
							Run([](const FCamera& Camera) { return AllOff(Camera); });
							return FReply::Handled();
						})
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
					[
						SNew(STextBlock).AutoWrapText(true)
						.Text_Lambda([this]()
						{
							FString Text = State.bValid ? FString() : State.Message;
							for (const FString& Warning : State.Warnings)
							{
								Text += (Text.IsEmpty() ? TEXT("") : TEXT("\n")) + Warning;
							}
							if (!Status.IsEmpty())
							{
								Text += (Text.IsEmpty() ? TEXT("") : TEXT("\n\n")) + Status;
							}
							return FText::FromString(Text);
						})
					]
				]
			];
		}

		void SetSequencer(TSharedPtr<ISequencer> InSequencer)
		{
			Sequencer = InSequencer;
			CameraName.Reset();
			Refresh();
		}

	private:
		TWeakPtr<ISequencer> Sequencer;
		FString CameraName;               // empty until picked: FindCamera's own choice
		bool bFromPlayhead = false;       // switches and subjects apply from the playhead on (keyed), not to the whole shot
		// Only values, never the subject actors: a level change or a deleted actor would leave the window drawing a
		// destroyed object (it crashed so, 2026-10-21). Refresh turns the actors into text and drops them.
		FBlackEyeSetupState State;
		FString SubjectText[3];
		FString Status;
		TArray<TSharedPtr<FString>> CameraNames;
		TSharedPtr<SComboBox<TSharedPtr<FString>>> CameraCombo;

		bool Resolve(FCamera& Out)
		{
			FString Error;
			if (!FindCamera(Sequencer.Pin(), CameraName, Out, Error))
			{
				State = FBlackEyeSetupState();
				State.Message = Error;
				return false;
			}
			CameraName = Out.Name;
			return true;
		}

		void Refresh()
		{
			const TArray<FString> Names = ListCameras(Sequencer.Pin());
			if (Names.Num() != CameraNames.Num() || Names.ContainsByPredicate([this](const FString& N) { return !CameraNames.ContainsByPredicate([&N](const TSharedPtr<FString>& C) { return *C == N; }); }))
			{
				CameraNames.Reset();
				for (const FString& Name : Names)
				{
					CameraNames.Add(MakeShared<FString>(Name));
				}
				if (CameraCombo)
				{
					CameraCombo->RefreshOptions();
				}
			}
			FCamera Camera;
			if (Resolve(Camera))
			{
				State = ReadState(Camera);
			}
			FBlackEyeSetupSubject* Subjects[3] = { &State.FollowSubject, &State.LookAtSubject, &State.FocusSubject };
			for (int32 i = 0; i < 3; ++i)
			{
				SubjectText[i] = State.bValid ? Describe(*Subjects[i]) : FString();
				Subjects[i]->Actor = nullptr;
			}
		}

		void Run(TFunction<FString(const FCamera&)> Action)
		{
			const TWeakPtr<SBlackEyeSetupPanel> Weak = StaticCastSharedRef<SBlackEyeSetupPanel>(AsShared());
			BlackEyeFastBake::RunNextTick([Weak, Action]()
			{
				const TSharedPtr<SBlackEyeSetupPanel> Panel = Weak.Pin();
				if (!Panel)
				{
					return;
				}
				FCamera Camera;
				if (!Panel->Resolve(Camera))
				{
					Panel->Status = Panel->State.Message;
					return;
				}
				Panel->Status = Action(Camera);
				LogResult(Panel->Status);
				Panel->Refresh();
			});
		}

		bool IsOn(EBlackEyeSetupSwitch Switch) const
		{
			return Switch == EBlackEyeSetupSwitch::Follow ? State.bFollow : Switch == EBlackEyeSetupSwitch::LookAt ? State.bLookAt : State.bFocus;
		}

		const FString& SubjectOf(EBlackEyeSetupSwitch Switch) const
		{
			return SubjectText[static_cast<int32>(Switch)];
		}

		TSharedRef<SWidget> Row(EBlackEyeSetupSwitch Switch, const FText& Name, const FText& Tip)
		{
			return SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder")).Padding(8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(64.f)
					[ SNew(STextBlock).Text(Name).Font(FAppStyle::GetFontStyle("BoldFont")).ToolTipText(Tip) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
				[
					SNew(SBox).WidthOverride(72.f)
					[
						SNew(SCheckBox)
						.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
						.ToolTipText(Tip)
						.IsEnabled_Lambda([this]() { return State.bValid; })
						.IsChecked_Lambda([this, Switch]() { return IsOn(Switch) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this, Switch](ECheckBoxState NewState)
						{
							const bool bOn = NewState == ECheckBoxState::Checked;
							const bool bHere = bFromPlayhead;
							Run([Switch, bOn, bHere](const FCamera& Camera) { return SetSwitch(Camera, Switch, bOn, bHere); });
						})
						[
							SNew(SBox).HAlign(HAlign_Center).MinDesiredWidth(56.f).Padding(FMargin(2, 1))
							[ SNew(STextBlock).Text_Lambda([this, Switch]() { return IsOn(Switch) ? LOCTEXT("SwitchOn", "ON") : LOCTEXT("SwitchOff", "OFF"); }) ]
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text_Lambda([this, Switch]() { return FText::FromString(SubjectOf(Switch)); }) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("UseSelected", "Use Selected"))
					.ToolTipText(LOCTEXT("UseSelectedTip", "The actor selected in the level becomes this switch's subject. A character gives its "
					                                       "Heading socket / pelvis (Follow) or its tracker / head (LookAt, Focus)."))
					.IsEnabled_Lambda([this]() { return State.bValid; })
					.OnClicked_Lambda([this, Switch]()
					{
						const bool bHere = bFromPlayhead;
						Run([Switch, bHere](const FCamera& Camera)
						{
							AActor* Actor = SelectedActor();
							if (!Actor)
							{
								return FString(TEXT("error: select an actor in the level first"));
							}
							FString Note;
							const FBlackEyeSetupSubject Subject = SubjectFromActor(Switch, Actor, Note);
							const FString Result = bHere ? SetSubjectFromPlayhead(Camera, Switch, Subject) : SetSubject(Camera, Switch, Subject);
							return Note.IsEmpty() ? Result : Result + TEXT("\nnote: ") + Note;
						});
						return FReply::Handled();
					})
				]
			];
		}
	};

	namespace
	{
		TWeakPtr<SWindow> OpenWindow;
		TWeakPtr<SBlackEyeSetupPanel> OpenPanel;
		const FName MenuOwner(TEXT("BlackEyeSetup"));
		FDelegateHandle ToolMenusStartup;

		void OpenPanelFor(TSharedPtr<ISequencer> Sequencer)
		{
			if (const TSharedPtr<SWindow> Window = OpenWindow.Pin())
			{
				if (const TSharedPtr<SBlackEyeSetupPanel> Panel = OpenPanel.Pin())
				{
					Panel->SetSequencer(Sequencer);
				}
				Window->BringToFront();
				return;
			}
			TSharedPtr<SBlackEyeSetupPanel> Panel;
			const TSharedRef<SWindow> Window = SNew(SWindow)
				.Title(LOCTEXT("Title", "Black Eye Setup"))
				.ClientSize(FVector2D(600, 360))
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				[ SAssignNew(Panel, SBlackEyeSetupPanel, Sequencer) ];
			OpenWindow = Window;
			OpenPanel = Panel;
			FSlateApplication::Get().AddWindow(Window);
		}

		void ExtendMenus()
		{
			const FToolMenuOwnerScoped Owner(MenuOwner);
			UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu(TEXT("Sequencer.MainToolBar"));
			Toolbar->AddDynamicSection(TEXT("BlackEyeSetup"), FNewToolMenuDelegate::CreateLambda([](UToolMenu* Menu)
			{
				const USequencerToolMenuContext* Context = Menu->FindContext<USequencerToolMenuContext>();
				if (!Context)
				{
					return;
				}
				const TWeakPtr<ISequencer> WeakSequencer = Context->WeakSequencer;
				FToolMenuSection& Section = Menu->AddSection(TEXT("BlackEyeSetup"));
				Section.AddEntry(FToolMenuEntry::InitToolBarButton(TEXT("BlackEyeSetup"),
					FUIAction(FExecuteAction::CreateLambda([WeakSequencer]()
					{
						BlackEyeFastBake::RunNextTick([WeakSequencer]() { OpenPanelFor(WeakSequencer.Pin()); });
					})),
					LOCTEXT("SetupLabel", "Black Eye Setup"),
					LOCTEXT("SetupTip", "Follow, LookAt and Focus on or off for a Black Eye camera in this sequence, each with its subject. "
					                    "Each switch also sets the camera's tracks so nothing fights it and the camera stays where it is."),
					FSlateIconFinder::FindIconForClass(UCameraComponent::StaticClass())));
			}));
		}

		// Console: the same actions for scripts and tests, on the Sequencer that shows a Level Sequence.
		bool ParseSwitch(const FString& Text, EBlackEyeSetupSwitch& Out)
		{
			if (Text.Equals(TEXT("Follow"), ESearchCase::IgnoreCase)) { Out = EBlackEyeSetupSwitch::Follow; return true; }
			if (Text.Equals(TEXT("LookAt"), ESearchCase::IgnoreCase)) { Out = EBlackEyeSetupSwitch::LookAt; return true; }
			if (Text.Equals(TEXT("Focus"), ESearchCase::IgnoreCase)) { Out = EBlackEyeSetupSwitch::Focus; return true; }
			return false;
		}

		void WithCamera(const FString& Name, TFunctionRef<FString(const FCamera&)> Action)
		{
			FCamera Camera;
			FString Error;
			LogResult(FindCamera(ActiveSequencer(), Name, Camera, Error) ? Action(Camera) : TEXT("error: ") + Error);
		}

		FAutoConsoleCommand GOpen(TEXT("BlackEyeCustom.Setup.Open"), TEXT("Open the Black Eye Setup window for the open sequence."),
			FConsoleCommandDelegate::CreateLambda([]() { OpenPanelFor(ActiveSequencer()); }));

		FAutoConsoleCommand GState(TEXT("BlackEyeCustom.Setup.State"), TEXT("[Camera]: log the camera's switches and subjects."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				WithCamera(Args.IsValidIndex(0) ? Args[0] : FString(), [](const FCamera& Camera)
				{
					const FBlackEyeSetupState S = ReadState(Camera);
					FString Out = FString::Printf(TEXT("%s (%s): Follow %s [%s], LookAt %s [%s], Focus %s [%s]"), *S.CameraBindingName,
						S.bSpawnable ? TEXT("spawnable") : TEXT("level"), S.bFollow ? TEXT("on") : TEXT("off"), *Describe(S.FollowSubject),
						S.bLookAt ? TEXT("on") : TEXT("off"), *Describe(S.LookAtSubject), S.bFocus ? TEXT("on") : TEXT("off"), *Describe(S.FocusSubject));
					for (const FString& W : S.Warnings)
					{
						Out += TEXT("\nwarning: ") + W;
					}
					return Out;
				});
			}));

		FAutoConsoleCommand GSwitch(TEXT("BlackEyeCustom.Setup.Switch"), TEXT("<Follow|LookAt|Focus> <0|1> [Camera]: turn a switch on or off."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				EBlackEyeSetupSwitch Switch;
				if (Args.Num() < 2 || !ParseSwitch(Args[0], Switch))
				{
					LogResult(TEXT("error: usage BlackEyeCustom.Setup.Switch <Follow|LookAt|Focus> <0|1> [Camera]"));
					return;
				}
				const bool bOn = FCString::Atoi(*Args[1]) != 0;
				WithCamera(Args.IsValidIndex(2) ? Args[2] : FString(), [Switch, bOn](const FCamera& Camera) { return SetSwitch(Camera, Switch, bOn); });
			}));

		FAutoConsoleCommand GSwitchHere(TEXT("BlackEyeCustom.Setup.SwitchHere"),
			TEXT("<Follow|LookAt> <0|1> [Camera]: turn a switch on or off from the playhead on (keyed)."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				EBlackEyeSetupSwitch Switch;
				if (Args.Num() < 2 || !ParseSwitch(Args[0], Switch))
				{
					LogResult(TEXT("error: usage BlackEyeCustom.Setup.SwitchHere <Follow|LookAt> <0|1> [Camera]"));
					return;
				}
				const bool bOn = FCString::Atoi(*Args[1]) != 0;
				WithCamera(Args.IsValidIndex(2) ? Args[2] : FString(), [Switch, bOn](const FCamera& Camera) { return SetSwitch(Camera, Switch, bOn, true); });
			}));

		FAutoConsoleCommand GAllOff(TEXT("BlackEyeCustom.Setup.AllOff"), TEXT("[Camera]: Follow, LookAt and Focus off (no subjects needed): a plain CineCamera."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				WithCamera(Args.IsValidIndex(0) ? Args[0] : FString(), [](const FCamera& Camera) { return AllOff(Camera); });
			}));

		FAutoConsoleCommand GFill(TEXT("BlackEyeCustom.Setup.Fill"), TEXT("[Camera]: fill the subjects from the character selected in the level."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				WithCamera(Args.IsValidIndex(0) ? Args[0] : FString(), [](const FCamera& Camera) { return Autofill(Camera, SelectedActor()); });
			}));
	}

	void RegisterPanel()
	{
		ToolMenusStartup = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&ExtendMenus));
	}

	void UnregisterPanel()
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusStartup);
		UToolMenus::UnregisterOwner(MenuOwner);
		ShutdownUndo();
		if (const TSharedPtr<SWindow> Window = OpenWindow.Pin())
		{
			Window->RequestDestroyWindow();
		}
	}
}

#undef LOCTEXT_NAMESPACE
