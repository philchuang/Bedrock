# Partial port to .NET

Bedrock's window moves to C#. The network calls and the game logic around them move to a separate C# library, and the window is not allowed to absorb that library. The screen readers and the capture loop stay in C++. This document is the plan for that cut. It is a high-level design, not an API spec.

The product stays what it is: an unzip-and-run companion for Windows and Linux, reading the game off the screen and `Game.log`, with no installer and no account. Official builds keep a reproducible native reader. The C# executable is a second artifact with its own publish hash.

## Decision

One process, three layers:

- **The reader** is a C++ library, `bedrock_read`. It owns Desktop Duplication, the HDR tone map, the X11 window grab, the glyph reader, the PP-OCRv4 line reader, and every other screen reader. It also owns the scan state machine that decides when those readers run.
- **The core** is a C# library, `Bedrock.Core`. It tails `Game.log`, applies the generated tables, talks to UEX and to Bedrock's own service, and hosts plugins. It has no UI types in it.
- **The window** is a C# executable, `Bedrock.App`. Avalonia draws it on Windows and on Linux. It binds to what the core already decided and sends the player's actions back. It does not parse, sort, or call the network itself.

The reader reports what the screen said: numbers and strings. The core decides what that means for the player. A signature of 6370 crosses into the core as an integer, and the ore tables there turn it into Stileron × 2. The window receives that result and draws it.

Avalonia is the UI toolkit because it runs on Windows and on Linux from one codebase. WPF and WinUI do not. The Linux capture path and the global hotkeys are not the UI toolkit's job. They stay beside it, as they do today.

## What this port does not do

- It does not rewrite the readers on OpenCvSharp or any other imaging library. The measured behaviour of the glyph reader, the locator, and the tone curve stays in the code that was measured.
- It does not split a reader in the middle of a file. `RefineryTerminal`, `CommodityTerminal`, `ScanPanel`, `LoadoutReader`, `LaserRange`, and `RockContents` stay whole. Each already ends in a struct. That struct is the boundary.
- It does not carry `cv::Mat`, `std::string`, or C++ objects across the boundary.
- It does not port the developer tools. Tesseract, the harvest commands, and the benches stay a C++ executable built from the same reader library. `BEDROCK_DEV_TOOLS` does not grow a C# half.

## The two parts

### Reader (`bedrock_read`)

Stays C++20, built by the existing CMake, linked as a DLL on Windows and a `.so` on Linux. OpenCV, zstd, DXGI, and X11 stay with it.

| Stays | Role |
|---|---|
| `DxgiCapture`, `ScreenCapture`, `LinuxCapture` | Grab a region. HDR scRGB is tone-mapped here. On Linux the picture is the game window, because the XWayland root comes back black. |
| `GlyphReader`, `TextLineReader`, `ScanPanel` | Find a signature or a scan panel and read it. |
| `RockContents`, `LoadoutReader`, `LaserRange` | Read the panel's composition, the ship's lasers, the range. |
| `RefineryTerminal`, `CommodityTerminal` | Read a terminal into the struct it already returns. |
| `FrameRecorder` | The optional ring of recent frames. |
| `OCREngine` | Developer builds only. |
| The scan half of `Application` | When to grab, which reader to run, the recent-read window, the "both readers already agreed" cache, marker tracking. |

The scan half is the part of `Application.cpp` that exists so the line reader is not paid for twice on the same pixels: `processScanWithGUI`, `lookForRock`, `trackCameraMotion`, `followLockedMarker`, and the loadout, contents, and refinery jobs that run off a grabbed frame. That state moves into the library with the readers. It is not UI state.

`LoadoutReader` today returns a `const MiningLaser*` into the generated hardware table. Across the boundary it returns the HUD line it read. Resolving that line to a laser moves to C# with `MiningHardware`.

Reader assets stay embedded in the library: `glyphs.bin`, `glyphs_mfd.bin`, `textline.onnx`, `textline_keys.txt`. UI pictures, fonts, and the tour images move to the window. `EmbeddedData.cpp` is split along that line.

### Core (`Bedrock.Core`)

A class library with no reference to Avalonia, and no window, control, brush, or bitmap type of its own. `HttpClient` for every request the C++ `service*` methods make today. `System.Text.Json` replaces `MiniJson`. One small interop type is the only C# that mentions the DLL. The rest of the core talks to an `IReader`.

