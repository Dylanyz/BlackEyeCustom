// SPDX-License-Identifier: Apache-2.0

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

	/** VersionName from Black_Eye.uplugin; empty when the plugin is not enabled. */
	BLACKEYECUSTOMEDITOR_API FString GetInstalledVersion();

	/** Logs one line per check. Returns true when every check passes. */
	BLACKEYECUSTOMEDITOR_API bool RunSelfTest();
}
