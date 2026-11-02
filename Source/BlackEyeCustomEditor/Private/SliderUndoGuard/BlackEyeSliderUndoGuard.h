// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

#pragma once

/**
 * Slider undo guard (docs/slider-undo-guard/DESIGN.md).
 *
 * A drag on a spin box in the Black Eye panel can leave its "Edit <field>" undo transaction open for good, which
 * locks Undo/Redo ("Can't undo while 'Edit X' is in progress") until the level is reloaded. The guard watches each
 * transaction a Black Eye panel drag starts and, once the mouse has let go, closes it if Black Eye didn't.
 */
namespace BlackEyeSliderUndoGuard
{
	void Register();
	void Unregister();
}