| Moves here | Role |
|---|---|
| `Updater` | The update check, the download, and starting the script that replaces the build. WinHTTP goes away. |
| `UexPrices`, `UexMarket`, `UexReports`, `WhereToBuy`, `HaloRoute` | Prices, shops, reports, distance and shop order. |
| `RefineryShare`, `RockScans` | The player's own orders stay local. Sharing them is HTTP. |
| `RefineryOrders` | The book of work orders once a terminal has been read. |
| `GameState`, `GamePlaces`, `TradeBook`, `BlueprintLibrary`, `Reputation`, `GameTexts` | `Game.log` and the files written back into the game folder. |
| `NoteBook`, `DataBackup`, `GameFiles`, `AppPaths` | Local data, portable mode, settings as data. |
| `ColourScheme` | The scheme as data: which colour each role has. Turning that into paint is the window's job. |
| `Plugins`, `PluginHost` | The Lua host and the `bedrock` table. A plugin's `draw` list comes out as data. |
| `SignatureAnalyzer` and the generated tables | `CraftingData`, `MiningHardware`, `MiningLocations`, `CommodityData`, `RefineryData`, `RefiningData`, `GameTextMarks`, `NavPlaces`. |

`Datarunner` splits at the picture. `CommodityTerminal` reads the screenshot inside the reader and returns rows plus the crops. Cooldown, the UEX commodity match, and the `data_submit` POST live in the core. The window shows the crops and forwards Send. Nothing is sent until the player clicks Send, as today.

The core's public surface is plain models and methods: a rock, an order, a shop list, a command such as `ShareRefinery(bool)`. It does not accept a control, a click coordinate in a widget, or a framework dispatcher. The window translates a click into one of those commands.

A fake `IReader` returns canned readings. The core's tests use it, and so can the window. The real reader is a second implementation of the same interface, still inside the core.

The Python generators emit C# into this library. While both sides still compile, they emit the C++ headers too. Those headers are deleted when the last C++ consumer of that table is gone. Tables are not hand-ported and they are not maintained twice by hand.

### Window (`Bedrock.App`)

Avalonia, and a reference to `Bedrock.Core`. Nothing else about the game lives here.

| Lives here | Role |
|---|---|
| The replacement for `MinerGUI` | Rewritten, not translated. The GDI+ painting and the Win32-to-Qt shim go away with it. |
| `PhoneServer` | Serves pictures of the Avalonia window. The snapshot is taken here, because this is the process that holds the pixels. Which port, which token, and what a tap means are answered by the core. |
| View models | Bind a core model to controls and forward the player's action. A view model does not parse a log line, sort a shop list, or open an HTTP call. |

The rule is which project a new class is allowed to reference:

- The core does not reference `Bedrock.App` or Avalonia. A `using` of a UI namespace in the core is a broken build, enforced by the project reference going one way.
- The window does not reference `HttpClient`, the log parsers, or the generated tables except by calling the core. Shop order, signature lookup, and refinery arithmetic are not reimplemented in a view model.
- The window does not P/Invoke the reader. Cover rectangles and the aim box are methods on the core, which owns the interop type.

## The boundary

A C ABI, polled. The core calls `bedrock_poll` once a frame and receives plain events. The reader never calls back into C# during a grab, and the window never calls the reader at all. There is no `cv::Mat` and no `std::string` in the header.

```c
/* bedrock_read.h — stable ABI. Bump BEDROCK_READ_ABI when a field's meaning changes. */
#define BEDROCK_READ_ABI 1

typedef struct BedrockEvent {
    int kind;            /* signature, rock, contents, refinery, loadout, terminal, datarunner */
    int value;           /* the signature, when that is what was read */
    int confidence;      /* percent, or -1 when the reader has none */
    int x, y, w, h;      /* where on the screen, for the overlay */
    const char* json;    /* one JSON object for the heavier readings; null for a bare signature */
    int json_length;
} BedrockEvent;

int  bedrock_open(void);
void bedrock_close(void);
void bedrock_set_aim(int x, int y, int w, int h, int automatic);
void bedrock_set_live(int on);
void bedrock_set_track(int on);
/* Rectangles of Bedrock's own windows, virtual-screen coordinates.
   On Windows the grab is the desktop as shown, so these are blacked out
   of the picture. Pass count 0 when there is nothing to mask. */
void bedrock_set_covers(const int* xywh, int count);
/* Copy out the next event. Returns 1 when there was one, 0 when the queue is empty. */
int  bedrock_poll(BedrockEvent* out);
void bedrock_free(const char* json);

/* The same readers, on a picture the caller already has. Used by the
   self-test and by a Datarunner screenshot. bgra is tightly packed. */
int  bedrock_read_picture(const unsigned char* bgra, int width, int height, int stride,
                          int kind, BedrockEvent* out);
```

JSON is the encoding for anything heavier than a signature. A refinery screen is a list of names and times. A hand-written C struct for each of them drifts the first time a field is added. The scan loop does not build that JSON. It builds it once, when a reading is ready to show.

