// SPDX-License-Identifier: Apache-2.0

#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "BlackEyeContract.h"

/**
 * `BlackEyeCustom.SelfTest` in the console checks every Black Eye symbol this plugin reaches by name
 * (BlackEyeContract.h) and logs one line per check. Run it after every install and every Black Eye update.
 */
static FAutoConsoleCommand GBlackEyeCustomSelfTest(
	TEXT("BlackEyeCustom.SelfTest"),
	TEXT("Check that every Black Eye class, function and property this plugin relies on still resolves."),
	FConsoleCommandDelegate::CreateStatic([]() { BlackEyeContract::RunSelfTest(); }));

IMPLEMENT_MODULE(FDefaultModuleImpl, BlackEyeCustomEditor);
