<!-- Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
     SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0 -->
# General Bugfixes

Small fixes for Black Eye bugs that don't belong to Bake or Setup. Each one is a guard that only acts on the bug's
exact signature, so it goes quiet once Black Eye fixes the bug. Each fix gets one section below with the same shape:
the bug, why it happens, the guard, the native fix, how to verify. Citations are pinned to Black Eye 2.0.7 and UE 5.8.2.

| Fix | Status | Off switch |
|---|---|---|
| [Slider undo guard](#slider-undo-guard) | v2 measured 2026-11-02: works (v1 missed the leak) | `BlackEyeCustom.SliderUndoGuard 0` |

## Slider undo guard

### 1. The bug (status: measured)

Dragging a spin box in the Black Eye panel (seen on LookAt **Subject Screen Position X**) can leave its undo
transaction open. Edit ▸ Undo then reads *"Can't undo while 'Edit X' is in progress"* and stays greyed out until the
level is reloaded. Undo History shows "Edit X" as the newest row with empty Transaction Details.

Measured 2026-11-02: the open count, read by calling `EndTransaction` until it returned 1, was **108** the first
time and **17** the second time in the same session. Each bad drag adds one level, because every later edit nests
inside the open one.

### 2. Why (status: from source, not caught live)

1. The panel's spin boxes (`SHorizontalSSplitterSSpinbox.cpp:92-142`, `SHorizontalSSplitterSSpinBoxFVector.cpp:90-125`)
   open no transaction on drag start. The first `SetValue(…, InteractiveChange)` makes the property system open one
   titled "Edit <field>" (`PropertyHandleImpl.cpp:557`). Only a later finished `SetValue` closes it (`:683`), and only
   when that handle has a drag in progress (`bInteractiveChangeInProgress`, `PropertyHandleImpl.h:397`, read at `:545`)
   or the value text differs.
2. The panel recreates all of its property handles when its row generator refreshes (`SBlackEyeTab.cpp:1063-1072`,
   `CreatePropertyHandle` at `:838`; proxies re-pointed at `:974`, `:1015`). A refresh during a drag sends the final
   `SetValue` through a new handle that never saw the drag. The value is already equal, so nothing closes the transaction.
3. A spin box that loses mouse capture during a drag runs neither its commit nor `OnEndSliderMovement`
   (`SSpinBox.cpp:349`), so the transaction stays open there as well.

Epic's own numeric editor avoids both: it opens the transaction itself in `OnBeginSliderMovement`, sets values with
`InteractiveChange | NotTransactable` while dragging, and closes it in `OnEndSliderMovement`
(`SPropertyEditorNumeric.h:569-598`).

### 3. The guard (status: v2 measured 2026-11-02; v1 missed the leak)

`Private/GeneralBugfixes/BlackEyeSliderUndoGuard.cpp`:

- On `TransactionStarted` (top-level only, `TransBuffer.h:76`, `:134`), when the session context is `"PropertyEditor"`
  and a widget holds the cursor (a drag), remember the transaction id and that widget.
- On each Slate post-tick, once that widget no longer holds the cursor: if the same transaction is still `GUndo` and
  open, call `GEditor->EndTransaction()` until it closes (at most 256 calls), then log it to `LogBlackEyeCustom`.
- A drag that closed its own transaction is never touched (`TransactionFinalized` clears the watch first).
- Off switch: `BlackEyeCustom.SliderUndoGuard 0`. `log LogBlackEyeCustom Verbose` logs every drag it watches.

Why the `"PropertyEditor"` context is the signature: only the property system's own value sets open transactions with
it (`PropertyHandleImpl.cpp:557`, `SPropertyEditorNumeric.h:576`), and on a working drag they close before the
mouse-up returns. Transactions that are meant to stay open past a click, such as the colour picker's
(`ColorStructCustomization.cpp:286`) or the vector sliders' (`MathStructCustomizations.cpp:747`), use the empty
context, so they are never watched. The same leak from any other property drag is closed too.

**v2 measured 2026-11-02** on a production Black Eye camera, dragging Subject Screen Position X in the panel: the log
shows `watching 'Edit X' (drag by SSpinBox<NumericType>)`, then `closed 'Edit X', left open by a slider drag (6 levels)`
a second later, and Undo stayed available. One drag had left the transaction **6 levels** deep. That fits §2.2: each
handle the panel recreates mid-drag starts with no drag in progress, so its next interactive `SetValue` opens one more
nested level.

**v1 missed the leak (measured 2026-11-02).** It only watched drags inside the Black Eye panel and found the panel by
walking up from the slider to `SBlackEyeTab`. A docked tab's widgets are not children of its `SDockTab`, though: the
stack holds them (`SDockingTabStack.cpp:181`, `:451`). The walk never matched, and the guard logged nothing during a
drag that locked Undo. Black Eye's tab ids also differ by code path (`Black_Eye_Editor.cpp:92` vs `:585`), so a tab
lookup would be fragile as well.

Rejected: a hotkey or console command that ends every open transaction. It would also close legitimate ones, and you
would still lose undo until you noticed.

### 4. Native fix for Black Eye (BE-NATIVE)

In the panel spin boxes, do what `SPropertyEditorNumeric` does: `GEditor->BeginTransaction` in `OnBeginSliderMovement`,
`InteractiveChange | NotTransactable` in `OnValueChanged`, a finished `SetValue` plus `EndTransaction` in
`OnEndSliderMovement`, and also end the transaction if the widget loses capture or is destroyed during a drag. Then
handle swaps no longer matter, and this guard goes quiet.

### 5. To verify

1. Install, then run `BlackEyeCustom.SelfTest`, and `log LogBlackEyeCustom Verbose`.
2. Drag Subject Screen Position X in the panel repeatedly, including while selecting another camera. Edit ▸ Undo must
   stay available. Each drag logs `watching`, and each fix logs `slider undo guard: closed`.
3. Control: `BlackEyeCustom.SliderUndoGuard 0` should bring the lock back.
