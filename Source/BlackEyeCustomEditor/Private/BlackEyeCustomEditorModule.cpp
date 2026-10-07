// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "BlackEyeContract.h"
#include "FastBake/BlackEyeFastBakeInternal.h"

/**
 * `BlackEyeCustom.SelfTest` in the console checks every Black Eye symbol this plugin reaches by name
 * (BlackEyeContract.h) and logs one line per check. Run it after every install and every Black Eye update.
 */
static FAutoConsoleCommand GBlackEyeCustomSelfTest(
	TEXT("BlackEyeCustom.SelfTest"),
	TEXT("Check that every Black Eye class, function and property this plugin relies on still resolves."),
	FConsoleCommandDelegate::CreateStatic([]() { BlackEyeContract::RunSelfTest(); }));

class FBlackEyeCustomEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		BlackEyeFastBake::RegisterMenus();
		BlackEyeFastBake::RegisterEditMenus();
	}

	virtual void ShutdownModule() override
	{
		// A record left running would leave a world-tick delegate pointing into the unloaded module.
		UBlackEyeFastBakeLibrary::StopRealtimeRecord(FString());
		BlackEyeFastBake::UnregisterEditMenus();
		BlackEyeFastBake::UnregisterMenus();
	}
};

IMPLEMENT_MODULE(FBlackEyeCustomEditorModule, BlackEyeCustomEditor);
