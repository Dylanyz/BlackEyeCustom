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
 * The guard: when a top-level transaction with the "PropertyEditor" session context starts while a widget holds the
 * cursor (a drag), remember it. Once that capture is gone (the drag is over, whatever ended it) and the same transaction
 * is still the open one, end it. A drag that closed its own transaction is never touched.
 *  - Only the property system's own value sets use that context (PropertyHandleImpl.cpp:557, SPropertyEditorNumeric.h:576),
 *    and on a working drag they close before the mouse-up returns. Transactions meant to stay open past a click, like
 *    the colour picker's (ColorStructCustomization.cpp:286), use the empty context and are never watched.
 *  - It is not limited to the Black Eye panel: a docked tab's widgets are not children of its SDockTab (the stack
 *    holds them, SDockingTabStack.cpp:181, :451), and Black Eye's tab ids differ by code path (Black_Eye_Editor.cpp:92,
 *    :585), so no widget or tab check finds the panel reliably. The same leak from any other property drag is closed too.
 *
 * BE-NATIVE: open the transaction in the spin box's OnBeginSliderMovement, set values with InteractiveChange |
 * NotTransactable while dragging, and close it in OnEndSliderMovement and on capture loss, as Epic's own numeric
 * property editor does (SPropertyEditorNumeric.h:569-598); then the handle swap can't strand it
 * (SHorizontalSSplitterSSpinbox.cpp:92-142, SHorizontalSSplitterSSpinBoxFVector.cpp:90-125).
 */

static TAutoConsoleVariable<bool> CVarSliderUndoGuard(
	TEXT("BlackEyeCustom.SliderUndoGuard"),
	true,
	TEXT("Close a property-editor undo transaction a slider drag left open, e.g. in the Black Eye panel (it locks Undo until a level reload)."));

namespace
{
	// The session context the property system opens its value-set transactions with (PropertyHandleImpl.cpp:557).
	const FString PropertyEditorContext(TEXT("PropertyEditor"));

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
			if (CVarSliderUndoGuard.GetValueOnGameThread() && Captor.IsValid() && Context.Context == PropertyEditorContext)
			{
				GWatch = FWatch{ Context.TransactionId, Captor, Context.Title.ToString() };
				UE_LOG(LogBlackEyeCustom, Verbose, TEXT("[BlackEyeCustom] slider undo guard: watching '%s' (drag by %s)"),
				       *GWatch->Title, *Captor->GetTypeAsString());
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
			       TEXT("[BlackEyeCustom] slider undo guard: closed '%s', left open by a slider drag (%d level%s)."),
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
