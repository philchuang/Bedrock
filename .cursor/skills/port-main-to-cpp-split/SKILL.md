---
name: port-main-to-cpp-split
description: >-
  Port features onto the cpp-split branch by refactoring them across the
  bedrock_rules, bedrock_read, and bedrock_host split. Features travel
  upstream main, then origin/main, local main, and local dev, and only then
  onto cpp-split. Use when the user asks to bring main or dev onto cpp-split,
  cherry-pick or rebase onto the split, or land an upstream feature there.
---

# Port main onto cpp-split

`cpp-split` is the long-lived fork. Upstream and this repo's `main` and `dev` keep the pre-split tree, where screen reading, the ore tables, and the window still share `Application`. A feature reaches `cpp-split` only along this path:

`upstream main` → `origin/main` → local `main` → local `dev` → local `cpp-split`

The first four branches are the same shape, so those hops are ordinary merges. The last hop is this skill: take what landed on local `dev` and move its behavior into the split. Do not merge `dev` or `main` into `cpp-split`, and do not rebase `cpp-split` onto either. Both replay `Application.cpp` over the split and put the moved functions back. A commit still only on `main` waits until it has been merged into local `dev`.

When running the rebase, first branch into `backup/cpp-split-{version}` and `cpp-split-rebase`. Perform the rebase in `cpp-split-rebase`. When the rebase is complete and successful, replace the existing `cpp-split` branch.

## What the split is

Three static libraries, plus the executable. `cmake/BedrockFence.cmake` fails configure when an include would merge them again. Read the source lists in `CMakeLists.txt` (`RULES_SOURCES`, `READ_SOURCES`, `HOST_SOURCES`, `SOURCES`) rather than a copy of them.

| Target | Holds | Must not include |
|---|---|---|
| `bedrock_rules` | Tables, advice, the book, the shops, what a reading means as data | OpenCV, Win32, `MinerGUI.h` |
| `bedrock_read` | Capture and the readers, including `ScanSession`, `RefineryGlance`, `DatarunnerEyes` | `MinerGUI.h`, any header in `BEDROCK_TABLE_HEADERS` |
| `bedrock_host` | Phone, plugins, files, feedback, the debug zip. Not the window | — |
| executable | `Application`, `MinerGUI`, and the developer tools | From `Application.h`, `Application.cpp`, and `MinerGUI`: a reader header whose text contains `cv::`. `SelfTest`, `DevTools`, `DevShots`, and `main` may include those headers |

A reader returns what the screen said. The rules decide what it means. The window draws that. `ScanSession` does not touch the window, and its header names no picture type and no window type. `RefineryGlance` returns a reading; the order book stays in `Application`. `DatarunnerEyes` turns a grabbed frame or a screenshot file into a shot; the grab stays `ScanSession`'s.

Seams already in the tree:

- `ScanSession::RockAsk` is what one rock look needs from the log and the window. `ScanSession::Event` is what comes back. `Application::applyRock` applies it.
- `RefineryAsk` / `RefineryLook` is the same cut for a refinery terminal.
- A HUD line is resolved to a laser or module in `LoadoutNames` (rules). The reader calls the function pointers in `LoadoutResolve` and does not include the hardware table.
- The composition list's rules live in `RockContents`. The picture read is `RockContentsRead`. `fiveQualities` lives with the rules, so `PanelFive` does not include `MiningHardware.h`.
- `Application` asks `scan_->gameView()` and `scan_->gameMinimised()` instead of including `ScreenCapture.h`.

## Port one feature

1. `git fetch origin`. On `cpp-split`, list what local `dev` has that this branch does not: `git log --oneline HEAD..dev`. Group commits that are one feature. Port one feature at a time.
2. Read the feature's diff against the tree it was written on (`git show`, or `git diff` across the group). Find each changed function.
3. On `cpp-split`, find where that function lives now. `git log -S` and a search of the source lists are enough. Trust the current tree over `main`'s file path.
4. Port the behavior into that home. Leave `main`'s copy of a moved function out of `Application`.
5. Add a new source file to the one list it belongs in. Configure then compiles it, and the fence walks that list.
6. Build. Configure must succeed, because that is when the fence runs.

A commit that only touches files still in the same shape (`MinerGUI`, a rules file the split did not cut) may cherry-pick cleanly. If the cherry-pick conflicts inside a moved function, abort it and port by hand. Resolving that conflict by taking `main`'s side puts the pre-split code back.

## Where a hunk goes

- **Inside a function that already moved** (`lookForRock`, the signature scan, aim, snapshot, recording, the loadout read, the contents read, the laser range, the panel spot): edit it in `ScanSession`. A new fact from the log or the window is a field on `RockAsk`, set at the call site in `Application`. A new fact for the window or for `rock_scans` is an `Event`, handled in `applyRock`.
- **Refinery terminal glance:** `RefineryGlance`. Writing the book, sharing, and UEX stay in `Application`.
- **Datarunner picture:** `DatarunnerEyes`. Cooldown, the commodity match, and the POST stay in `Application`.
- **New screen reader:** `bedrock_read`. It returns a struct and includes neither `MinerGUI.h` nor an ore-table header. Pass a table fact in on the ask (an int or a string), or return the raw text and resolve it in `bedrock_rules`.
- **What a figure means, a table, a shop, a log line:** `bedrock_rules`.
- **Files, plugins, feedback, zip, phone:** `bedrock_host`.
- **Drawing, a click, what the player is told:** `MinerGUI` and `Application`.

`bedrock_read` must not include `SignatureAnalyzer.h`. When a look needs the place's rock model, pass the small fact `Application` already computes (`RockAsk::rocksHere`, `gameGeneration`) rather than the analyzer.

## Fence failures

A fence error is an include in the wrong library. Move the include, or pass the fact across the seam. Do not weaken `BedrockFence.cmake` and do not drop the feature.

`Application.cpp` including `LoadoutReader.h` or `ScreenCapture.h` fails because those headers mention `cv::`. Call `panelsWanted` (declared in `LoadoutNames.h`) and `scan_->gameView()` / `scan_->gameMinimised()` instead.

## Done

From the repo root, in PowerShell:

```
cmake -S . -B build
cmake --build build --config Release --target StarCitizenMiner
```

The feature on local `dev` is the behavior to keep. After the build, say which commits were ported and which seam each hunk landed on. Leave the result uncommitted unless the user asked for a commit.
