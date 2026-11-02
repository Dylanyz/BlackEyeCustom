<!-- Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
     SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0 -->
# Slider undo guard

Citations are pinned to Black Eye 2.0.7 and UE 5.8.2.

## 1. The bug (status: measured)

Dragging a spin box in the Black Eye panel (seen on LookAt **Subject Screen Position X**) can leave its undo
transaction open. Edit ▸ Undo then reads *"Can't undo while 'Edit X' is in progress"* and stays greyed out until the
level is reloaded. Undo History shows "Edit X" as the newest row with empty Transaction Details.

Measured 2026-11-02: the open count, read by calling `EndTransaction` until it returned 1, was **108** the first
time and **17** the second time in the same session. Each bad drag adds one level, because every later edit nests
inside the open one.

## 2. Why (status: from source, not caught live)

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

## 3. The guard (status: built 2026-11-02, not yet run)

`Private/SliderUndoGuard/BlackEyeSliderUndoGuard.cpp`:

- On `TransactionStarted` (top-level only, `TransBuffer.h:76`, `:134`), if the cursor captor is a widget inside the Black Eye
  panel (`BlackEyeContract::PanelWidgetType`, an ancestor walk), remember the transaction id and the captor.
- On each Slate post-tick, once that widget no longer holds the cursor: if the same transaction is still `GUndo` and
  open, call `GEditor->EndTransaction()` until it closes (at most 256 calls), then log it to `LogBlackEyeCustom`.
- A drag that closed its own transaction is never touched (`TransactionFinalized` clears the watch first).
- Off switch: `BlackEyeCustom.SliderUndoGuard 0`.

Rejected: a hotkey or console command that ends every open transaction. It would also close legitimate ones, and you
would still lose undo until you noticed.

## 4. Native fix for Black Eye (BE-NATIVE)

In the panel spin boxes, do what `SPropertyEditorNumeric` does: `GEditor->BeginTransaction` in `OnBeginSliderMovement`,
`InteractiveChange | NotTransactable` in `OnValueChanged`, a finished `SetValue` plus `EndTransaction` in
`OnEndSliderMovement`, and also end the transaction if the widget loses capture or is destroyed during a drag. Then
handle swaps no longer matter, and this guard goes quiet.

## 5. To verify

1. Install, then run `BlackEyeCustom.SelfTest`; check 8 must be `ok`.
2. Drag Subject Screen Position X in the panel repeatedly, including while selecting another camera. Edit ▸ Undo must
   stay available. Every intervention is logged as `slider undo guard: closed`.
3. Control: `BlackEyeCustom.SliderUndoGuard 0` should bring the lock back.