Three things cross as raw bytes, and only when they change or when a shot is taken:

- The aim box, live mode, and auto-track, written in when the player changes them.
- The list of Bedrock's own window rectangles, before each grab. The window measures them and the core passes them on. Missing one makes the reader treat the overlay as a signature. On Linux the picture is the game window, so this list is empty there.
- One BGRA copy of a Datarunner screenshot, plus each row's crop, a few times a visit rather than a few times a second.

Strings are UTF-8. The core releases them with `bedrock_free`. `bedrock_open` fails cleanly when duplication is unavailable. The core reports that, and the window shows the same fallback the C++ app shows today.

`BEDROCK_READ_ABI` is compiled into the reader and the core. The core refuses a DLL whose number it does not know.

## Threading

The reader runs its scan on its own thread. The core polls it, and the window reads the core's latest model. The pump inside today's scan, which exists so the title bar still drags during a 0.6–2.7 s read, goes away. The window was never on that thread.

Below-normal priority moves from the process to the scan thread. `SetPriorityClass(BELOW_NORMAL_PRIORITY_CLASS)` on the C# process would also deprioritize the window. The scan thread still yields to the game. The line reader still takes 50–60 ms a line, and longer with the game running. That cost stays where it is measured.

HTTP continues off the UI thread, as the `std::future` jobs do today. `await` replaces the `serviceUex` / `serviceRefineryShare` pollers.

## Linux

Avalonia draws the windows under X11 and under Wayland. Two pieces stay native, and both already have a Linux implementation:

- Capture stays in the reader. `LinuxCapture` finds the game window in the X tree and reads its Composite pixmap.
- Ctrl+Alt+S and Ctrl+Alt+K stay an `XGrabKey` on the X11 root, so the keys arrive while the game has focus. Proton is an XWayland client, and a grab from another X11 client reaches it. A native Wayland window does not. The core opens its own X11 display for the grab rather than waiting for Avalonia's event loop, because the window may be on Wayland. The grab is not a UI concern.

The check that has to be done on a machine, and cannot be settled here, is the overlay: a topmost transparent Avalonia window stays above a Proton game and takes clicks the way the Qt window does now.

Packaging stays an AppImage: the self-contained publish, `bedrock_read.so`, and the OpenCV libraries the reader already needs. The folder is larger than today's by the size of the .NET runtime.

## Plugins

The Lua host moves into the core and keeps the `bedrock` table in `Plugins.h`. Existing `main.lua` files and existing `.bplugin` signatures have to keep working. Ed25519 verification uses a standard library. Monocypher's signatures are ordinary Ed25519. The host returns a `draw` list and a note as data. The window paints that list and does not host Lua itself.

The sandbox is the part that has to be re-proved: no files, no processes, no network beyond what `bedrock` hands over, an 8 ms budget, and a fault that stops that plugin only. Wikelo and Blackbox are the acceptance test. The host lands when the window can paint a list the core hands it.

## How to perform it

Each step leaves a program that still builds, still passes `--selftest`, and can still be shipped. The reader is extracted before the window is rewritten, so a Star Citizen patch during the port lands in one reader rather than in two trees.

### 1. Library, same executable

Move the reader sources and the scan state machine behind a C++ class the existing `Application` calls. The class returns the structs the code already uses (`RockStats`, `RefineryScreen`, `SeenLoadout`, and the rest). `Application` keeps the window, the log, and HTTP.

Exit: the current executable, linked against the new library, passes `--selftest` and a live session behaves as before. No C# yet.

### 2. C ABI and the picture entry point

Add `bedrock_read.h`. The executable uses it, so the ABI is exercised by the real program and not only by a test harness. `bedrock_read_picture` takes a BGRA buffer and runs one reader. Point the picture-driven cases in `SelfTest.cpp` at it.

Exit: `--selftest` passes through the C ABI. A wrong signature or a wrong panel is still a C++ failure.

### 3. C# shell

Create `Bedrock.Core` and `Bedrock.App` with the reference pointing from the window to the core. The core loads the DLL and polls. The window shows the last signature and the last rock from a model the core published. The aim box and the cover rectangles are methods on the core. The window passes its own rectangles in, and it does not call `bedrock_set_aim` itself.

Exit: on Windows, a live scan shown in the C# window agrees with the C++ executable on the same session. The scan thread, not the process, is below normal priority. `Bedrock.Core` has no Avalonia reference.

### 4. Game logic, one area at a time

Port in this order. Each area brings the self-test cases that cover it, rewritten as C# tests, and the generator output for any table it needs.

