// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

using UnrealBuildTool;

public class BlackEyeCustomEditor : ModuleRules
{
	public BlackEyeCustomEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Deliberately no "Black_Eye" here. Black Eye is reached by name through reflection
		// (Private/BlackEyeContract.cpp), so a Fab update never forces a rebuild of this plugin and it
		// still loads without Black Eye. docs/fast-bake/DESIGN.md "Linking Black_Eye" weighs this.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CinematicCamera",
				"Constraints",
				"ContentBrowser",
				"LevelSequence",
				"LevelSequenceEditor",
				"MovieScene",
				"MovieSceneTracks",
				"Projects",
				"Sequencer",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UniversalObjectLocator",
				"UnrealEd",
			}
		);
	}
}
