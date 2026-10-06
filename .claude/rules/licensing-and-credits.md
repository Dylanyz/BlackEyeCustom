# Licensing and credits: what is BlackEyeCustom's

The repo is **source-available: CPAL-1.0 + Commons Clause** (2026-10-06), with **one extra permission for Black
Eye Technologies**. `LICENSE` and `NOTICE` at the root are the authority. The rules shared by every plugin repo
(SPDX headers, no engine or vendor code, no machine paths, ask before any licence change) are in the plugin hub's
`refs/licensing.md`. This file holds only what is unique to this repo.

## The Black Eye Technologies permission

Dylan, 2026-10-06: *"for the devs, they can use my plugin freely to incorporate into black eye"*; *"credit is
welcome but not required"*; *"only blackeye team can use it in their paid plugin"*.

- It is set out in `LICENSE` under "ADDITIONAL PERMISSION FOR BLACK EYE TECHNOLOGIES", and pointed to from
  `NOTICE` and the README.
- Black Eye Technologies and its affiliates may build any of this into their products, paid ones included, under
  any terms they choose, with no copyleft, no attribution display and no selling ban. It is irrevocable and covers
  every version Dylan publishes.
- It is theirs alone. Everyone else, including a third party porting this work into Black Eye, is under
  CPAL + Commons Clause.
- Why it works: Dylan is the sole copyright holder, so he can grant extra rights beside the public licence.
  **If anyone else ever contributes code**, the permission only covers it if that contributor agrees to the same
  grant in writing. Ask Dylan before merging outside code.

## No Black Eye code, ever

The permission is only clean while the repo contains nothing of Black Eye's. A scan on 2026-10-06 compared every
line of 30+ characters in all history against the installed Black Eye 2.0.7 source: the only matches were engine
`#include`s and standard `Build.cs` idioms. Keep it so (rule 1 of `blackeye-handoff.md`).

## History

Commits before 2026-10-06 carry Apache-2.0 headers. They were never released, and `LICENSE` says so: the repo was
private and unshared until it was relicensed.