1. Signature tables and `SignatureAnalyzer`. The shell shows an ore, not an integer.
2. `Game.log`: place, trades, blueprints, reputation. The place filter is applied in C# and does not re-enter the reader. The reader does not know which system the player is in.
3. UEX prices, shops, and `WhereToBuy`. Gear and the Ore Finder have data before they have a finished screen.
4. Refinery orders and refinery share. The reader delivers a `RefineryScreen`. The book and the upload are C#.
5. Rock-scan sharing, the updater, notes, backups, and paths (`portable.txt`, `%LOCALAPPDATA%\Bedrock`, `~/.local/share/bedrock`).

Exit for each: its old self-test cases pass as tests of `Bedrock.Core`, with no Avalonia project loaded, and the C++ executable still passes the cases that have not moved yet.

### 5. Windows, screen by screen

Rewrite the windows against the core's models. Where a screen needs a live reading before the DLL is wired, the core is given the fake `IReader`. The window never takes a second path to the DLL. Order:

1. Main window and the overlay. This is the session a player lives in.
2. Ore Finder, including the Aaron Halo calculator (`HaloRoute`).
3. Gear and shops.
4. Refinery.
5. Blueprints.
6. Datarunner, including the picture beside each row.
7. Settings and the colour schemes.
8. Phone page. It photographs the Avalonia window, so it waits until those windows look like the product.

The C++ executable remains the shipped build until the C# app has been used through a real mining run: a signature, a rock, a refinery visit, and a shop list.

### 6. Plugins, phone, and update

Port the Lua host. Run Wikelo and Blackbox against a recorded `Game.log` and against the game. Turn the phone server on and drive the overlay from a browser. Walk the update check against a test manifest, not the official one.

### 7. Linux

Build `bedrock_read.so` from the same CMake the Windows DLL uses. Run the Avalonia app, confirm the X11 grab and the two hotkeys under Proton, and confirm the overlay. Pack the AppImage. `QT_QPA_PLATFORM` and the Qt dependency are gone once `linux_app.cpp` is gone.

### 8. Remove the C++ UI

Delete `MinerGUI.cpp`, `MinerGUI.h`, `src/linux/Win32Shim.cpp`, `Win32Controls.cpp`, `GdiplusShim.h`, `include/win32/`, and the Qt find in `CMakeLists.txt`. Delete the HTTP clients and the log parsers from the C++ tree as their C# replacements become the ones the self-test runs. The developer executable and `bedrock_read` remain.

## Tests

`SelfTest.cpp` is about 8,000 lines and covers both sides. It splits with the code.

- Picture tests stay C++ and call `bedrock_read_picture`. They remain the gate for a reader change. The labelled frames in `tests/` do not move.
- Parser, table, shop-order, and log tests move into `Bedrock.Core`'s test project. They run without the DLL and without Avalonia. A test that needs a window to make an assertion about a shop list or a log line is in the wrong project.
- One joining check, on a machine with a screen, runs a live grab through the core and compares it with `bedrock_read_picture` on a snapshot of that same grab. The window is not part of that check.

A reader change is not done when the C# tests are green. It is done when the C++ picture tests are green.

## Packaging and updates

The Windows folder is the self-contained publish with `bedrock_read.dll` and the OpenCV DLLs beside it. The Linux folder is the AppImage described above. `uex_token.dat` stays a file beside the executable, mixed the way `UexReports::unmixAppToken` already reverses, so the token is still not inside the program.

The reader's hash can stay a reproducible MSVC `/Brepro` or GCC prefix-map build. The C# publish is pinned to an SDK and recorded in `BUILDINFO.txt` as its own hash. The two hashes are not one hash, and the release notes should say so.

The update manifest gains a second file, the reader library, and the apply step replaces both. An update that shipped only one of them would mix an ABI the core has already refused to load.

## Risks worth remembering

- **The cut is `Application.cpp`.** About 6,500 lines refer to both the scan and the window. Step 1 is the dangerous step. Do it while the UI is still the C++ UI, so a regression is a C++ bug.
- **Cover rectangles.** The Windows grab sees the overlay. The list passed to `bedrock_set_covers` has to include every Bedrock window, including the overlay, on every grab.
- **Priority.** Lower the scan thread, not the process.
- **Plugins.** The sandbox will not match until Wikelo and Blackbox have been run. A Lua fault must not take the process down.
- **The overlay on Linux** is the one behaviour with no proof yet under Avalonia. Step 7 exists to get that proof before the Qt shim is deleted.
- **A game patch during the port** touches the reader once, because there is one reader. It touches the C# tables only when the generators have learned to emit them. Until step 4 has emitted a given table, that table's patch still lands in the C++ header, and the generator change for it happens in the same patch.
- **Logic in a view model.** Shop sorting, log parsing, and HTTP are easy to write at the click that needs them. They belong in the core. A view model that grows a parser is the separation failing.
