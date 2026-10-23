// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0
//
// Black Eye Setup, shared between the logic (BlackEyeSetup.cpp) and the window (BlackEyeSetupPanel.cpp).

#pragma once

#include "CoreMinimal.h"
#include "BlackEyeSetupLibrary.h"

class AActor;
class ISequencer;
class ULevelSequence;
class UMovieScene;

namespace BlackEyeSetup
{
	/** A Black Eye camera bound in the sequence Sequencer has focused. */
	struct FCamera
	{
		TSharedPtr<ISequencer> Sequencer;
		ULevelSequence* Sequence = nullptr;
		UMovieScene* MovieScene = nullptr;
		FGuid Binding;
		FString Name;
		AActor* Live = nullptr;   // what Sequencer has bound now: the spawned copy, or the level actor
		AActor* Setup = nullptr;  // what is saved: the spawnable's template, or the level actor
		bool bSpawnable = false;
	};

	/**
	 * The camera to set up, in the sequence Sequencer has focused. Name empty: a selected Black Eye binding, else the
	 * one the camera cuts play, else the first (DESIGN §3). False with a reason when there is none or it isn't spawned.
	 */
	bool FindCamera(const TSharedPtr<ISequencer>& Sequencer, const FString& Name, FCamera& Out, FString& OutError);

	/** Every Black Eye camera binding name in the focused sequence, for the window's picker. */
	TArray<FString> ListCameras(const TSharedPtr<ISequencer>& Sequencer);

	FBlackEyeSetupState ReadState(const FCamera& Camera);

	/** Each one undo step; the returned text says what changed, one line each. */
	/** bFromPlayhead: keyed from the playhead on, the Transform section split there (DESIGN §5); else the whole shot. */
	FString SetSwitch(const FCamera& Camera, EBlackEyeSetupSwitch Switch, bool bOn, bool bFromPlayhead = false);
	/** Follow, LookAt and Focus off, whatever they read and with or without subjects: the camera as a plain CineCamera. */
	FString AllOff(const FCamera& Camera);
	FString SetSubject(const FCamera& Camera, EBlackEyeSetupSwitch Switch, const FBlackEyeSetupSubject& Subject);
	/** The subject reference keyed at the playhead; the subject before it stays. Same component and bone only. */
	FString SetSubjectFromPlayhead(const FCamera& Camera, EBlackEyeSetupSwitch Switch, const FBlackEyeSetupSubject& Subject);
	FString Autofill(const FCamera& Camera, AActor* Character);

	/** The subject a switch would take from one actor: a character's Heading socket / pelvis, tracker or head. */
	FBlackEyeSetupSubject SubjectFromActor(EBlackEyeSetupSwitch Switch, AActor* Actor, FString& OutNote);

	/** The Sequencer toolbar button and the window (BlackEyeSetupPanel.cpp). */
	void RegisterPanel();
	void UnregisterPanel();

	/** Drops the undo listener (module shutdown). */
	void ShutdownUndo();
}
