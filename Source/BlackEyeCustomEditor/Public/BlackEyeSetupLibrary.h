// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlackEyeSetupLibrary.generated.h"

class AActor;
class ULevelSequence;

/** The three switches of Black Eye Setup (docs/blackeye-setup/DESIGN.md). */
UENUM(BlueprintType)
enum class EBlackEyeSetupSwitch : uint8
{
	Follow,
	LookAt,
	Focus,
};

/** One switch's subject: an actor, and optionally a component and a bone or socket on it. */
USTRUCT(BlueprintType)
struct FBlackEyeSetupSubject
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Eye Setup") TObjectPtr<AActor> Actor = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Eye Setup") FString Component;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Black Eye Setup") FString Bone;
};

/** A Black Eye camera's setup in the sequence Sequencer shows. */
USTRUCT(BlueprintType)
struct FBlackEyeSetupState
{
	GENERATED_BODY()

	/** False when no Black Eye camera could be found; Message says why. */
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") FString Message;
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") FString CameraBindingName;
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") bool bSpawnable = false;

	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") bool bFollow = false;
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") bool bLookAt = false;
	/** On: the lens tracks the focus subject. Off: manual focus distance. */
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") bool bFocus = false;

	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") FBlackEyeSetupSubject FollowSubject;
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") FBlackEyeSetupSubject LookAtSubject;
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") FBlackEyeSetupSubject FocusSubject;

	/** Keyed switch or subject tracks the setup leaves alone (their keys win over the setup), one line each. */
	UPROPERTY(BlueprintReadOnly, Category = "Black Eye Setup") TArray<FString> Warnings;
};

/**
 * Black Eye Setup: three independent switches (Follow, LookAt, Focus) for a Black Eye camera in a sequence, each of
 * which also sets the sequence's tracks for that camera so the camera keeps its place and nothing fights it. The
 * Sequencer toolbar's "Black Eye Setup" window calls these. All of them work on the sequence open and focused in
 * Sequencer (it is opened if it isn't) and are one undo step each.
 */
UCLASS()
class BLACKEYECUSTOMEDITOR_API UBlackEyeSetupLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The camera's switches and subjects. Empty CameraBindingName: the camera the window would pick (DESIGN §3). */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Setup")
	static FBlackEyeSetupState GetSetupState(ULevelSequence* Sequence, const FString& CameraBindingName);

	/** Turns one switch on or off and sets the tracks it needs. Returns what changed, one line each. */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Setup")
	static FString SetSetupSwitch(ULevelSequence* Sequence, const FString& CameraBindingName, EBlackEyeSetupSwitch Switch, bool bOn);

	/** Sets one switch's subject (and the reference tracks a spawnable camera needs for it). */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Setup")
	static FString SetSetupSubject(ULevelSequence* Sequence, const FString& CameraBindingName, EBlackEyeSetupSwitch Switch,
	                               const FBlackEyeSetupSubject& Subject);

	/**
	 * Fills all three subjects from one character: Follow its `Heading` socket (else its pelvis bone), LookAt and Focus
	 * its tracker (a tracker actor attached to it or resting at its head; else its head bone). Switches are unchanged.
	 */
	UFUNCTION(BlueprintCallable, Category = "Black Eye|Setup")
	static FString AutofillSetup(ULevelSequence* Sequence, const FString& CameraBindingName, AActor* Character);
};
