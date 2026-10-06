# Writing for the Black Eye developers

Dylan, 2026-10-06: *"ill share everything with the devs, so it needs to be readable and usable for them, to quickly
understand the implementations, engineering, and design, so they can implement it in their own way in the main
plugin."* The repo **is** the handoff; there is no separate package. These rules keep it one.

1. **Cite Black Eye and engine source by `file:line`, never paste it.** Name the version the citation is pinned to
   (Black Eye 2.0.7, UE 5.8.2). Why: Black Eye is Fab code and engine code is under the UE EULA; neither may be
   redistributed (plugin hub `refs/licensing.md`).
2. **Every Black Eye name goes through `BlackEyeContract`** (`Source/BlackEyeCustomEditor/Public/BlackEyeContract.h`)
   with a matching `BlackEyeCustom.SelfTest` check. Why: the plugin doesn't link `Black_Eye`, so a rename only
   fails at runtime; one file and one self-test keep the whole dependency surface visible to us and to the devs.
3. **Every workaround gets `// BE-NATIVE: <what Black Eye would do natively> (<BE file:line>)`.** Why: grepping
   `BE-NATIVE` is the devs' port checklist (the plan, approved 2026-10-06).
4. **One `docs/<extension>/DESIGN.md` per extension, updated in the same commit as the code.** Sections carry a
   status (planned → built → measured); rejected ideas stay in with the reason. Why: the devs need the reasoning,
   not just the result, and a later agent would otherwise "simplify" a trap away.
5. **No film or project names, no machine paths, no project content in tracked files.** Test targets live in the
   film project's `.claude/refs/`. Why: strangers (the devs) read this repo.
