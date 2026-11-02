// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

#include "GeneralBugfixes/BlackEyeSliderUndoGuard.h"

#include "BlackEyeContract.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ITransaction.h"
#include "Widgets/SWidget.h"

/**
 * Why a Black Eye panel drag leaks its transaction (Black Eye 2.0.7, UE 5.8.2; DESIGN § Slider undo guard has the full trail):
 *  - The panel's spin boxes open no transaction of their own on drag start (SHorizontalSSplitterSSpinbox.cpp:92-109).
 *    The first interactive SetValue makes the property system open one, titled "Edit <field>" (PropertyHandleImpl.cpp:557),
 *    and only a later SetValue with bFinished through the same handle closes it, because whether a drag is in progress
 *    is held on that handle (bInteractiveChangeInProgress, PropertyHandleImpl.h:397; PropertyHandleImpl.cpp:545, :683).
 *  - The panel recreates every property handle when its row generator refreshes (SBlackEyeTab.cpp:1063-1072, :838).
 *    A refresh mid-drag sends the final SetValue through a fresh handle that never saw the drag, so nothing closes it.
 *  - A spin box that loses mouse capture mid-drag skips both its commit and OnEndSliderMovement (SSpinBox.cpp:349).
 * Each leak nests the next edit inside it, so the lock grows by one per bad drag (107 were stacked in one session).
 *
 * The guard: when a top-level transaction starts while the cursor is captured by a widget inside the Black Eye panel,
 * remember it. Once that capture is gone (the drag is over, whatever ended it) and the same transaction is still the
 * open one, end it. A drag that closed its own transaction is never touched.
 *
 * BE-NATIVE: open the transaction in the spin box's OnBeginSliderMovement, set values with InteractiveChange |
 * NotTransactable while dragging, and close it in OnEndSliderMovement and on capture loss, as Epic's own numeric
 * property editor does (SPropertyEditorNumeric.h:569-598); then the handle swap can't strand it
 * (SHorizontalSSplitterSSpinbox.cpp:92-142, SHorizontalSSplitterSSpinBoxFVector.cpp:90-125).
 */

static TAutoConsoleVariable<bool> CVarSliderUndoGuard(
	TEXT("BlackEyeCustom.SliderUndoGuard"),
	true,
	TEXT("Close an undo transaction a Black Eye panel slider drag left open (it locks Undo until a level reload)."));

namespace
{
	// More than this many nested opens on one leaked transaction means something else is going on: stop and say so.
	constexpr int32 MaxEndsPerLeak = 256;

	struct FWatch
	{
		FGuid TransactionId;
		TWeakPtr<SWidget> Captor;
		FString Title;
	};

	TOptional<FWatch> GWatch;
	FDelegateHandle GTransactionHandle;
	FDelegateHandle GPostTickHandle;
	FDelegateHandle GPostEngineInitHandle;
	TWeakObjectPtr<UTransBuffer> GBoundBuffer;

	TSharedPtr<SWidget> GetCursorCaptor()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetCursorUser();
		return User.IsValid() ? User->GetCursorCaptor() : nullptr;
	}

	bool IsInsideBlackEyePanel(const TSharedPtr<SWidget>& Widget)
	{
		int32 Depth = 0;
		for (TSharedPtr<SWidget> It = Widget; It.IsValid() && Depth < 128; It = It->GetParentWidget(), ++Depth)
		{
			if (It->GetTypeAsString() == BlackEyeContract::PanelWidgetType)
			{
				return true;
			}
		}
		return false;
	}

	bool IsWatchedStillOpen()
	{
		return GWatch.IsSet() && GEditor && GEditor->Trans && GEditor->Trans->IsActive() && GUndo
			&& GUndo->GetContext().TransactionId == GWatch->TransactionId;
	}

	void OnTransactionStateChanged(const FTransactionContext& Context, ETransactionStateEventType Event)
	{
		if (Event == ETransactionStateEventType::TransactionStarted)
		{
			// Started fires only for a top-level Begin (TransBuffer.h:76, :134), so this is never a nested one.
			const TSharedPtr<SWidget> Captor = GetCursorCaptor();
			if (CVarSliderUndoGuard.GetValueOnGameThread() && Captor.IsValid() && IsInsideBlackEyePanel(Captor))
			{
				GWatch = FWatch{ Context.TransactionId, Captor, Context.Title.ToString() };
			}
			else
			{
				GWatch.Reset();
			}
		}
		else if (GWatch.IsSet() && Context.TransactionId == GWatch->TransactionId
			&& (Event == ETransactionStateEventType::TransactionFinalized || Event == ETransactionStateEventType::TransactionCanceled))
		{
			GWatch.Reset();
		}
	}

	void OnPostTick(float)
	{
		if (!GWatch.IsSet())
		{
			return;
		}

		// Still dragging: the widget that started the transaction holds the cursor.
		const TSharedPtr<SWidget> Captor = GWatch->Captor.Pin();
		if (Captor.IsValid() && Captor == GetCursorCaptor())
		{
			return;
		}

		// The drag is over. Mouse-up commits run inside input handling, before this post-tick, so an open
		// transaction here was stranded.
		if (!IsWatchedStillOpen())
		{
			GWatch.Reset();
			return;
		}

		const FString Title = GWatch->Title;
		int32 Ends = 0;
		while (IsWatchedStillOpen() && Ends < MaxEndsPerLeak)
		{
			GEditor->EndTransaction();
			++Ends;
		}

		if (IsWatchedStillOpen())
		{
			UE_LOG(LogBlackEyeCustom, Warning,
			       TEXT("[BlackEyeCustom] slider undo guard: '%s' is still open after %d closes; left it alone. Set BlackEyeCustom.SliderUndoGuard 0 and report it."),
			       *Title, Ends);
		}
		else
		{
			UE_LOG(LogBlackEyeCustom, Log,
			       TEXT("[BlackEyeCustom] slider undo guard: closed '%s', left open by a Black Eye panel drag (%d level%s)."),
			       *Title, Ends, Ends == 1 ? TEXT("") : TEXT("s"));
		}
		GWatch.Reset();
	}

	void Bind()
	{
		UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (Buffer && !GTransactionHandle.IsValid())
		{
			GTransactionHandle = Buffer->OnTransactionStateChanged().AddStatic(&OnTransactionStateChanged);
			GBoundBuffer = Buffer;
		}
		if (FSlateApplication::IsInitialized() && !GPostTickHandle.IsValid())
		{
			GPostTickHandle = FSlateApplication::Get().OnPostTick().AddStatic(&OnPostTick);
		}
	}
}

void BlackEyeSliderUndoGuard::Register()
{
	// Default-phase editor modules load before GEditor exists (the .uplugin's LoadingPhase), so bind after engine init.
	if (GEditor)
	{
		Bind();
	}
	else
	{
		GPostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&Bind);
	}
}

void BlackEyeSliderUndoGuard::Unregister()
{
	FCoreDelegates::GetOnPostEngineInit().Remove(GPostEngineInitHandle);
	GPostEngineInitHandle.Reset();
	if (UTransBuffer* Buffer = GBoundBuffer.Get())
	{
		Buffer->OnTransactionStateChanged().Remove(GTransactionHandle);
	}
	GTransactionHandle.Reset();
	GBoundBuffer.Reset();
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnPostTick().Remove(GPostTickHandle);
	}
	GPostTickHandle.Reset();
	GWatch.Reset();
}
