// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;

BLACKEYECUSTOMEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogBlackEyeCustom, Log, All);

/**
 * Every Black Eye symbol this plugin touches, in one place.
 *
 * This plugin does not link the Black_Eye module. It reaches Black Eye by name through reflection, so a
 * Fab update never forces a rebuild and the plugin still loads where Black Eye is absent. The cost is that
 * a renamed symbol fails at runtime instead of at compile time, so every name lives here and
 * `BlackEyeCustom.SelfTest` checks each one. If a Black Eye update breaks something, the self-test names it.
 *
 * Stepping a camera needs no symbol: ABlackEyeCineCameraActorBase overrides the virtual AActor::Tick, which
 * runs Follow then LookAt (BlackEyeCineCameraActorBase.cpp:124-150, Black Eye 2.0.7).
 *
 * BE-NATIVE: inside Black Eye this whole file disappears; each lookup becomes a direct call or member access.
 */
namespace BlackEyeContract
{
	/** The Black Eye release this plugin was written and tested against. Another version only warns. */
	inline constexpr const TCHAR* TestedVersion = TEXT("2.0.7");

	/** ABlackEyeCineCameraActorBase (BEC, Cross Camera, Shot List...), or null when Black Eye is not loaded. */
	BLACKEYECUSTOMEDITOR_API UClass* GetCameraBaseClass();

	/** True for any Black Eye camera actor. */
	BLACKEYECUSTOMEDITOR_API bool IsBlackEyeCamera(const AActor* Actor);

	/**
	 * Calls SnapComponentsToTargetsNow (a BlueprintCallable UFUNCTION) on a Black Eye camera. False if it isn't one.
	 * BE-NATIVE: a direct call (BlackEyeCineCameraActorBase.h:78).
	 */
	BLACKEYECUSTOMEDITOR_API bool SnapNow(AActor* Camera);

	/**
	 * Every actor named as a subject by the camera's Follow and LookAt components: the `Actor` of each
	 * FBlackEyeSimpleTarget-derived property (LookAt Target_0..11, Follow Target_0..5). Unset or unloaded ones are skipped.
	 * BE-NATIVE: the components already resolve these every tick (BlackEyeLookUtils.cpp:295-310).
	 */
	BLACKEYECUSTOMEDITOR_API void GetSubjectActors(const AActor* Camera, TArray<AActor*>& OutActors);

	/**
	 * World position of the first subject (Target_0) of the camera's LookAt (bLookAt) or Follow component: its bone or
	 * socket when one is named, else the actor. For checking that a bake saw the same subject pose as live playback.
	 * False when unset.
	 * BE-NATIVE: the look and follow states already resolve this point (BlackEyeLookUtils.cpp:295-310).
	 */
	BLACKEYECUSTOMEDITOR_API bool GetFirstSubjectPoint(const AActor* Camera, bool bLookAt, FVector& OutWorld);

	/** VersionName from Black_Eye.uplugin; empty when the plugin is not enabled. */
	BLACKEYECUSTOMEDITOR_API FString GetInstalledVersion();

	/** Logs one line per check. Returns true when every check passes. */
	BLACKEYECUSTOMEDITOR_API bool RunSelfTest();
}
