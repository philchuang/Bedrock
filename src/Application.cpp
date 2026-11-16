#include "Application.h"
#include "DevSwitch.h"
#include "LoadoutNames.h"
#include "MiningHardware.h"
#include "GamePlaces.h"
#include "GameFiles.h"
#include <cctype>

#include "AppPaths.h"
#include "CraftingData.h"
#include "DebugZip.h"
#include "MiniJson.h"
#include "RefineryData.h"
#include "RefiningData.h"
#include "resource.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <chrono>
#include <thread>
#include <conio.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>

namespace SCMiner {

// Bedrock's UEX app token, read from beside the program (see below)
static std::string uexToken();

namespace {

const char* const REGION_CONFIG_FILE = "region_config.txt";
const char* const INSTALL_ID_FILE = "install_id.txt";

// Everything the app writes goes under AppPaths::data() - %LOCALAPPDATA%\Bedrock,
// or beside the exe when portable.txt says so. Never simply beside the exe:
// a program folder gets replaced or deleted on an update, and the player's
// library, notes and settings must not be in the way when it does.
std::string dataPath(const std::string& name) {
    return (AppPaths::data() / name).string();
}
// debug/snapshot_* files, oldest making room - the number is snapshotKeep_,
// snapshots= in region_config.txt, ten unless said otherwise

} // namespace

// Static instance for the hotkey callback
static Application* g_appInstance = nullptr;

// Overlay window procedure - draggable and resizable
LRESULT CALLBACK Application::OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rect;
            GetClientRect(hwnd, &rect);

            // Fill the background with the color key (turns transparent)
            HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);

            // Cyan frame, 2px (matches the app theme, easy to grab)
            HPEN framePen = CreatePen(PS_SOLID, 2, RGB(56, 211, 255));
            HPEN oldPen = (HPEN)SelectObject(hdc, framePen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

            Rectangle(hdc, 0, 0, rect.right, rect.bottom);

            // Label only when the region is large enough that the text does
            // not cover the content being aligned underneath
            if (rect.right >= 160 && rect.bottom >= 60) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(56, 211, 255));
                HFONT font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(hdc, font);
                TextOutA(hdc, 6, 4, "SCAN REGION", 11);
                SelectObject(hdc, oldFont);
                DeleteObject(font);
            }

            // Resize handle marker (bottom right)
            HBRUSH cornerBrush = CreateSolidBrush(RGB(255, 166, 87));
            const int cs = 10;
            RECT corner = {rect.right - cs, rect.bottom - cs, rect.right, rect.bottom};
            FillRect(hdc, &corner, cornerBrush);
            DeleteObject(cornerBrush);

            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(framePen);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            // Enables dragging and resizing
            POINT pt = {LOWORD(lParam), HIWORD(lParam)};
            ScreenToClient(hwnd, &pt);

            RECT rect;
            GetClientRect(hwnd, &rect);

            const int borderSize = 10;

            // Bottom right = resize both
            if (pt.x >= rect.right - borderSize && pt.y >= rect.bottom - borderSize) {
                return HTBOTTOMRIGHT;
            }
            // Right = resize width
            if (pt.x >= rect.right - borderSize) {
                return HTRIGHT;
            }
            // Bottom = resize height
            if (pt.y >= rect.bottom - borderSize) {
                return HTBOTTOM;
            }
            // Anywhere else = drag
            return HTCAPTION;
        }

        case WM_MOVE:
        case WM_SIZE: {
            // Report position/size back to the application
            if (g_appInstance) {
                RECT rect;
                GetWindowRect(hwnd, &rect);
                g_appInstance->onOverlayMoved(rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// Whether the second reader has already looked at this row and found no
// signature in it, lately. The HUD sways a pixel or two and the row is cut
// a little differently every scan, so "the same row" is one about the same
// place and about the same width.


void Application::onOverlayMoved(int x, int y, int w, int h) {
    scan_->moveAim(x, y, w, h);
    if (gui_) gui_->setRegionValues(x, y, w, h);
    saveRegionConfig();
}

void Application::createOverlayWindow(HINSTANCE hInstance) {
    // Register the window class
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "SCMinerOverlay";
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    RegisterClassExA(&wc);

    // Create the window - WITHOUT WS_EX_TRANSPARENT so mouse clicks work
    overlayHwnd_ = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "SCMinerOverlay",
        "Overlay",
        WS_POPUP,
        scan_->aim().x, scan_->aim().y,
        scan_->aim().width, scan_->aim().height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (overlayHwnd_) {
        // Color-key transparency - black becomes see-through
        SetLayeredWindowAttributes(overlayHwnd_, RGB(0, 0, 0), 0, LWA_COLORKEY);
        ShowWindow(overlayHwnd_, SW_SHOW);
    }
}

void Application::updateOverlayPosition() {
    if (overlayHwnd_) {
        const auto aim = scan_->aim();
        SetWindowPos(overlayHwnd_, HWND_TOPMOST,
            aim.x, aim.y,
            aim.width, aim.height,
            SWP_SHOWWINDOW);
        InvalidateRect(overlayHwnd_, nullptr, TRUE);
    }
}

void Application::destroyOverlay() {
    if (overlayHwnd_) {
        DestroyWindow(overlayHwnd_);
        overlayHwnd_ = nullptr;
    }
}

Application::Application() {
    g_appInstance = this;
    scan_ = std::make_unique<ScanSession>();
}

Application::~Application() {
    stop();
    rockScans_.gone();
    keepRockScans(true);
    destroyOverlay();
    g_appInstance = nullptr;
}

bool Application::initialize() {
    installLoadoutResolve();
    // Load the saved capture region first
    loadRegionConfig();
    scan_->setDebugImages(recordScans_);
    scan_->setThreads(scanThreads_);
    refreshAutoAim(true);
    scan_->refreshHudScale();

    // The blueprint library: what was ticked and read so far, then the
    // game's log - its backups are read one per tick from here on
    library_.load();
    library_.setGameLog(locateGameLog());

    // Ask once, in the background, whether there is a newer version.
    // Nothing is downloaded by asking and nothing is installed without
    // being asked; update=0 in region_config.txt stops it entirely.
    updater_.configure(updateUrl_.empty() ? BEDROCK_UPDATE_URL : updateUrl_, updateCheck_);
    // ...with the install's own number, so that installs are counted rather
    // than addresses - unless stats=0 (see Updater::installId). The refinery
    // workloads carry it too: the service holds back an install that sends
    // too many readings that do not fit.
    installId_ = statsWanted_ ? Updater::installId(AppPaths::file(INSTALL_ID_FILE)) : std::string();
    rockSession_ = RockScans::newSession();
    updater_.setInstallId(installId_);
    updater_.checkInBackground();
    lastUpdateCheck_ = std::chrono::steady_clock::now();

    std::cout << "=========================================" << std::endl;
    std::cout << "  BEDROCK" << std::endl;
    std::cout << "  Mining Companion for Star Citizen" << std::endl;
    std::cout << "=========================================\n" << std::endl;

#ifdef BEDROCK_TESSERACT
    std::cout << "Initializing Tesseract OCR..." << std::endl;
    std::cout << "tessdata path: ./tessdata" << std::endl;
#endif

    // Digit templates for the OCR-free reader, harvested from real captures
    const int glyphTemplates = scan_->loadGlyphs("./glyphs");
    std::cout << "Loaded " << glyphTemplates << " glyph templates." << std::endl;

    // Tesseract is opt-in (SCMINER_OCR=1) and its data is not shipped; without
    // it the app simply has no OCR, which is the normal state
    const bool ocrOn = scan_->initOcr();
#ifdef BEDROCK_TESSERACT
    if (!ocrOn) std::cout << "OCR: no tessdata beside the exe - off (the reader does not need it)" << std::endl;
#endif
    std::cout << (scan_->embeddedTemplates() ? "Templates: built in" : "Templates: from ./glyphs") << std::endl;

    // The second reader. Without it the scan runs on the glyph reader alone;
    // SCMINER_NO_TEXTLINE=1 asks for that on purpose.
    if (!DEV_ENV("SCMINER_NO_TEXTLINE")) {
        const auto lineStart = std::chrono::steady_clock::now();
        const bool lines = scan_->loadLines();
        std::cout << "Second reader: " << (lines ? "ready" : "could not be loaded - the glyph reader reads alone")
                  << " (" << static_cast<int>(std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - lineStart).count())
                  << " ms)" << std::endl;
    }

    analyzer_.setSystemFilter(currentSystem_);
    writeSessionInfo();

    std::cout << "\nControls:" << std::endl;
    std::cout << "  L        - toggle live mode" << std::endl;
    std::cout << "  S / F9   - single scan" << std::endl;
    std::cout << "  M        - manual signature input (console)" << std::endl;
    std::cout << "  R        - re-detect the capture region" << std::endl;
    std::cout << "  +/-      - adjust the scan interval" << std::endl;
    std::cout << "  Q/ESC    - quit" << std::endl;
    std::cout << "  Ctrl+Alt+S (anywhere) - snapshot of the game picture to debug/" << std::endl;
    std::cout << "\n=========================================\n" << std::endl;

    return true;
}

void Application::run() {
    running_ = true;

    std::cout << "Press 'L' for live mode or 'M' for manual input...\n" << std::endl;

    lastScanTime_ = std::chrono::steady_clock::now();

    while (running_) {
        // Poll keyboard input
        if (_kbhit()) {
            int key = _getch();

            switch (key) {
                case 'l':
                case 'L':
                    toggleLiveMode();
                    break;

                case 'm':
                case 'M': {
                    // Pause live mode for manual input
                    bool wasLive = liveModeActive_;
                    liveModeActive_ = false;

                    std::cout << "\nEnter signature value: ";
                    int manualValue = 0;
                    if (!(std::cin >> manualValue)) {
                        std::cin.clear();
                        std::cin.ignore(1024, '\n');
                        manualValue = 0;
                    }

                    if (manualValue > 0) {
                        std::string report = analyzer_.generateReport(manualValue);
                        std::cout << report << std::endl;
                    }

                    // Resume live mode if it was active
                    if (wasLive) {
                        liveModeActive_ = true;
                        std::cout << "\n[LIVE] mode resumed..." << std::endl;
                    }
                    break;
                }

                case 's':
                case 'S':
                    triggerScan();
                    break;

                case 'r':
                case 'R':
                    regionDetected_ = false;
                    lastRecognizedSignature_ = 0;
                    std::cout << "\nThe region will be re-detected on the next scan..." << std::endl;
                    break;

                case '+':
                case '=':
                    captureIntervalMs_ = (std::max)(100, captureIntervalMs_ - 100);
                    std::cout << "\nScan interval: " << captureIntervalMs_ << "ms" << std::endl;
                    break;

                case '-':
                case '_':
                    captureIntervalMs_ = (std::min)(3000, captureIntervalMs_ + 100);
                    std::cout << "\nScan interval: " << captureIntervalMs_ << "ms" << std::endl;
                    break;

                case 'q':
                case 'Q':
                case 27:  // ESC
                    running_ = false;
                    break;

                case 0:
                case 224: {
                    // Function keys (extended key code)
                    int fKey = _getch();
                    if (fKey == 67) {  // F9
                        triggerScan();
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // Live mode: automatic scan at a fixed interval
        if (liveModeActive_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScanTime_).count();

            if (elapsed >= captureIntervalMs_) {
                lastScanTime_ = now;
                if (gameOnScreen()) processScanLive();
            }
        }

        // Manual scan if requested
        if (scanRequested_) {
            scanRequested_ = false;
            if (gameOnScreen()) processScan();
        }

        // Short sleep to spare the CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\nApplication closed." << std::endl;
}

void Application::runWithGUI(HINSTANCE hInstance) {
    running_ = true;

    // Below normal priority: a reading waits for a core the game leaves free
    // rather than taking one from it - the second reader keeps several busy
    // for most of a second, enough to make the game stutter. The scan is light
    // and gets what it needs; on a full machine it only comes a moment later.
#ifdef _WIN32
    if (!DEV_ENV("SCMINER_NORMAL_PRIORITY")) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif

    // Create the GUI
    gui_ = std::make_unique<MinerGUI>();
    if (!gui_->initialize(hInstance)) {
        std::cerr << "Failed to create the GUI!" << std::endl;
        MessageBoxA(nullptr, "Failed to create the GUI!", "Bedrock", MB_ICONERROR);
        return;
    }

    // The capture region overlay, only when asked for (overlay=1); every
    // use of overlayHwnd_ below is guarded, so without it nothing shows
    if (overlayEnabled_) createOverlayWindow(hInstance);

    // System selection callback
    gui_->setSystemChangedCallback([this](const std::string& system) {
        currentSystem_ = system;
        analyzer_.setSystemFilter(system);
        // The wider choice, made on purpose: the place filter steps back
        // until the log names a new place (see applyGameState)
        if (!analyzer_.placeFilter().empty()) {
            analyzer_.setPlaceFilter("");
            if (gui_) gui_->setPlace(placeLine_, "");
        }
        std::cout << "System switched to: " << system << std::endl;

        // Re-run the analysis for the current signature with the new filter
        if (lastRecognizedSignature_ > 0) {
            auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
            gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
        }
    });

    // Live mode callback
    gui_->setPlaceOverrideCallback([this](const std::string& place) { setPlaceOverride(place); });
    // The Blueprints window's box that puts the marks into the game's texts,
    // and the one beside it for the names
    gui_->setGameTexts(gameTextsWanted_, gameTextsNote_);
    gui_->setGameNames(gameNamesWanted_);
    // Reporting to UEX: the player's key, where they gave one, and what the
    // window says of it
    uexKey_ = UexKey::load();
    gui_->setUexAccountCallback([this](int action, const std::string& text) { uexAccountAction(action, text); });
    gui_->setDatarunnerCallback([this](int action, int shot, int row, int value) {
        datarunnerAction(action, shot, row, value);
    });
    pushUexAccount();
    gui_->setGameTextsCallback([this](bool on) { setGameTextsWanted(on); });
    gui_->setGameNamesCallback([this](bool on) { setGameNamesWanted(on); });
    // The Refinery window's box that shares refinery workloads
    gui_->setRefineryShare(refineryShare_, !refineryServiceUrl().empty());
    gui_->setRefineryShareCallback([this](bool on) { setRefineryShare(on); });
    // ...and the Settings window's box that shares rock scans
    gui_->setRockShare(rockShare_, !refineryServiceUrl().empty());
    gui_->setRockShareCallback([this](bool on) { setRockShare(on); });
    // ...and the Feedback page, in the rail under the same condition: what
    // Send does, and what the page says goes along
    gui_->setFeedbackCallback([this](const std::string& text, const std::string& name, bool debugFolder) {
        sendFeedback(text, name, debugFolder);
    });
    serviceFeedback();
    loadPlaceOverride();
    gui_->setLiveModeCallback([this](bool active) { applyLiveMode(active); });
    // The update strip: one click fetches, the next installs. Nothing
    // happens without one, and the app only quits once the script that
    // replaces it has actually started.
    gui_->setUpdateActionCallback([this] {
        switch (updater_.state()) {
            case Updater::State::Available:
                updater_.downloadInBackground();
                break;
            case Updater::State::Ready:
                if (updater_.apply() && gui_) PostMessage(gui_->windowHandle(), WM_CLOSE, 0, 0);
                break;
            case Updater::State::Downloading:
            case Updater::State::Checking:
                break;      // already busy; let it finish
            default:
                updater_.checkInBackground();   // ask again, now
                break;
        }
    });

    // Manual signature input callback
    gui_->setManualAnalyzeCallback([this](int signature) {
        std::cout << "Manual signature: " << signature << std::endl;
        lastRecognizedSignature_ = signature;
        auto results = analyzer_.analyzeSignature(signature);
        gui_->updateResults(signature, results, -1.0, analyzer_.outOfPlace());
        displayResultsCompact(results, signature);
    });

    // Single scan callback (SCAN button / S key)
    gui_->setScanRequestedCallback([this]() {
        triggerScan();
    });

    // Auto-tracking toggle
    gui_->setTrackModeCallback([this](bool enabled) {
        setAutoTrack(enabled);
    });

    // The zoom, if this machine has been told one before
    if (uiScalePercent_ > 0) gui_->setUiScale(uiScalePercent_);
    gui_->setUiScaleCallback([this](int percent) {
        uiScalePercent_ = percent;
        saveRegionConfig();
    });

    // A rank picked in the Grind Planner, or the logs' word taken again
    gui_->setRankSetCallback([this](int faction, int ladder, int rank) {
        library_.setRank(faction, ladder, rank);
        pushRanks();
    });

    // A blueprint ticked or unticked by hand in the Blueprints window
    gui_->setOwnedToggleCallback([this](int blueprint, bool owned) {
        library_.setByHand(blueprint, owned);
        pushLibrary();
        // ...and into the game's texts, a moment later: ticks come in runs
        gameTextsDue_ = true;
        gameTextsDueAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    });

    // A backup put back in the Your data card. What it wrote is the truth
    // from here on: the library is read again at once - left alone, its next
    // save would put the one in memory back over it - and the settings file
    // stays as the backup has it until the next start reads it. It is
    // written on every close, so without the hold it would never be read.
    gui_->setDataRestoredCallback([this]() {
        library_.load();
        const std::filesystem::path log = library_.gameLog();
        library_.setGameLog(log);
        pushLibrary();
        gameTextsDue_ = true;
        gameTextsDueAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        settingsHeld_ = true;
        // ...and the plugins, from what the backup put back
        if (plugins_) {
            plugins_->load();
            for (const auto& info : plugins_->found()) plugins_->restart(info.id);
            pushPlugins();
        }
    });

    // The plugins: found, started, and handed the window
    setUpPlugins();

    // Ctrl+Alt+K, system-wide: keep the last few minutes of recording -
    // only while recording is on at all; otherwise the key stays free
    if (recordScans_) {
        gui_->setKeepRecordingCallback([this]() {
            keepRecording();
        });
    } else {
        gui_->releaseKeepHotkey();
    }
    // Ctrl+Alt+S, system-wide, always: the picture as the scan sees it
    gui_->setSnapshotCallback([this]() { requestSnapshot(); });

    // The scan pumps the window's messages at every expensive step, so the
    // window stays draggable while live mode works; otherwise a scan holds the
    // thread, and the title bar with it, for 0.6-2.7 s.
    ScanSession::Hooks hooks;
    hooks.breathe = [this]() { if (gui_ && inScan_) gui_->processMessages(); };
    hooks.overlayVisible = [this]() { return overlayHwnd_ && IsWindowVisible(overlayHwnd_); };
    hooks.haveOverlay = [this]() { return overlayHwnd_ != nullptr; };
    hooks.showOverlay = [this](bool show) {
        if (overlayHwnd_) ShowWindow(overlayHwnd_, show ? SW_SHOW : SW_HIDE);
    };
    hooks.beforeCapture = [this]() {
        if (!snapshotPending_) return;
        snapshotPending_ = false;
        saveSnapshot();
    };
    hooks.aimMoved = [this]() {
        if (gui_) {
            const auto box = scan_->aim();
            gui_->setRegionValues(box.x, box.y, box.width, box.height);
            gui_->setRegionAuto(true);
        }
        updateOverlayPosition();
    };
    hooks.firstCapture = [this]() {
        saveRegionConfig();
        writeSessionInfo();
    };
    scan_->setHooks(std::move(hooks));

    // Capture region callback - applies the region and runs a test scan
    // AUTO: back to the box derived from the game's viewport
    gui_->setRegionAutoCallback([this]() {
        scan_->armAutoAim();
        if (!refreshAutoAim(true) && gui_) gui_->setRegionAuto(true);
        saveRegionConfig();
        updateOverlayPosition();
    });

    gui_->setRegionChangedCallback([this](int x, int y, int w, int h) {
        scan_->setAim(x, y, w, h);
        if (gui_) gui_->setRegionAuto(false);
        saveRegionConfig();
        std::cout << "Region set: " << x << "," << y << " (" << w << "x" << h << ")" << std::endl;
        const ScanSession::OcrHit hit = scan_->probeAim(readingPolicy());
        if (hit.captured && autoTrack_) {
            std::cout << (hit.tracked
                              ? "[label found at " + std::to_string(hit.x) + "," + std::to_string(hit.y) + "]"
                              : "[no label found - using the fixed region]")
                      << std::endl;
        }
        if (hit.recognized) {
            std::cout << "[OCR recognized: " << hit.value << "]" << std::endl;
            lastRecognizedSignature_ = hit.value;
            auto results = analyzer_.analyzeSignature(hit.value);
            gui_->updateResults(hit.value, results, hit.confidence, analyzer_.outOfPlace());
            displayResultsCompact(results, hit.value);
        } else if (hit.captured) {
            std::cout << "[OCR failed - check debug/region_apply.png]" << std::endl;
        }
        saveRegionConfig();
    });

    // Hand the loaded region values (from initialize()) to the GUI
    {
        const auto box = scan_->aim();
        gui_->setRegionValues(box.x, box.y, box.width, box.height);
        gui_->setRegionAuto(box.automatic);
        gui_->setTrackingStatus(autoTrack_, false, box.x, box.y);
    }
    updateOverlayPosition();

    // The work orders read off refinery terminals before, and the window's
    // "Collected" taking one off the list
    refineryOrders_.load(AppPaths::file(REFINERY_ORDERS_FILE));
    gui_->setRefineryOrderDoneCallback([this](const RefineryOrder& done) {
        const auto& all = refineryOrders_.orders();
        for (size_t i = 0; i < all.size(); i++) {
            if (all[i].station == done.station && all[i].number == done.number && all[i].due == done.due) {
                refineryOrders_.remove(i);
                refineryOrders_.save(AppPaths::file(REFINERY_ORDERS_FILE));
                std::cout << "\n[refinery] " << done.station << ": work order " << done.number << " collected" << std::endl;
                break;
            }
        }
        pushRefineryOrders();
    });
    pushRefineryOrders();

    gui_->show();

    // ...and, once, what changed since the version they had
    maybeShowWhatsNew();
    // UEX, offered once: at the first start, when nobody has a key in yet.
    // It waits for the window to be free. A build without Bedrock's app token
    // can do nothing with an account.
    if (uexWanted_ && !uexOffered_ && uexKey_.empty() && !uexToken().empty()) gui_->offerUex();

    // Come back scanning if that is how the app was left. An instance that
    // starts idle looks exactly like one that is working, and the difference
    // only surfaces much later - in a saved recording that turns out to be
    // minutes old, or in readings that never arrive.
    if (startLive_) {
        applyLiveMode(true);
        gui_->setLiveModeActive(true);
    }

    lastScanTime_ = std::chrono::steady_clock::now();

    // Main loop with GUI
    while (running_ && gui_->isOpen()) {
        // Pump Windows messages
        if (!gui_->processMessages()) {
            break;
        }

        // Poll console input (when a console is attached)
        if (_kbhit()) {
            int key = _getch();

            switch (key) {
                case 'l':
                case 'L':
                    toggleLiveMode();
                    gui_->setLiveModeActive(liveModeActive_);
                    break;

                case 's':
                case 'S':
                    triggerScan();
                    break;

                case 'r':
                case 'R':
                    regionDetected_ = false;
                    lastRecognizedSignature_ = 0;
                    std::cout << "The region will be re-detected..." << std::endl;
                    break;

                case 'q':
                case 'Q':
                case 27:
                    running_ = false;
                    break;
            }
        }

        // The blueprint library follows the game's log
        pollLibrary();
        // ...and the plugins with it: their older logs a while each round
        servicePlugins();
        // ...the Datarunner the game's ScreenShots folder, more often, and the
        // commodity terminal in use
        if (watchScreenshots()) pushDatarunner();
        if (watchTerminal()) pushDatarunner();
        // ...and a refinery's terminal, at a station that has one
        lookForRefinery();
        handRefineryOrdersToUex();
        serviceRefineryShare();
        tendRockScans();
        serviceRockShare();
        serviceFeedback();

        // Live mode
        if (liveModeActive_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScanTime_).count();

            const int wait = effectiveIntervalMs();
            if (elapsed >= wait) {
                scan_->noteWait(wait);   // logged, so it can be checked
                lastScanTime_ = now;
                // ...while the game is on screen: without it the screen is
                // other programs', and nothing on it is Bedrock's to read
                if (gameOnScreen()) processScanWithGUI();
            }
        }

        // Single scan
        if (scanRequested_) {
            scanRequested_ = false;
            if (gameOnScreen()) processScanWithGUI();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Application closed." << std::endl;
}

// Whether Star Citizen runs: its window on screen, or else its process - a
// minimised game is still running, and a game whose window class changed in
// a patch is still found by its process. The process list is gone through
// every two seconds at the most.
bool Application::gameUp() {
    if (scan_->gameView().found) return true;
    return gameProcessUp();
}

// ...and whether any of it is on the screen: nothing reads the screen
// without it. Minimised, the game is all other programs' there. A game whose
// window is not found at all is still read while its process runs.
bool Application::gameOnScreen() {
    if (scan_->gameView().found) return true;
    if (scan_->gameMinimised()) return false;
    return gameProcessUp();
}

bool Application::gameProcessUp() {
    const auto now = std::chrono::steady_clock::now();
    if (now - gameProcessChecked_ >= std::chrono::seconds(2)) {
        gameProcessChecked_ = now;
        gameProcess_ = GameFiles::gameRunning();
    }
    return gameProcess_;
}

// How long to wait before the next scan. The base is what the player
// set; on top of it comes what the situation allows.
//
// One thing stretches it, and the very next find undoes it:
//
//  - The game is not the window in front. Then there is nothing on
//    screen to read at all and the scan is pure heat.
//
// The ceiling is deliberately low. Two seconds saves nearly everything
// there is to save; ten would save a little more and would now and then
// lose the first sight of a signature, which is the one thing this
// program is for. And nothing here returns zero scans: the slowest
// setting still looks four times a minute, so a wrong guess about what
// the player is doing shows up as latency, never as a dead program.
// Whether the window in front lies over the game. Not when it is the game,
// one of this app's own windows, or the desktop and the taskbar behind
// everything - and not when it is on another screen: the game is in full
// view there and still being played, and a chat open on a second monitor
// must not slow the scan down.

int Application::effectiveIntervalMs() const {
    return scan_->effectiveIntervalMs(captureIntervalMs_, easeIdle_);
}

// The middle reading of the recent ones, figure by figure.
//
// A median rather than an average: OCR fails by reading a whole digit wrong,
// so a bad reading is not a little off but a lot, and an average carries it
// into the answer where a median throws it away. The ore's name is not a
// number, so it takes the one seen most often.

// The rock in front of the ship, if the game is showing its figures: this
// is what makes the window switch to the rock view.
//
// On its own clock, and a slow one. The panel slides with the rock, so the
// search has to cover the whole viewport (5120x1440 on a 32:9 screen)
// where the signature reader only ever looks at a few hundred pixels. Once
// every second and a half is cheap enough not to be felt, and a glance at
// the place the last panel stood goes with every scan, so that a rock is
// not waited for.
// One constant rather than a setting, so that the reader, the search and
// the harvest tools stay testable with it either way.

// What the reader made of a panel, one line per look, for laying against
// a recording of the same minutes. The figures of this one look sit
// beside what the median over the last five made of them: a misread shows up
// in the first set, and whether the smoothing swallowed it or passed it on
// shows up in the second.
//
// Local time to the millisecond, because a video's file name says when it
// started and nothing finer. Written only while a panel is on screen, so
// flying about with nothing scanned writes nothing.

// The middle of the gauge's recent readings: the figure a flash or a
// misread numeral put there once does not move it.


// Every look at the screen for a rock, read or not, with what it cost and
// when it came. rock_readings.csv holds only the looks that read something,
// and what makes a rock come in late is the looks in between.

// What happens to a rock's list, step by step: a reading started, what it
// came back with and whether it was taken, the list shown, the list taken off
// and why. The other two logs say when a rock was looked at and what its
// figures read; this one says why its list came late or not at all.

// The ship the log names mines - or no ship is named yet, which says nothing
// either way. The three Bedrock reads, and whatever else carries a mining
// head: its scan panel comes up there too, even where the view cannot read
// its HUD yet. Standing beside the ship, or in one of a MOLE's turrets, the
// ship is still the one last boarded. Out of a ship's seat the panel is
// looked for whatever ship that was (panelsWanted): the next one may be
// boarded without the log saying so.
bool Application::shipCanMine() const {
    return panelsWanted(gameState_.ship(), gameState_.inSeat());
}


// The rock is off the screen: the view goes back to the signatures, and
// what was read of the rock goes with it

// The loadout a one-seat ship's mining HUD shows: the laser at the bottom
// left, the modules over it (LoadoutReader). The log names the very hull
// only when a target is picked in it or its seat is left - a quantum jump
// too short to be logged names none, so two Prospectors may be told apart
// only a jump later - while the HUD says it as soon as the mining mode is
// on. The MOLE's pilot never sees its heads, so its fit stays the log's
// and the player's.

void Application::loadoutLookAgain() {
    scan_->loadoutLookAgain();
}



// A new rock on view. A settled loadout is read again only every three
// minutes, and a change of ship the log does not report (two Prospectors,
// one boarded after the other) would leave the overlay judging the rock for
// the wrong lasers that long. So the first look at each new rock reads the
// loadout again when the last reading is half a minute old - one reading a
// rock at the most.



// The rock the view took, for rock_scans.csv: where and from which ship -
// the place as rockPlace() decides it, a place of the ore tables or none
// (the header may say more: a station, a gateway), and a place set by hand
// when it is such a place
void Application::noteRockScan(const RockStats& shown, const RockStats& look, int rows, float labelScale) {
    const std::string place = placeOverride_.empty() ? gameState_.rockPlace() : rockPlaceByHand(placeOverride_);
    const long long observed = (long long)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    rockScans_.taken(shown, look, observed, RockScans::shipKind(gameState_.ship()), place, rows, labelScale);
    keepRockScans();
}

// Each rock gone off view, a line in debug/rock_scans.csv once its wait is
// over (Recorder): its figures, the list where the view read it, the place
// and the ship - in a release too, for what the player wants to work out
// from their own rocks
void Application::keepRockScans(bool all) {
    const long long now = (long long)std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::vector<RockScans::Scan> done = all ? rockScans_.done() : rockScans_.done(now);
    if (done.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    const std::string path = dataPath("debug") + "/rock_scans.csv";
    // a file with other columns in it starts over
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path);
        std::string first;
        std::getline(in, first);
        in.close();
        while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
        if (first != RockScans::CSV_HEADER) std::filesystem::rename(path, dataPath("debug") + "/rock_scans.old.csv", ec);
    }
    const bool fresh = !std::filesystem::exists(path, ec);
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;
    if (fresh) out << RockScans::CSV_HEADER << std::endl;
    for (const RockScans::Scan& scan : done) {
        const std::time_t secs = (std::time_t)scan.observed;
        std::tm local{};
        localtime_s(&local, &secs);
        char when[40] = {};
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
        out << RockScans::csvLine(scan, when) << std::endl;
        // ...and to the service, where the player shares
        if (rockShare_ && !refineryServiceUrl().empty() && RockScans::sendable(scan)) rockShareQueue_.push_back(scan);
    }
    while (rockShareQueue_.size() > 500) rockShareQueue_.pop_front();
}

void Application::tendRockScans() {
    const auto now = std::chrono::steady_clock::now();
    if (now - rockScansTended_ < std::chrono::seconds(1)) return;
    rockScansTended_ = now;
    if (scan_->gameView().found) {
        rockGameUp_ = true;
        keepRockScans();
        return;
    }
    if (!rockScans_.holding() && !rockGameUp_) return;
    // The game put away: the rock on view waits as one off view does, and is
    // done half a minute on rather than when Bedrock closes
    rockScans_.gone();
    if (rockGameUp_ && !gameUp()) {
        // ...or closed: none of its rocks comes back on view, so each is done
        // now, and what waits for the service goes at once rather than with
        // the next report two minutes on - Bedrock may well be closed next
        rockGameUp_ = false;
        keepRockScans(true);
        if (!rockShareQueue_.empty()) rockShareNext_ = now;
        return;
    }
    keepRockScans();
}

void Application::setRockShare(bool on) {
    rockShare_ = on;
    if (!on) rockShareQueue_.clear();
    saveRegionConfig();
    if (gui_) gui_->setRockShare(rockShare_, !refineryServiceUrl().empty());
}

// The rocks, fifty at most to a report, off the UI thread: the first at
// once, then every two minutes while there are any, or as soon as fifty
// wait. Where the service could not be reached or was busy they wait two
// minutes, where it has had enough from this address ten; a report it
// turned down goes.
void Application::serviceRockShare() {
    const std::string base = refineryServiceUrl();
    if (base.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (rockShareJob_.valid() && rockShareJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const int status = rockShareJob_.get();
        if (status == 0 || status >= 500 || status == 429) {
            if (rockShare_) rockShareQueue_.insert(rockShareQueue_.begin(), rockShareSending_.begin(), rockShareSending_.end());
            rockShareRetry_ = now + std::chrono::minutes(status == 429 ? 10 : 2);
        } else if (status >= 400) {
            std::cout << "\nRock scans: the service turned a report down (" << status << ")" << std::endl;
        }
        rockShareSending_.clear();
    }
    if (rockShareJob_.valid() || !rockShare_ || rockShareQueue_.empty() || now < rockShareRetry_) return;
    if (now < rockShareNext_ && rockShareQueue_.size() < RockScans::ROCKS_PER_REPORT) return;
    while (!rockShareQueue_.empty() && rockShareSending_.size() < RockScans::ROCKS_PER_REPORT) {
        rockShareSending_.push_back(rockShareQueue_.front());
        rockShareQueue_.pop_front();
    }
    rockShareNext_ = now + std::chrono::minutes(2);
    const int game = library_.channel().rfind("LIVE", 0) == 0 ? library_.liveGameVersion() : 0;
    std::vector<std::string> headers = installHeader();
    headers.push_back("Content-Type: application/json");
    try {
        rockShareJob_ = std::async(std::launch::async, [url = base + "/v1/rocks/observations", headers,
                                                        body = RockScans::body(rockShareSending_, BEDROCK_VERSION, game,
                                                                               library_.channel(), rockSession_)]() {
            HttpAnswer answer;
            httpsPost(url, headers, body, answer, 64 * 1024);
            return answer.status;
        });
    } catch (const std::system_error&) {
        // no thread to be had: they go on the next round
        rockShareQueue_.insert(rockShareQueue_.begin(), rockShareSending_.begin(), rockShareSending_.end());
        rockShareSending_.clear();
    }
}

// ---- a message from the Feedback page (Feedback.h) -----------------------

// The game's version as a message carries it, and as the rock scans send
// theirs: LIVE's, where the log is LIVE's; 0 otherwise
int Application::feedbackGame() {
    return library_.channel().rfind("LIVE", 0) == 0 ? library_.liveGameVersion() : 0;
}

// Send clicked: the message to the service and, with the page's tick, the
// debug folder after it - picked, the user's name masked and zipped in
// memory, never written anywhere - all on one job off the UI thread
// (Feedback::send). One attempt; the page is told how far the zip is and
// what came of it all (serviceFeedback), and keeps the message where it did
// not go.
void Application::sendFeedback(const std::string& text, const std::string& name, bool debugFolder) {
    if (!gui_) return;
    if (feedbackJob_.valid()) return;   // (one at a time: the page sends none while one is on its way)
    const std::string base = refineryServiceUrl();
    if (base.empty()) {
        gui_->feedbackAnswered(Feedback::Result{});   // (no entry in the rail without the address)
        return;
    }
    if (Feedback::fault(text, name) != Feedback::Fault::None) {
        // (the page sends nothing that does not fit; were it to, this is
        // what the service would say)
        Feedback::Result tooLarge;
        tooLarge.status = 413;
        gui_->feedbackAnswered(tooLarge);
        return;
    }
    const std::filesystem::path folder = debugFolder ? AppPaths::data() / "debug" : std::filesystem::path();
    const int game = feedbackGame();
    const std::string channel = library_.channel();
    const std::vector<std::string> install = installHeader();
    const auto upload = std::make_shared<FeedbackUpload>();
    feedbackUpload_ = upload;
    try {
        feedbackJob_ = std::async(std::launch::async, [base, install, text, name, game, channel, folder, upload]() {
            // the debug folder as it goes; no folder, or nothing in it: no zip
            std::string zip;
            if (!folder.empty()) {
                const std::vector<DebugZip::Entry> picked =
                    DebugZip::pick(folder, DebugZip::masksHere(), Feedback::DEBUG_MOST);
                if (!picked.empty()) zip = DebugZip::zip(picked);
            }
            const std::string body = Feedback::body(text, name, BEDROCK_VERSION, game, channel, Feedback::platform(),
                                                    (std::int64_t)zip.size());
            const Feedback::Post post = [&](const std::string& path, const std::vector<std::string>& headers,
                                            const std::string& payload, const Feedback::Progress& progress,
                                            std::string& said) {
                std::vector<std::string> all = install;
                all.insert(all.end(), headers.begin(), headers.end());
                HttpAnswer answer;
                httpsPost(base + path, all, payload, answer, 64 * 1024, progress);
                said = answer.body;
                return answer.status;
            };
            return Feedback::send(body, zip, post, [upload](std::int64_t sent, std::int64_t total) {
                upload->total = total;
                upload->sent = sent;
            });
        });
    } catch (const std::system_error&) {
        gui_->feedbackAnswered(Feedback::Result{});   // no thread to be had: it did not go
    }
}

void Application::serviceFeedback() {
    if (!gui_) return;
    // what the page says goes along with a message, told when it changes:
    // the log names the game's version some time after the start
    const int game = feedbackGame();
    const bool install = !installId_.empty();
    if (game != feedbackGameTold_ || install != feedbackInstallTold_) {
        feedbackGameTold_ = game;
        feedbackInstallTold_ = install;
        gui_->setFeedback(!refineryServiceUrl().empty(), game, install);
    }
    if (!feedbackJob_.valid()) return;
    if (feedbackJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        gui_->feedbackAnswered(feedbackJob_.get());
    } else if (feedbackUpload_) {
        // how far the zip is: the page draws itself again where its MB change
        gui_->feedbackProgress(feedbackUpload_->sent.load(), feedbackUpload_->total.load());
    }
}

// What the rock on view holds: the composition list under the panel's
// figures - a share and a quality for every part of it (RockContents). A
// list is some five lines of two or three words, at 50 ms a word for the
// second reader, so it is read off the window's thread like the loadout, on
// a copy of that corner of the picture, and only while the view has no list
// for this rock: on every look until two readings say the same - two looks,
// over a ground that lets the list be read. A reading counts only when its
// shares add up to the rock and every quality is one its ore has; two alike
// are asked for all the same, because a 6 read for an 8 in a share's last
// place passes both (54 readings to 3 on one Titanium rock).
//
// Which rock a list is of: the one whose mass the view had when the picture
// was taken, and no other - a pilot pans from one rock to the next of its
// kind without the panel going away, say two Hephaestanite rocks of 8,513
// and 7,058 kg one after the other. Two readings that say the same are one
// rock's whatever the mass did in between, for no two rocks have one list.
// The same mass, or the same mass with one figure misread (21,460 and
// 21,464 on one Copper rock, look by look), counts as one: otherwise a list
// read at the one is thrown away at the other, and the second reading that
// would agree with the first never comes. Two rocks a pilot looks at one
// after the other do not differ in a single figure.


// The last panel's place, across sessions.
//
// A player who quits in the pilot seat sits down in it again: the panel
// comes back where it was, and the glance can look there from the first
// scan instead of after the first full search - which, with nothing known,
// tries the orange ink only every other look and takes up to three seconds
// to notice the first rock. The viewport's size goes with it: another
// resolution puts the panel elsewhere, and a spot that does not fit reaches
// no bar and costs a few milliseconds a scan until a panel is read.


void Application::applyRock(const ScanSession::RockReport& report) {
    for (const ScanSession::Event& ev : report.events) {
        switch (ev.kind) {
        case ScanSession::Event::Kind::UpdateRock:
            if (gui_) gui_->updateRock(ev.rock);
            if (ev.noteScan) noteRockScan(ev.rock, ev.rawLook, ev.rows, ev.labelScale);
            break;
        case ScanSession::Event::Kind::ClearRock:
            rockScans_.gone();
            keepRockScans();
            if (gui_) gui_->clearRock();
            break;
        case ScanSession::Event::Kind::SetContents:
            if (gui_) gui_->setRockContents(ev.contents);
            uexRockOres_.clear();
            for (const RockPartRead& part : ev.contents.parts) {
                if (!part.inert && !part.ore.empty() && part.quality >= UEX_MARKET_QUALITY)
                    uexRockOres_.push_back(part.ore);
            }
            break;
        case ScanSession::Event::Kind::ClearContents:
            if (gui_) gui_->setRockContents(RockContents());
            break;
        case ScanSession::Event::Kind::Loadout: {
            const MiningShip* kind = nullptr;
            for (int i = 0; i < MINING_SHIP_COUNT && !ev.loadout.kindId.empty(); i++) {
                if (std::strcmp(MINING_SHIPS[i].id, ev.loadout.kindId.c_str()) == 0) kind = &MINING_SHIPS[i];
            }
            const MiningLaser* laser = laserByHudLine(ev.loadout.laserText);
            std::vector<const MiningModule*> modules;
            bool ok = kind && laser && ev.loadout.moduleTexts.size() == ev.loadout.moduleBelow.size();
            if (ok) {
                for (size_t i = 0; i < ev.loadout.moduleTexts.size(); i++) {
                    const MiningModule* module = moduleByHudBlock(ev.loadout.moduleTexts[i], ev.loadout.moduleBelow[i]);
                    if (!module) { ok = false; break; }
                    modules.push_back(module);
                }
            }
            std::string fit;
            bool changed = false;
            if (ok && gui_) fit = gui_->setSeenLoadout(*kind, *laser, modules, ev.loadout.own, &changed);
            scan_->noteLoadoutResult(fit, changed);
            if (ok) {
                std::string names = laser->name;
                for (const MiningModule* module : modules) names += std::string(", ") + module->name;
                std::cout << "\n[loadout] the HUD shows " << names
                          << (fit.empty() ? std::string(" - no ship in the list has that, and none could take it")
                                          : " - ship '" + fit + "'" + (changed ? ", which has it from now on" : ""))
                          << std::endl;
            }
            break;
        }
        case ScanSession::Event::Kind::Turret: {
            const MiningLaser* laser = turretLaserByHudLine(ev.turretLine);
            int head = -1;
            if (laser && gui_) head = gui_->setTurretLaser(laser);
            if (laser) {
                std::cout << "\n[turret] the HUD shows " << laser->name
                          << (head >= 0 ? " - head " + std::to_string(head + 1)
                                        : std::string(" - no head of the fit has it"))
                          << std::endl;
            }
            break;
        }
        case ScanSession::Event::Kind::ClearTurret:
            if (gui_) gui_->setTurretLaser(nullptr);
            std::cout << (ev.turretMinute ? "\n[turret] no turret's HUD in view for a minute"
                                          : "\n[turret] out of the turret")
                      << std::endl;
            break;
        case ScanSession::Event::Kind::ListTried:
            rockScans_.tried(ev.rock.mass);
            break;
        case ScanSession::Event::Kind::ListRecorded: {
            std::vector<RockScans::ScanPart> parts;
            for (const RockPartRead& part : ev.contents.parts)
                parts.push_back({part.ore, part.inert, part.percent, part.quality});
            rockScans_.listed(ev.rock.mass, parts);
            break;
        }
        case ScanSession::Event::Kind::ShipSeen:
            if (gui_) {
                for (int i = 0; i < MINING_SHIP_COUNT; i++) {
                    if (std::strcmp(MINING_SHIPS[i].id, ev.shipId.c_str()) == 0) {
                        gui_->shipSeenOnHud(MINING_SHIPS[i]);
                    }
                }
            }
            break;
        }
    }
}

void Application::processScanWithGUI() {
    if (inScan_) return;
    struct ScanGuard { bool& flag; ~ScanGuard() { flag = false; } } scanGuard{inScan_};
    inScan_ = true;

    if (overlayHwnd_ && IsWindowVisible(overlayHwnd_)) {
        RECT rect{};
        if (GetWindowRect(overlayHwnd_, &rect)) {
            scan_->setCovers({{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top}});
        }
    } else {
        scan_->setCovers({});
    }
    ScanSession::RockAsk ask;
    ask.inSeat = gameState_.inSeat();
    ask.shipCanMine = shipCanMine();
    ask.headCount = gui_ ? gui_->headCount() : 0;
    ask.ship = gameState_.ship();
    ask.hull = gameState_.hull();
    ask.boardings = gameState_.boardings();
    {
        using Rocks = SignatureAnalyzer::Rocks;
        const Rocks here = analyzer_.rocksHere();
        ask.rocksHere = here == Rocks::Own ? 1 : here == Rocks::Either ? 2 : 0;
    }
    ask.gameGeneration = library_.liveGameVersion() / 10000;
    ask.channel = library_.channel();
    applyRock(scan_->lookForRock(ask));

    scan_->setCovers({});
    const ScanSession::SignatureReport sig = scan_->readSignature(readingPolicy(), lastRecognizedSignature_);
    applyRock(ScanSession::RockReport{sig.rockEvents});
    lastRecognizedSignature_ = sig.recognized;
    if (gui_ && sig.haveTrack) gui_->setTrackingStatus(sig.sawLabels, sig.locked, sig.trackX, sig.trackY);
    if (sig.updated) {
        auto results = analyzer_.analyzeSignature(sig.value);
        if (gui_) gui_->updateResults(sig.value, results, sig.confidence, analyzer_.outOfPlace());
        displayResultsCompact(results, sig.value);
        if (resultCallback_) resultCallback_(results);
    }
}

ReadingPolicy Application::readingPolicy() {
    return makeReadingPolicy(analyzer_);
}

void Application::stop() {
    running_ = false;
}

void Application::setResultCallback(ResultCallback callback) {
    resultCallback_ = callback;
}

void Application::setCaptureInterval(int milliseconds) {
    captureIntervalMs_ = milliseconds;
    if (scan_) scan_->setInterval(milliseconds);
}

void Application::setHotkey(int vkCode) {
    hotkeyVkCode_ = vkCode;
}

void Application::triggerScan() {
    scanRequested_ = true;
    loadoutLookAgain();
}

void Application::toggleLiveMode() {
    liveModeActive_ = !liveModeActive_;

    if (liveModeActive_) {
        loadoutLookAgain();
        lastRecognizedSignature_ = 0;  // reset for a fresh detection
        lastScanTime_ = std::chrono::steady_clock::now();
        std::cout << "\n========================================" << std::endl;
        std::cout << "  [LIVE] mode ENABLED" << std::endl;
        std::cout << "  Scan interval: " << captureIntervalMs_ << "ms" << std::endl;
        std::cout << "  Press 'L' to stop" << std::endl;
        std::cout << "========================================\n" << std::endl;
    } else {
        std::cout << "\n[LIVE] mode DISABLED\n" << std::endl;
    }
}

bool Application::isLiveModeActive() const {
    return liveModeActive_;
}

void Application::setAutoTrack(bool enabled) {
    autoTrack_ = enabled;
    if (scan_) scan_->setAutoTrack(enabled);
    std::cout << "Auto-tracking: " << (enabled ? "ON" : "OFF") << std::endl;
    saveRegionConfig();
}

bool Application::isAutoTrackActive() const {
    return autoTrack_;
}

// The aim box for a game drawn in this viewport, in the proportions of a
// 5120x1440 viewport: 352x180 with its centre at 2560,475 - centred on the
// crosshair, a third of the way down - and 250 px of search margin around
// it (400 takes in the top of a Prospector's screen and the lower canopy
// of a fighter: see g_outside). On 53 markers read in that setup, the
// centres sat at y 402-547 (mean 486) and x 2522-2565, the plates from
// 382 to 567. Everything scales with the HEIGHT: the HUD keeps its size
// relative to the vertical field of view, an ultrawide only adds width at
// the sides. And it sits wherever the viewport is, so the box lands on the
// monitor the game runs on in a multi-monitor setup.

bool Application::refreshAutoAim(bool announce) {
    const bool changed = scan_->refreshAutoAim(announce);
    if (changed && gui_) {
        const auto box = scan_->aim();
        gui_->setRegionValues(box.x, box.y, box.width, box.height);
        gui_->setRegionAuto(true);
    }
    return changed;
}



void Application::applyLiveMode(bool active) {
    liveModeActive_ = active;
    std::cout << "Live mode: " << (active ? "ON" : "OFF") << std::endl;
    // What the player flies may have changed while nothing was looked at: the
    // first look reads the loadout, so a ship switched while live was off is
    // picked up the first time live starts.
    if (active) loadoutLookAgain();
    scan_->setLive(active);
    scan_->syncRecording(active);

    // Hide the overlay in live mode (it would disturb the OCR)
    if (overlayHwnd_) {
        ShowWindow(overlayHwnd_, active ? SW_HIDE : SW_SHOW);
    }

    saveRegionConfig();   // so a restart comes back in the same state
}

void Application::keepRecording() {
    scan_->keepRecording();
}






void Application::processScan() {
    std::cout << "\nScanning the screen..." << std::endl;
    const ScanSession::OcrHit hit = scan_->readFixed(readingPolicy());
    if (!hit.captured) {
        std::cerr << "Error: could not capture the screen!" << std::endl;
        return;
    }
    if (autoTrack_) {
        std::cout << (hit.tracked
                          ? "Label located at " + std::to_string(hit.x) + "," + std::to_string(hit.y)
                          : "No label in the search area - using the fixed region")
                  << std::endl;
    }
    if (!hit.recognized) {
        std::cout << "No number recognized. Try manual input (key 'M')." << std::endl;
        return;
    }
    std::cout << "Recognized signature: " << hit.value << " (confidence: " << hit.confidence << "%)" << std::endl;
    auto results = analyzer_.analyzeSignature(hit.value);
    displayResults(results, hit.value);
    if (resultCallback_) resultCallback_(results);
}

void Application::displayResults(const std::vector<AnalysisResult>& results, int scannedValue) {
    std::string report = analyzer_.generateReport(scannedValue);
    std::cout << report << std::endl;
}

void Application::displayResultsCompact(const std::vector<AnalysisResult>& results, int scannedValue) {
    if (results.empty()) return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);

    char timeStr[12];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

    // Compact one-line output
    std::cout << "[" << timeStr << "] Signature: " << scannedValue << " -> ";

    // Top 3 results
    int count = 0;
    for (const auto& r : results) {
        if (count > 0) std::cout << " | ";
        std::cout << r.name << " x" << r.clusterSize << " "
                  << std::fixed << std::setprecision(0) << r.probability << "%";
        if (++count >= 3) break;
    }

    // Highlight a clear best match
    if (!results.empty() && results[0].probability > 50.0) {
        std::cout << " << " << results[0].name << " x" << results[0].clusterSize;
    }

    std::cout << std::endl;
}

void Application::processScanLive() {
    const ScanSession::OcrHit hit = scan_->readFixed(readingPolicy());
    if (!hit.captured || !hit.recognized) return;
    if (hit.value == lastRecognizedSignature_) return;
    lastRecognizedSignature_ = hit.value;
    auto results = analyzer_.analyzeSignature(hit.value);
    displayResultsCompact(results, hit.value);
    if (resultCallback_) resultCallback_(results);
}

void Application::setupHotkey() {
    // Register the hotkey (F9)
    if (!RegisterHotKey(nullptr, 1, 0, hotkeyVkCode_)) {
        std::cerr << "Warning: could not register the hotkey." << std::endl;
    }
}

#ifdef _WIN32
LRESULT CALLBACK Application::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* kbStruct = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (g_appInstance && kbStruct->vkCode == static_cast<DWORD>(g_appInstance->hotkeyVkCode_)) {
            g_appInstance->triggerScan();
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
#endif

// What changed, put up once on the first start after an update. The marker
// lives beside the player's things, not beside the exe: the program folder
// is what an update replaces, and a note that reappeared after every update
// would be exactly the thing it is trying not to be.
//
// A first-ever run is not an update. Nothing is new about a program someone
// is opening for the first time, so the first start only writes the marker.
// region_config.txt is the tell - it is written as soon as anything is
// changed or the app is closed, so anyone who has used Bedrock before has one.
void Application::maybeShowWhatsNew() {
    if (!gui_) return;
    const std::string marker = dataPath("seen_version.txt");
    std::string seen;
    {
        std::ifstream in(marker);
        if (in.is_open()) std::getline(in, seen);
    }
    while (!seen.empty() && (seen.back() == '\r' || seen.back() == ' '))
        seen.pop_back();
    if (seen == BEDROCK_VERSION) return;

    std::error_code ec;
    const bool firstEverRun =
        seen.empty() && !std::filesystem::exists(dataPath(REGION_CONFIG_FILE), ec);
    {
        std::ofstream out(marker, std::ios::trunc);
        if (out.is_open()) out << BEDROCK_VERSION << std::endl;
    }
    switch (whatsNewAfter(seen, BEDROCK_VERSION, firstEverRun)) {
        case WhatsNew::Tour:
            gui_->showTour();
            break;
        case WhatsNew::News:
            gui_->showNews();
            break;
        case WhatsNew::Thanks:
            gui_->showThanks();
            break;
        case WhatsNew::Card:
            gui_->showWhatsNew();
            break;
        case WhatsNew::Nothing:
            break;
    }
}

Application::WhatsNew Application::whatsNewAfter(const std::string& seen, const std::string& version,
                                                 bool firstEverRun) {
    if (seen == version || firstEverRun) return WhatsNew::Nothing;
    int major = 0, minor = 0, patch = 0;
    {
        size_t at = 0;
        while (at < seen.size() && std::isdigit((unsigned char)seen[at])) major = major * 10 + (seen[at++] - '0');
        if (at < seen.size() && seen[at] == '.') at++;
        while (at < seen.size() && std::isdigit((unsigned char)seen[at])) minor = minor * 10 + (seen[at++] - '0');
        if (at < seen.size() && seen[at] == '.') at++;
        while (at < seen.size() && std::isdigit((unsigned char)seen[at])) patch = patch * 10 + (seen[at++] - '0');
    }
    // 3.0 shows what it is, page by page in the news window, to everyone
    // coming from before it; the card is a link on its last page. A hotfix
    // of it does not show it twice, and the release after it no longer at all.
    if ((version == "3.0" || version.rfind("3.0.", 0) == 0) && major < 3) return WhatsNew::Tour;
    // Coming from anything before 2.5 (1.x, 2.0.x, or a build older than the
    // marker): the news window, since the rock view and the game's contract
    // texts change what the window is for and a scrolling card would bury
    // them. From 2.5 on: the What's New card.
    const bool before25 = seen.empty() || major < 2 || (major == 2 && minor < 5);
    // 2.6.7 says thank you first, in the news window, to everyone coming
    // from before it - the card follows when that is closed. A hotfix of it
    // does not say it twice, and the release after it no longer at all.
    const bool thanks = version.rfind("2.6.7", 0) == 0 &&
                        (major < 2 || (major == 2 && (minor < 6 || (minor == 6 && patch < 7))));
    if (before25) return WhatsNew::News;
    if (thanks) return WhatsNew::Thanks;
    return WhatsNew::Card;
}

void Application::saveRegionConfig() {
    // After a restore the file is the backup's until the next start
    if (settingsHeld_) return;
    // Relative to the working directory (main() sets it to the exe folder)
    std::ofstream file(dataPath(REGION_CONFIG_FILE));
    if (file.is_open()) {
        const auto aim = scan_->aim();
        file << "x=" << aim.x << std::endl;
        file << "y=" << aim.y << std::endl;
        file << "width=" << aim.width << std::endl;
        file << "height=" << aim.height << std::endl;
        file << "autotrack=" << (autoTrack_ ? 1 : 0) << std::endl;
        file << "trackmargin=" << aim.margin << std::endl;
        // Written for the reader above; the box is automatic unless this
        // file says otherwise on purpose
        file << "fixedaim=" << (aim.automatic ? 0 : 1) << std::endl;
        // The frame on screen, off unless asked for - see overlayEnabled_
        file << "overlay=" << (overlayEnabled_ ? 1 : 0) << std::endl;
        file << "record=" << (recordScans_ ? 1 : 0) << std::endl;
        file << "ease=" << (easeIdle_ ? 1 : 0) << std::endl;
        file << "threads=" << scanThreads_ << std::endl;
        file << "keeps=" << maxKeeps_ << std::endl;
        file << "snapshots=" << snapshotKeep_ << std::endl;
        file << "keepgb=" << maxKeepGb_ << std::endl;
        // 100: pictures are read as captured (measured best); 0: the HUD scale
        // follows the game picture (1.5 on 4K); another number forces it
        file << "hudscale=" << hudScalePercent_ << std::endl;
        // How big the app draws itself, as set with Ctrl+plus / Ctrl+minus
        file << "uiscale=" << uiScalePercent_ << std::endl;
        // gamelog=<path to Game.log> pins the log the blueprint library
        // reads; normally it is found from the running game or the launcher
        if (!gameLogOverride_.empty()) file << "gamelog=" << gameLogOverride_ << std::endl;
        // Whether the scan was running when the app was last closed. A restart
        // that comes back idle looks identical to one that is working, and the
        // difference only shows up much later - in a recording that turns out
        // to be minutes old.
        file << "live=" << (liveModeActive_ ? 1 : 0) << std::endl;
        // Written back, not only read: this file is rewritten on every close, so
        // a value set by hand (update=0) would otherwise last one session.
        file << "update=" << (updateCheck_ ? 1 : 0) << std::endl;
        // stats=0: the update check goes without install_id.txt's number
        file << "stats=" << (statsWanted_ ? 1 : 0) << std::endl;
        file << "gametexts=" << (gameTextsWanted_ ? 1 : 0) << std::endl;
        file << "gamenames=" << (gameNamesWanted_ ? 1 : 0) << std::endl;
        // sharerefinery=0: the box in the Refinery window unticked - see
        // RefineryShare. On until then; an earlier test build's
        // "refineryshare" was off before anyone could choose, and is let go.
        file << "sharerefinery=" << (refineryShare_ ? 1 : 0) << std::endl;
        // sharerocks=0: the box in Settings unticked - see RockScans
        file << "sharerocks=" << (rockShare_ ? 1 : 0) << std::endl;
        // uex=0: the Ore Finder never asks uexcorp.space for prices
        file << "uex=" << (uexWanted_ ? 1 : 0) << std::endl;
        // Reporting to UEX: 0, or when the player agreed to it (on a UEX
        // card); the key is not here but in uex_key.dat
        file << "uexreport=" << uexReportSince_ << std::endl;
        // ...and whether the first start's offer of it was made
        file << "uexoffered=" << (uexOffered_ ? 1 : 0) << std::endl;
        // UEX Datarunner: 0, or when the player turned it on (on the UEX card)
        file << "datarunner=" << datarunnerSince_ << std::endl;
        // ...and whether its introduction comes up when the rail opens it
        file << "datarunnerintro=" << (datarunnerIntroOff_ ? 0 : 1) << std::endl;
        if (!updateUrl_.empty()) file << "updateurl=" << updateUrl_ << std::endl;
        // The viewport the aim point was picked in. The point was chosen by
        // eye against the HUD, and the HUD is laid out in the viewport, not on
        // the desktop - so noting the viewport lets the point be carried over
        // when the game is later drawn at a different size or place.
        const auto view = scan_->gameView();
        if (view.found) {
            file << "viewx=" << view.x << std::endl;
            file << "viewy=" << view.y << std::endl;
            file << "vieww=" << view.width << std::endl;
            file << "viewh=" << view.height << std::endl;
        }
    }
    mirrorConfigToDebug();
}

void Application::loadRegionConfig() {
    std::ifstream file(dataPath(REGION_CONFIG_FILE));
    int ax = 100, ay = 100, aw = 200, ah = 50, margin = 250;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    bool automatic = true;
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                // The one key whose value is a path, not a number
                if (key == "gamelog") {
                    gameLogOverride_ = line.substr(pos + 1);
                    continue;
                }
                if (key == "updateurl") {
                    updateUrl_ = line.substr(pos + 1);
                    continue;
                }
                // a Unix second, past what an int holds in 2038
                if (key == "uexreport" || key == "datarunner") {
                    long long& since = key == "uexreport" ? uexReportSince_ : datarunnerSince_;
                    try {
                        since = std::stoll(line.substr(pos + 1));
                    } catch (const std::exception&) {
                        since = 0;
                    }
                    continue;
                }
                int value = 0;
                try {
                    value = std::stoi(line.substr(pos + 1));
                } catch (const std::exception&) {
                    continue;
                }

                if (key == "x") ax = value;
                else if (key == "y") ay = value;
                else if (key == "width") aw = value;
                else if (key == "height") ah = value;
                // "autotrack" is read for compatibility and ignored: tracking is
                // always on.
                else if (key == "autotrack") autoTrack_ = true;
                else if (key == "trackmargin") margin = std::clamp(value, 50, 2000);
                // "aimauto" is read and dropped: an older file can hold a manual
                // setting with no button left to switch it back. The automatic box
                // is the right one.
                else if (key == "aimauto") { /* ignored */ }
                // A fixed box is still possible, but it has to be asked for
                // on purpose: fixedaim=1 with x/y/width/height below.
                else if (key == "fixedaim") automatic = (value == 0);
                else if (key == "overlay") overlayEnabled_ = (value != 0);
                // record=0 turns the rolling recording off. It is lossless and
                // therefore large - about 90 MB per minute of scanning, ~460 MB
                // for the full ring of five.
                else if (key == "record") recordScans_ = (value != 0);
                // ease=0 makes the scan run flat out. threads= caps how much of the
                // machine one scan may take at once; 0 works it out from the core count.
                else if (key == "ease") easeIdle_ = (value != 0);
                else if (key == "threads") scanThreads_ = std::clamp(value, 0, 64);
                else if (key == "keeps") maxKeeps_ = std::clamp(value, 1, 1000);
                else if (key == "snapshots") snapshotKeep_ = std::clamp(value, 1, 1000);
                else if (key == "keepgb") maxKeepGb_ = std::clamp(value, 1, 10000);
                else if (key == "live") startLive_ = (value != 0);
                else if (key == "update") updateCheck_ = (value != 0);
                else if (key == "stats") statsWanted_ = (value != 0);
                else if (key == "uex") uexWanted_ = (value != 0);
                else if (key == "uexoffered") uexOffered_ = (value != 0);
                else if (key == "datarunnerintro") datarunnerIntroOff_ = (value == 0);
                // gametexts=1: the box in the Blueprints window - see GameTexts
                else if (key == "gametexts") gameTextsWanted_ = (value != 0);
                // gamenames=1: the box beside it, for the components' names
                else if (key == "gamenames") gameNamesWanted_ = (value != 0);
                else if (key == "sharerefinery") refineryShare_ = (value != 0);
                else if (key == "sharerocks") rockShare_ = (value != 0);
                else if (key == "hudscale") hudScalePercent_ = std::clamp(value, 0, 500);
                else if (key == "uiscale") uiScalePercent_ = std::clamp(value, 0, 250);
                else if (key == "viewx") vx = value;
                else if (key == "viewy") vy = value;
                else if (key == "vieww") vw = value;
                else if (key == "viewh") vh = value;
            }
        }
        scan_->loadAim(ax, ay, aw, ah, automatic, margin);
        if (!automatic) scan_->carryFixedAim(vx, vy, vw, vh);
        const auto box = scan_->aim();
        std::cout << "Region loaded: " << box.x << "," << box.y
                  << " (" << box.width << "x" << box.height << ")" << std::endl;
    } else {
        scan_->loadAim(100, 100, 200, 50, true, 250);
    }
    scan_->setRecord(recordScans_, maxKeeps_, maxKeepGb_);
    scan_->setEase(easeIdle_);
    scan_->setInterval(captureIntervalMs_);
    scan_->setHudPercent(hudScalePercent_);
    scan_->setAutoTrack(autoTrack_);
}

// The picture the game is drawn in: its window when it is running, otherwise
// the monitor under the aim box - the box was made for that monitor too



static std::string timeStamp(const char* format) {
    char stamp[40] = {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), format, &local);
    return stamp;
}

// Everything a scan on this machine works from, for the session file and
// the snapshot note
std::filesystem::path Application::locateGameLog() const {
    std::error_code ec;
    // 1. pinned by hand
    if (!gameLogOverride_.empty()) {
        std::filesystem::path pinned(gameLogOverride_);
        if (std::filesystem::exists(pinned, ec)) return pinned;
    }
#ifdef _WIN32
    // 2. the running game: its exe sits in <channel>\Bin64, the log one up
    if (HWND window = static_cast<HWND>(scan_->gameWindowHandle())) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t exe[1024] = {};
            DWORD length = 1024;
            std::filesystem::path log;
            if (QueryFullProcessImageNameW(process, 0, exe, &length)) {
                log = std::filesystem::path(exe).parent_path().parent_path() / "Game.log";
            }
            CloseHandle(process);
            if (!log.empty() && std::filesystem::exists(log, ec)) return log;
        }
    }
    // 3. the launcher's log names the folder it last started the game from:
    //    "[Launcher::launch] Launching Star Citizen LIVE from (E:\\RSI\\StarCitizen\\LIVE)"
    char appdata[1024] = {};
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) > 0) {
        std::ifstream in(std::filesystem::path(appdata) / "rsilauncher" / "logs" / "log.log");
        std::string line, folder;
        while (std::getline(in, line)) {
            const size_t k = line.find("Launching Star Citizen ");
            if (k == std::string::npos) continue;
            const size_t a = line.find(" from (", k);
            const size_t b = (a == std::string::npos) ? a : line.find(')', a);
            if (a == std::string::npos || b == std::string::npos) continue;
            const std::string raw = line.substr(a + 7, b - a - 7);
            std::string path;
            for (size_t i = 0; i < raw.size(); i++) {
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') i++;   // JSON doubles them
                path += raw[i];
            }
            folder = path;   // the last launch wins
        }
        if (!folder.empty()) {
            const std::filesystem::path log = std::filesystem::path(folder) / "Game.log";
            if (std::filesystem::exists(log, ec)) return log;
        }
    }
#else
    // 2. Linux: the game runs in a Wine prefix, and the prefix is wherever
    //    Steam, Lutris or Bottles put it. The usual places, and of the logs
    //    found the one written to last - the one the running game writes.
    //    gamelog= in region_config.txt names it outright (step 1 above).
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const std::filesystem::path h(home);
        const std::filesystem::path inPrefix =
            std::filesystem::path("drive_c") / "Program Files" / "Roberts Space Industries" / "StarCitizen";
        std::vector<std::filesystem::path> prefixes;
        for (const auto& steam : {h / ".steam" / "steam" / "steamapps" / "compatdata",
                                  h / ".local" / "share" / "Steam" / "steamapps" / "compatdata",
                                  h / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam" / "steamapps" / "compatdata"}) {
            for (const auto& entry : std::filesystem::directory_iterator(steam, ec)) {
                if (entry.is_directory(ec)) prefixes.push_back(entry.path() / "pfx");
            }
        }
        for (const auto& lutris : {h / "Games", h / ".local" / "share" / "lutris" / "runners" / "wine"}) {
            for (const auto& entry : std::filesystem::directory_iterator(lutris, ec)) {
                if (entry.is_directory(ec)) prefixes.push_back(entry.path());
            }
        }
        for (const auto& bottles : {h / ".var" / "app" / "com.usebottles.bottles" / "data" / "bottles" / "bottles",
                                    h / ".local" / "share" / "bottles" / "bottles"}) {
            for (const auto& entry : std::filesystem::directory_iterator(bottles, ec)) {
                if (entry.is_directory(ec)) prefixes.push_back(entry.path());
            }
        }
        prefixes.push_back(h / ".wine");
        std::filesystem::path newest;
        std::filesystem::file_time_type newestTime{};
        for (const auto& prefix : prefixes) {
            for (const char* channel : {"LIVE", "PTU", "EPTU", "HOTFIX", "TECH-PREVIEW"}) {
                const std::filesystem::path log = prefix / inPrefix / channel / "Game.log";
                if (!std::filesystem::exists(log, ec)) continue;
                const auto written = std::filesystem::last_write_time(log, ec);
                if (newest.empty() || written > newestTime) {
                    newest = log;
                    newestTime = written;
                }
            }
        }
        if (!newest.empty()) return newest;
    }
#endif
    return {};
}

// "STANTON" -> "tanton", for the place line's "Stanton - Daymar"
static std::string toLowerRest(const std::string& word) {
    std::string rest = word.size() > 1 ? word.substr(1) : std::string();
    for (char& c : rest) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return rest;
}

// "2026-09-27T09:27:53": a moment in the spelling the game log uses, UTC
static std::string utcStamp(std::chrono::system_clock::time_point at) {
    using namespace std::chrono;
    const auto secs = floor<seconds>(at);
    const auto day = floor<days>(secs);
    const year_month_day ymd{day};
    const hh_mm_ss<seconds> tod{secs - day};
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02d:%02d:%02d", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(tod.hours().count()), static_cast<int>(tod.minutes().count()),
                  static_cast<int>(tod.seconds().count()));
    return text;
}

// What Game.log says, into the app: the system onto the buttons and the
// filter, the ship into the header and the scan log. The log sets the
// system when IT changes; a button pressed by hand holds until then.
void Application::loadPlaceOverride() {
    std::ifstream in(AppPaths::file(PLACE_OVERRIDE_FILE));
    if (!in) return;
    std::string name, stamp;
    std::getline(in, name);
    std::getline(in, stamp);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    while (!stamp.empty() && (stamp.back() == '\r' || stamp.back() == ' ')) stamp.pop_back();
    // Only a place the tables still know: one renamed or dropped between
    // versions would otherwise pin the app to something that does not exist.
    if (!name.empty() && !placeByLogName(name, "").known()) return;
    placeOverride_ = name;
    // When it was set: the second line. Older files have the name alone,
    // and the file's own time on disk stands in for it.
    if (!name.empty() && (stamp.size() != 19 || stamp[10] != 'T')) {
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(AppPaths::file(PLACE_OVERRIDE_FILE), ec);
#ifdef _WIN32
        stamp = ec ? std::string() : utcStamp(std::chrono::clock_cast<std::chrono::system_clock>(written));
#else
        // clock_cast is GCC 13's; the Linux package is built with GCC 11,
        // whose file_clock knows the way to system time itself
        stamp = ec ? std::string() : utcStamp(std::chrono::file_clock::to_sys(written));
#endif
    }
    placeOverrideStamp_ = name.empty() ? std::string() : stamp;
    if (gui_) gui_->setPlaceOverride(placeOverride_);
}

void Application::setPlaceOverride(const std::string& placeName) {
    placeOverride_ = placeName;
    placeOverrideStamp_ = placeName.empty() ? std::string() : utcStamp(std::chrono::system_clock::now());
    std::ofstream out(AppPaths::file(PLACE_OVERRIDE_FILE), std::ios::trunc);
    if (out) out << placeOverride_ << "\n" << placeOverrideStamp_ << "\n";
    placeLine_.clear();       // force the block below to recompute and repaint
    placeOre_ = "\x01";
    applyGameState();
    std::cout << "\nPlace set by hand: "
              << (placeOverride_.empty() ? "no - following the game log" : placeOverride_)
              << std::endl;
}

void Application::applyGameState() {
    // A place set by hand holds until the game itself says otherwise: a
    // line written AFTER the setting that names another place the tables
    // know, or another system, hands the place back to the log. Otherwise a
    // stale setting keeps showing the wrong place and filters the ores by
    // it. A line written before the setting says nothing: it is what the
    // setting overruled.
    if (!placeOverride_.empty() && !gameState_.namedStamp().empty() &&
        gameState_.namedStamp() > placeOverrideStamp_ &&
        contradictsHandPlace(placeOverride_, gameState_.namedPlace(), gameState_.namedSystem())) {
        std::cout << "\nPlace set by hand (" << placeOverride_ << ") handed back to the game log, which put the ship "
                  << (gameState_.namedPlace().empty() ? "in " + gameState_.namedSystem()
                                                      : "at " + gameState_.namedPlace())
                  << " at " << gameState_.namedStamp() << " UTC" << std::endl;
        placeOverride_.clear();
        placeOverrideStamp_.clear();
        std::ofstream out(AppPaths::file(PLACE_OVERRIDE_FILE), std::ios::trunc);
        if (gui_) gui_->setPlaceOverride("");
    }
    // The log spells the system "Nyx", the buttons spell it "NYX", and the
    // app compares against the buttons' spelling - whether a button is lit
    // is a plain string compare. So the buttons' spelling it is; with the
    // log's, the filter works but no button lights.
    std::string system = gameState_.system();
    for (char& c : system) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!system.empty() && system != logSystem_) {
        logSystem_ = system;
        if (system != currentSystem_) {
            currentSystem_ = system;
            analyzer_.setSystemFilter(system);
            std::cout << "\nSystem from Game.log: " << system << std::endl;
            if (gui_) {
                gui_->setCurrentSystem(system);
                if (lastRecognizedSignature_ > 0) {
                    auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
                    gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
                }
            }
        }
    }
    // Where exactly, for the header, the scan log and the ore filter: the
    // place the log names, with the ore tables' location under it when
    // they have one (GamePlaces). A system button pressed by hand lifts
    // the place filter until the log names a new place - the button is
    // the wider choice, and the player made it on purpose.
    {
        // A place set by hand wins over the log, and over the system the log
        // last named with it: someone who says "I am at Lyria" has said
        // Stanton too. Nothing else about the log is switched off - the ship,
        // the seat and the system all keep coming from it.
        const bool byHand = !placeOverride_.empty();
        const GamePlace place = byHand
            ? placeByLogName(placeOverride_, gameState_.system())
            : placeByLogName(gameState_.place(), gameState_.system());
        // A place that knows its own system settles the question, by hand or
        // not: "Nyx - Bloom" is not a place, and the half that is certain is
        // the one the tables can name.
        const std::string shownSystem = !place.system.empty() ? place.system : system;
        const std::string pretty = shownSystem.empty()
            ? std::string()
            : shownSystem.substr(0, 1) + toLowerRest(shownSystem);
        std::string line;
        if (place.known()) {
            line = pretty.empty() ? place.name : pretty + " - " + place.name;
        } else if (!pretty.empty()) {
            // Out in open space between the bodies - which is what a route
            // starting from the system itself means. The system is all the
            // log says, and saying it beats the line vanishing; when the log
            // has actually said "open space", the line says so too.
            line = gameState_.inSpace() ? pretty + " - Open space" : pretty;
        }
        if (byHand && !line.empty()) line += " (set manually)";
        const std::string ore = place.known() ? place.oreLocation : std::string();
        if (line != placeLine_ || ore != placeOre_) {
            placeLine_ = line;
            placeOre_ = ore;
            scan_->setLogPlace(line);
            analyzer_.setPlaceFilter(ore);
            // The ground under the ship: a body, open space, or no idea. A
            // station or a belt the tables know counts as space too.
            using Ground = SignatureAnalyzer::Ground;
            const Ground ground = place.known()
                ? (place.surface == 1 ? Ground::Surface : place.surface == 0 ? Ground::Space : Ground::Unknown)
                : (gameState_.inSpace() ? Ground::Space : Ground::Unknown);
            analyzer_.setGround(ground);
            if (gui_) {
                gui_->setPlace(line, analyzer_.placeFilter(), ground == Ground::Space);
                // ...and the place itself, for the gear's shops, nearest first
                gearPlace_ = place.known() ? place.name : std::string();
                gearSystem_ = pretty;
                gui_->setWhereAt(gearPlace_, gearSystem_);
                handGearOrder();
                if (lastRecognizedSignature_ > 0) {
                    auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
                    gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
                }
            }
            std::cout << "\nYou are at: " << (line.empty() ? "unknown" : line)
                      << (analyzer_.placeFilter().empty() ? "" : " - ores of " + analyzer_.placeFilter() + " only")
                      << std::endl;
        }
    }
    // The quantum route plotted, for the Aaron Halo (MinerGUI::setRoute):
    // where it set off and where it goes, as places
    if (gui_) {
        const GamePlace start = placeByLogName(gameState_.routeStart(), gameState_.system());
        const GamePlace end = gameState_.target().empty() ? GamePlace()
                                                          : placeByDestination(gameState_.target(), gameState_.system());
        gui_->setRoute(start.known() ? start.name : std::string(), end.known() ? end.name : std::string());
    }
    const std::string label = gameState_.shipLabel();
    const std::string model = GameState::model(gameState_.ship());
    scan_->setLogShip(model, label, GameState::maker(model) == "Drake");
    // Hulls lost with the player in them, before the ship: a replacement the
    // same lines name is then already waited for
    const auto& lost = gameState_.lostHulls();
    if (lost.size() < lostHullsPassed_) lostHullsPassed_ = 0;   // a new log, read from the start
    for (; lostHullsPassed_ < lost.size(); lostHullsPassed_++) {
        if (gui_) gui_->shipLost(lost[lostHullsPassed_]);
    }
    if (gui_) gui_->setShip(model, gameState_.inSeat(), gameState_.hull());
    // Sat down, got up, boarded another: the loadout is read on the next look,
    // live mode on or not. Left to the looks alone, a ship change made with
    // live mode off could leave the last ship's reading settled for good.
    scan_->noteLogSeat(model, gameState_.hull(), gameState_.inSeat(), gameState_.boardings());
    const std::string announce = label.empty() ? "" : label + (gameState_.inSeat() ? "" : " (not seated)");
    if (announce != logShipLabel_) {
        logShipLabel_ = announce;
        std::cout << "\nShip from Game.log: " << (announce.empty() ? "unknown" : announce) << std::endl;
    }
}

// The REFINEMENT CENTER, read off the screen (see Application.h). Not tied
// to live mode: the player at a refinery has put the ship away, and the
// terminal is the one thing on screen worth reading then.
void Application::lookForRefinery() {
    if (!gui_) return;
    RefineryReading read;
    std::string station;
    long long taken = 0;
    if (refineryGlance_.takeFinished(read, station, taken)) takeRefineryScreen(read, station, taken);
    const auto now = std::chrono::steady_clock::now();
    RefineryAsk ask;
    ask.station = refineryStationAt(gameState_.place(), gameState_.system());
    ask.inSeat = gameState_.inSeat();
    ask.mining = now - scan_->rockPanelAt() < std::chrono::seconds(30);
    ask.wantNotice = refineryOrders_.anyDone((long long)std::time(nullptr));
    ask.atTerminal = now - refineryLastPanels_ < std::chrono::seconds(30);
    const RefineryLook outcome = refineryGlance_.look(ask);
    if (outcome.away) refineryOrders_.take(outcome.station, RefineryReading(), outcome.taken);
}

// What two readings in a row agree on goes into the book (RefineryOrderBook::take)
void Application::takeRefineryScreen(const RefineryReading& read, const std::string& placed, long long taken) {
    // The station the terminal names itself, else the one the log has the
    // player at; neither, and there is nowhere to put an order
    // ...and with no panel up to name it, the station of the last panels
    std::string station = refineryStationNamed(read.stationText, gameState_.system());
    if (station.empty() && !refineryLastStation_.empty() &&
        std::chrono::steady_clock::now() - refineryLastPanels_ < std::chrono::seconds(30)) {
        station = refineryLastStation_;
    }
    if (station.empty()) station = placed;
    if (!station.empty() && !read.panels.empty()) {
        refineryLastStation_ = station;
        refineryLastPanels_ = std::chrono::steady_clock::now();
    }
    if (station.empty()) {
        refineryOrders_.take("", RefineryReading(), taken);
        return;
    }
    shareRefineryScreen(read, station, taken);
    const size_t had = refineryOrders_.orders().size();
    const bool changed = refineryOrders_.take(station, read, taken);
    keepRefineryNotes(refineryOrders_.takeNotes());
    if (!changed) return;
    refineryOrders_.save(AppPaths::file(REFINERY_ORDERS_FILE));
    if (refineryOrders_.orders().size() < had) {
        std::cout << "\n[refinery] " << station << ": " << (had - refineryOrders_.orders().size())
                  << " work order(s) collected" << std::endl;
    }
    if (refineryOrders_.orders().size() > had) {
        const RefineryOrder& added = refineryOrders_.orders().back();
        std::cout << "\n[refinery] " << station << ": work order " << added.number << ",";
        for (const RefineryOrder::Part& p : added.parts) {
            std::cout << " " << p.material << " " << p.quality << " -> " << p.yield << " cSCU";
        }
        std::cout << ", done in " << (added.due - taken) << " s" << std::endl;
    }
    pushRefineryOrders();
}

// ---- refinery workloads (RefineryShare) ---------------------------------

std::vector<std::string> Application::installHeader() const {
    if (installId_.empty()) return {};
    return {"X-Bedrock-Install: " + installId_};
}

std::string Application::refineryServiceUrl() {
    if (const char* dev = DEV_ENV("SCMINER_REFINERY_URL")) return dev;
#ifdef BEDROCK_REFINERY_URL
    return BEDROCK_REFINERY_URL;
#else
    return "";
#endif
}

void Application::setRefineryShare(bool on) {
    refineryShare_ = on;
    if (!on) shareQueue_.clear();
    saveRegionConfig();
    if (gui_) gui_->setRefineryShare(refineryShare_, !refineryServiceUrl().empty());
}

// What a reading of the terminal says of its station's workload, and of an
// order being set up: like the orders themselves, taken only where two
// readings in a row agree - one misread figure never stands - and then shown
// and, where the player shares, sent
void Application::shareRefineryScreen(const RefineryReading& read, const std::string& station, long long taken) {
    if (read.capacity >= 0 || read.workload >= 0) {
        const WorkloadReading now{read.capacity, read.workload, taken};
        const auto last = workloadLast_.find(station);
        const bool agrees = last != workloadLast_.end() && last->second.capacity == now.capacity &&
                            last->second.workload == now.workload && taken - last->second.at <= 120;
        workloadLast_[station] = now;
        if (agrees) {
            workloadMine_[station] = now;
            pushRefineryWorkloads();
            WorkloadReading& sent = workloadSent_[station];
            if (RefineryShare::worthSending(now.capacity, now.workload, taken, sent.capacity, sent.workload, sent.at)) {
                RefineryShare::Reading r;
                r.station = station;
                r.observed = taken;
                r.capacity = now.capacity;
                r.workload = now.workload;
                if (queueRefineryShare(r)) sent = now;
            }
        }
    }
    for (const RefineryOrderPanel& panel : read.panels) {
        if (panel.kind != RefineryKind::Setup || !panel.whole()) continue;
        RefineryShare::Reading r;
        r.station = station;
        r.observed = taken;
        r.capacity = read.capacity;
        r.workload = read.workload;
        for (const RefiningMethod& m : REFINING_METHODS) {
            if (panel.method == m.name) r.method = m.key;
        }
        r.cost = panel.cost;
        r.seconds = panel.seconds;
        std::string key = station + "|" + std::to_string(panel.number) + "|" + r.method + "|" +
                          std::to_string(std::llround(panel.cost)) + "|" + std::to_string(panel.seconds);
        for (const RefineryOrderRow& row : panel.rows) {
            if (row.inert || row.material.empty() || row.quantity <= 0 || row.yield < 0) continue;
            r.items.push_back({row.material, row.quantity, row.quality, row.yield});
            key += "|" + row.material + ":" + std::to_string(row.quantity) + ":" + std::to_string(row.yield);
        }
        if (r.method.empty() || r.items.empty()) continue;
        const bool agrees = key == quoteLast_;
        quoteLast_ = key;
        if (!agrees || quotesSent_.count(key)) continue;
        if (queueRefineryShare(r)) quotesSent_.insert(key);
    }
}

bool Application::queueRefineryShare(const RefineryShare::Reading& reading) {
    if (!refineryShare_ || refineryServiceUrl().empty()) return false;
    if (library_.channel().rfind("LIVE", 0) != 0) return false;   // the PTU's refineries are not LIVE's
    shareQueue_.push_back(RefineryShare::body(reading, BEDROCK_VERSION, library_.liveGameVersion(), "LIVE"));
    while (shareQueue_.size() > 200) shareQueue_.pop_front();
    return true;
}

// The reports, one at a time off the UI thread - again a minute later where
// the service could not be reached or was busy - and, while the Refinery
// window is open, every five minutes what the service knows of each station
void Application::serviceRefineryShare() {
    const std::string base = refineryServiceUrl();
    if (base.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (shareJob_.valid() && shareJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const int status = shareJob_.get();
        if (status == 0 || status >= 500) {
            if (refineryShare_) shareQueue_.push_front(shareSending_);
            shareRetryAt_ = now + std::chrono::minutes(1);
        } else if (status == 429) {
            // enough of this station - or from this install, or of readings
            // that did not fit - for now: a fresher reading comes along, so
            // this one goes, and the rest wait
            shareRetryAt_ = now + std::chrono::minutes(10);
        } else if (status >= 400) {
            std::cout << "\nRefinery workloads: the service turned a report down (" << status << ")" << std::endl;
        }
        shareSending_.clear();
    }
    if (!shareJob_.valid() && refineryShare_ && !shareQueue_.empty() && now >= shareRetryAt_) {
        shareSending_ = shareQueue_.front();
        shareQueue_.pop_front();
        std::vector<std::string> headers = installHeader();
        headers.push_back("Content-Type: application/json");
        shareJob_ = std::async(std::launch::async, [url = base + "/v1/refinery/observations", body = shareSending_,
                                                    headers]() {
            HttpAnswer answer;
            httpsPost(url, headers, body, answer, 64 * 1024);
            return answer.status;
        });
    }
    if (stationsJob_.valid() && stationsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto answer = stationsJob_.get();
        if (answer.first.empty()) {
            stationsShared_ = std::move(answer.second);
            pushRefineryWorkloads();
        } else {
            std::cout << "\nRefinery workloads: " << answer.first << std::endl;
        }
    }
    const bool asked = stationsAsked_.time_since_epoch().count() != 0;
    if (!stationsJob_.valid() && gui_ && gui_->refineryShown() &&
        (!asked || now - stationsAsked_ >= std::chrono::minutes(5))) {
        stationsAsked_ = now;
        stationsJob_ = std::async(std::launch::async, [url = base + "/v1/refinery/stations", headers = installHeader()]() {
            std::string body, why;
            std::map<std::string, RefineryShare::StationState> states;
            if (httpsGet(url, headers, body, why, 1024 * 1024)) RefineryShare::readStations(body, states, why);
            return std::make_pair(why, states);
        });
    }
}

// The window's figures: each station's newest - the player's own reading or
// the service's
void Application::pushRefineryWorkloads() {
    if (!gui_) return;
    std::map<std::string, MinerGUI::RefineryWorkload> out;
    for (const auto& [name, s] : stationsShared_) {
        out[name] = {s.capacity, s.workload, s.observed, s.sources, false};
    }
    for (const auto& [name, w] : workloadMine_) {
        const auto it = out.find(name);
        if (it == out.end() || w.at >= it->second.observed) out[name] = {w.capacity, w.workload, w.at, 1, true};
    }
    gui_->setRefineryWorkloads(std::move(out));
}

// Each work order read whole - method, bill, time, what went in and what
// comes out - goes to the UEX report once; reportRefineryJob decides whether
// the player reports at all (opted in, agreed before the order was placed).
// Only once the player has left the terminal: a long list has been scrolled
// through by then, and the order has all its parts.
void Application::handRefineryOrdersToUex() {
    const auto now = std::chrono::steady_clock::now();
    if (now - refineryHandCheck_ < std::chrono::seconds(5)) return;
    refineryHandCheck_ = now;
    if (refineryGlance_.panelUp() || now - refineryLastPanels_ < std::chrono::seconds(10)) return;
    bool handed = false;
    for (size_t i = 0; i < refineryOrders_.orders().size(); i++) {
        const RefineryOrder& order = refineryOrders_.orders()[i];
        if (order.handed > 0 || !order.complete()) continue;
        UexRefineryJob job;
        job.station = order.station;
        job.number = order.number;
        job.method = order.method;
        job.cost = order.cost;
        job.seconds = order.seconds;
        job.placed = order.placed;
        for (const RefineryOrder::Part& p : order.parts) job.items.push_back({p.material, p.quantity, p.yield});
        reportRefineryJob(job);
        handed |= refineryOrders_.markHanded(i, (long long)std::time(nullptr));
        keepRefineryNotes({"work order " + std::to_string(order.number) + " at " + order.station + " goes to UEX" +
                           (uexReportSince_ > 0 ? "" : " - but reporting is off")});
    }
    if (handed) refineryOrders_.save(AppPaths::file(REFINERY_ORDERS_FILE));
}

void Application::keepRefineryNotes(const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    std::error_code ec;
    const std::filesystem::path folder = AppPaths::data() / "debug";
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / "refinery_jobs.txt";
    // past 256 KB, the newer half kept
    const auto size = std::filesystem::file_size(file, ec);
    if (!ec && size > 256 * 1024) {
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const size_t from = text.find('\n', text.size() / 2);
        std::ofstream(file, std::ios::binary | std::ios::trunc)
            << (from == std::string::npos ? std::string() : text.substr(from + 1));
    }
    std::ofstream out(file, std::ios::app);
    const std::string when = timeStamp("%Y-%m-%d %H:%M:%S");
    for (const std::string& line : lines) out << when << "  " << line << "\n";
}

void Application::pushRefineryOrders() {
    if (gui_) gui_->setRefineryOrders(refineryOrders_.sorted());
}

// ---------------------------------------------------------------------------
// Plugins (PluginHost.h): found in the data folder and started once the
// window is up, fed the live log through the library's reading of it and
// the older logs a while every round, and put in from the server's list
// when the player asks for one in the Plugins window.
// ---------------------------------------------------------------------------
void Application::setUpPlugins() {
    const std::filesystem::path data = AppPaths::data();
#ifdef BEDROCK_DEV_TOOLS
    const bool acceptUnsigned = true;    // a developer runs his own as he writes them
#else
    const bool acceptUnsigned = false;
#endif
    plugins_ = std::make_unique<Plugins::Host>(data / "plugins", data / "plugin_data", BEDROCK_VERSION,
                                               Plugins::trustedKeys(), acceptUnsigned);
    if (!library_.gameLog().empty()) plugins_->setGameFolder(library_.gameLog().parent_path());
    plugins_->load();
    library_.setLiveSink([this](std::string_view line) {
        if (plugins_) plugins_->liveLine(line);
    });
    pluginIndexUrl_ = Plugins::indexUrlBeside(updateUrl_.empty() ? std::string(BEDROCK_UPDATE_URL) : updateUrl_);
#ifdef BEDROCK_DEV_TOOLS
    // ...or a list of a developer's own, an address or a file
    if (const char* own = std::getenv("SCMINER_PLUGIN_INDEX")) pluginIndexUrl_ = own;
#endif
    for (const auto& info : plugins_->found()) {
        const Plugins::Plugin* p = plugins_->plugin(info.id);
        std::cout << "[plugins] " << info.id << " " << info.version << ": "
                  << (p && p->running() ? std::string("running") : info.problem.empty() && p ? p->error() : info.problem)
                  << std::endl;
    }
    if (!gui_) return;
    gui_->setPluginCallbacks(
        [this](const std::string& id, Plugins::UiState& state) { return pluginView(id, state); },
        [this](const std::string& id, const std::string& action) { pluginAction(id, action); });
    pushPlugins();
}

MinerGUI::PluginView Application::pluginView(const std::string& id, Plugins::UiState& state) {
    MinerGUI::PluginView view;
    Plugins::Plugin* p = plugins_ ? plugins_->plugin(id) : nullptr;
    if (!p) return view;
    // the plugin stopped: why, and the way to start it again (the list of
    // plugins hears of it with the next round - see servicePlugins)
    if (state.clicked == "bedrock:restart") {
        state.clicked.clear();
        plugins_->restart(id);
        p = plugins_->plugin(id);   // a new one
        if (!p) return view;
    }
    if (p->running()) view.items = plugins_->draw(id, state);
    // ...and what a click in it put on the clipboard
    const std::string copied = p->takeCopy();
    if (!copied.empty() && gui_) gui_->copyText(copied);
    view.title = p->title();
    view.subtitle = p->subtitle();
    if (!p->running()) {
        using Plugins::UiItem;
        UiItem why;
        why.kind = UiItem::Kind::Text;
        why.tone = UiItem::Tone::Bad;
        why.text = p->info().problem.empty() ? "STOPPED - " + p->error() : p->info().problem;
        view.items = {why};
        if (p->info().problem.empty()) {
            UiItem again;
            again.kind = UiItem::Kind::Buttons;
            again.optionIds = {"bedrock:restart"};
            again.options = {"Start again"};
            again.kinds = {"primary"};
            view.items.push_back(again);
        }
    }
    return view;
}

void Application::pushPlugins() {
    if (!plugins_ || !gui_) return;
    std::vector<MinerGUI::PluginEntry> list;
    for (const auto& info : plugins_->found()) {
        MinerGUI::PluginEntry e;
        e.id = info.id;
        e.name = info.name;
        e.version = info.version;
        e.description = info.description;
        e.installed = true;
        e.problem = info.problem;
        const Plugins::Plugin* p = plugins_->plugin(info.id);
        e.running = p && p->running();
        if (p && !e.running && info.problem.empty()) e.error = p->error();
        int done = 0, all = 0;
        if (plugins_->readingHistory(info.id, &done, &all)) {
            e.historyDone = done;
            e.historyAll = all;
        }
        list.push_back(e);
    }
    for (const auto& offer : pluginOffers_) {
        auto at = std::find_if(list.begin(), list.end(), [&](const MinerGUI::PluginEntry& e) { return e.id == offer.id; });
        if (at == list.end()) {
            MinerGUI::PluginEntry e;
            e.id = offer.id;
            e.name = offer.name;
            list.push_back(e);
            at = list.end() - 1;
        }
        at->offered = offer.version;
        at->offeredNeeds = offer.needs;
        at->offeredText = offer.description;
    }
    for (auto& e : list) e.busy = (e.id == pluginBusy_);
    gui_->setPlugins(std::move(list), pluginServerNote_);
}

void Application::fetchForPlugins(const std::string& id, const std::string& url) {
    pluginBusy_ = id;
    // A package goes with the install's number (none with stats=0), so that
    // the server counts plugin downloads by install; the list goes without.
    std::vector<std::string> headers = id.empty() ? std::vector<std::string>() : installHeader();
    pluginJob_ = std::async(std::launch::async, [id, url, headers]() {
        PluginFetch f;
        f.id = id;
        if (url.rfind("https://", 0) == 0) {
            f.ok = httpsGet(url, headers, f.body, f.why);
            return f;
        }
#ifdef BEDROCK_DEV_TOOLS
        // a developer's list on his own disk, and the plugins beside it
        std::ifstream in(std::filesystem::path(url), std::ios::binary);
        f.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        f.ok = !f.body.empty();
        if (!f.ok) f.why = "the file cannot be read";
#else
        f.why = "no https address";
#endif
        return f;
    });
}

void Application::pluginAction(const std::string& id, const std::string& action) {
    if (!plugins_) return;
    if (action == "refresh") {
        if (pluginJob_.valid()) return;
        if (pluginIndexUrl_.empty()) {
            pluginServerNote_ = "No plugin server in this build";
            pushPlugins();
            return;
        }
        pluginServerNote_ = "Checking for plugins...";
        fetchForPlugins("", pluginIndexUrl_);
    } else if (action == "install") {
        if (pluginJob_.valid()) return;
        const auto offer = std::find_if(pluginOffers_.begin(), pluginOffers_.end(),
                                        [&](const Plugins::Offer& o) { return o.id == id; });
        if (offer == pluginOffers_.end()) return;
        const std::string url = Plugins::packageUrl(pluginIndexUrl_, offer->url);
        if (url.empty()) return;
        std::cout << "[plugins] fetching " << id << " " << offer->version << std::endl;
        fetchForPlugins(id, url);
    } else if (action == "remove") {
        std::string why;
        if (plugins_->remove(id, why)) {
            std::cout << "[plugins] " << id << " removed" << std::endl;
        } else {
            pluginServerNote_ = "Could not remove it - " + why;
        }
    } else if (action == "restart") {
        plugins_->restart(id);
    }
    pushPlugins();
}

void Application::servicePlugins() {
    if (!plugins_) return;
    if (!library_.gameLog().empty()) plugins_->setGameFolder(library_.gameLog().parent_path());
    plugins_->tick(8);
    const auto now = std::chrono::steady_clock::now();
    // a plugin's window drawn anew when the plugin heard something - four
    // times a second at most: while the older logs are read, it hears
    // something every round
    if (gui_ && now - pluginDrawnAt_ > std::chrono::milliseconds(250)) {
        pluginDrawnAt_ = now;
        for (const auto& info : plugins_->found()) {
            if (plugins_->takeChanged(info.id)) gui_->pluginChanged(info.id);
        }
    }
    if (gui_) {
        std::string id, note;
        while (plugins_->takeNote(id, note)) gui_->setPluginNote(id, note);
    }
    // The list anew when what it shows moved: how far the older logs are,
    // and which plugins run - once a second at most
    {
        unsigned long long look = 0;
        for (const auto& info : plugins_->found()) {
            int done = 0, all = 0;
            plugins_->readingHistory(info.id, &done, &all);
            const Plugins::Plugin* p = plugins_->plugin(info.id);
            look = look * 31u + (unsigned long long)done * 7u + (unsigned long long)all + (p && p->running() ? 1u : 0u);
        }
        if (look != pluginPushedLook_ && now - pluginPushedAt_ > std::chrono::seconds(1)) {
            pluginPushedLook_ = look;
            pluginPushedAt_ = now;
            pushPlugins();
        }
    }
    // ...and what the server sent
    if (pluginJob_.valid() && pluginJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const PluginFetch f = pluginJob_.get();
        pluginBusy_.clear();
        if (f.id.empty()) {
            std::string why = f.why;
            std::vector<Plugins::Offer> offers;
            if (f.ok && Plugins::readIndex(f.body, offers, why)) {
                pluginOffers_ = offers;
                pluginServerNote_.clear();
                std::cout << "[plugins] the server offers " << offers.size() << std::endl;
            } else {
                pluginServerNote_ = "Plugin list not available - " + why;
                std::cout << "[plugins] list: " << why << std::endl;
            }
        } else {
            std::string id, why = f.why;
            if (f.ok && Plugins::unpack(f.body, plugins_->root(), plugins_->keys(), plugins_->acceptsUnsigned(), id, why)) {
                plugins_->load();
                plugins_->restart(id);
                pluginServerNote_.clear();
                std::cout << "[plugins] " << id << " installed" << std::endl;
            } else {
                pluginServerNote_ = "Not installed - " + why;
                std::cout << "[plugins] " << f.id << ": " << why << std::endl;
            }
        }
        pushPlugins();
    }
}

void Application::pollLibrary() {
    // A backup per 15 ms while the history is read, else the live log every
    // two seconds - quicker than anyone walks from a hand-in to the bench
    const auto now = std::chrono::steady_clock::now();
    const auto wait = std::chrono::milliseconds(library_.historyPending() ? 15 : 2000);
    if (libraryPushed_ && now - lastLibraryPoll_ < wait) return;
    lastLibraryPoll_ = now;
    if (library_.gameLog().empty() && ++libraryLooks_ % 15 == 0) {
        // The game may have started since - look again every half minute
        library_.setGameLog(locateGameLog());
    }
    const bool changed = library_.poll();
    if (changed || !libraryPushed_) pushLibrary();
    // ...and what was traded at the terminals, out of the same logs
    if (gui_ && library_.trades().changes() != tradesSeen_) {
        tradesSeen_ = library_.trades().changes();
        gui_->setTrades(library_.trades().trades());
    }
    // ...and the ranks out of the same logs: once they have all been read,
    // and again with every contract taken or finished
    {
        const bool known = !library_.gameLog().empty() && !library_.historyPending();
        const bool moved = library_.ranksChanged();
        if (known != ranksKnown_ || (known && moved) || !ranksPushed_) pushRanks();
        // ...and the contracts of the moment: one taken, one over. A game
        // that was closed takes its contracts with it and its log says
        // nothing of it, it only stops - so a log not written to for a
        // quarter of an hour has nothing running (the engine writes its
        // terrain statistics every ten minutes, whatever the player does).
        const long long written = library_.liveLogEpoch();
        const bool quiet = written > 0 && (long long)std::time(nullptr) - written > 15 * 60;
        if (moved || library_.runningChanges() != runningSeen_ || quiet != sessionQuiet_ || !contractsPushed_) {
            sessionQuiet_ = quiet;
            pushContracts();
        }
        if (gui_) gui_->tickContracts();
        if (gui_) gui_->tickUexOffer();
    }
    if (changed) {
        // A blueprint just received: the game's texts are rewritten for its
        // next start. A few seconds' grace, since hand-ins come several at once.
        gameTextsDue_ = true;
        gameTextsDueAt_ = now + std::chrono::seconds(5);
    }
    serviceGameTexts();
    keepGameTextsFresh();
    serviceUex();
    serviceUexReports();
    serviceDatarunner();
    serviceUexMarket();
    serviceUexShops();
    // The same log, read for where the player is and what they fly
    if (gameState_.gameLog() != library_.gameLog()) {
        gameState_.setGameLog(library_.gameLog());
        applyGameState();
    } else if (gameState_.poll()) {
        applyGameState();
    }
    // ...and which game it is of, by its header: a place's own rocks (Nyx
    // I's) count from 5.0 on - SignatureAnalyzer::setGameGeneration
    if (const int generation = library_.liveGameVersion() / 10000; generation != analyzer_.gameGeneration()) {
        analyzer_.setGameGeneration(generation);
        std::cout << "\nSignatures of game " << (generation > 0 ? std::to_string(generation) : std::string("unknown"))
                  << (generation > 0 && generation < 5 ? ": no place has rocks of its own" : "") << std::endl;
        if (gui_ && lastRecognizedSignature_ > 0) {
            auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
            gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
        }
    }
    if (gameState_.gadgetChanges() != gadgetChangesSeen_) applyGadget();
    // Ask again once an hour. Only from UpToDate: a check in flight must
    // not be started twice, a version already found must not be forgotten
    // before it is fetched, and a check that failed is the player's to
    // retry - retrying it behind their back is how a broken network turns
    // into a request every hour that nobody asked for.
    if (updater_.enabled() && updater_.state() == Updater::State::UpToDate &&
        now - lastUpdateCheck_ >= std::chrono::minutes(UPDATE_RECHECK_MINUTES)) {
        lastUpdateCheck_ = now;
        updater_.checkInBackground();
    }
    if (gui_) {
        gui_->setUpdateStatus(updater_.enabled() ? (int)updater_.state() : -1,
                              updater_.latest().version,
                              updater_.state() == Updater::State::Failed
                                  ? updater_.couldNotSay()
                                  : updater_.latest().notes,
                              updater_.percent());
    }
}

// The two boxes in the Blueprints window: the marks on the contracts, and the
// names - class and grade before the components', tracking signal and size
// before the missiles'. Either ticked, the game's texts are written at once
// and again whenever the library changes; both unticked, Bedrock's file is
// taken away and the game shows its own texts from its next start. Either
// way the choice is gametexts= and gamenames= in region_config.txt, written
// at once.
void Application::setGameTextsWanted(bool wanted) {
    gameTextsWanted_ = wanted;
    gameTextsChosen();
}

void Application::setGameNamesWanted(bool wanted) {
    gameNamesWanted_ = wanted;
    gameTextsChosen();
}

void Application::gameTextsChosen() {
    saveRegionConfig();
    if (gameTextsJob_.valid()) {
        // a write in flight finishes first - it must not land after a removal
        gameTextsNote_ = gameTextsJob_.get().note;
    }
    const bool wanted = gameTextsWanted_ || gameNamesWanted_;
    if (wanted) {
        gameTextsDue_ = true;
        gameTextsDueAt_ = std::chrono::steady_clock::now();
        gameTextsNote_ = library_.gameLog().empty() ? "Awaiting game folder" : "Writing...";
        serviceGameTexts();
    } else if (library_.gameLog().empty()) {
        gameTextsNote_.clear();
    } else {
        // ...on every channel there is: one written while the PTU was played
        // would otherwise stay there, out of sight, as old as it is
        const std::filesystem::path followed = library_.gameLog().parent_path();
        for (const auto& channel : GameTexts::channelsBeside(followed)) {
            const GameTexts::Outcome taken = GameTexts::remove(channel);
            if (channel == followed) gameTextsNote_ = taken.note;
        }
    }
    std::cout << "\nGame texts: contracts " << (gameTextsWanted_ ? "on" : "off") << ", names "
              << (gameNamesWanted_ ? "on" : "off") << (gameTextsNote_.empty() ? "" : " - " + gameTextsNote_)
              << std::endl;
    if (gui_) {
        gui_->setGameTexts(gameTextsWanted_, gameTextsNote_);
        gui_->setGameNames(gameNamesWanted_);
    }
}

// Collects a write that has finished, and starts one that is owed: only once
// the whole library is known (the old logs read), and off the UI thread - the
// first write of a game build reads the game's text out of Data.p4k.
void Application::serviceGameTexts() {
    if (gameTextsJob_.valid() && gameTextsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const GameTexts::Outcome done = gameTextsJob_.get();
        gameTextsNote_ = done.note;
        std::cout << "\nGame texts: " << done.note << std::endl;
        if (gui_) gui_->setGameTexts(gameTextsWanted_, gameTextsNote_);
    }
    if (!(gameTextsWanted_ || gameNamesWanted_) || !gameTextsDue_ || gameTextsJob_.valid()) return;
    if (library_.gameLog().empty() || library_.historyPending()) return;
    if (std::chrono::steady_clock::now() < gameTextsDueAt_) return;
    gameTextsDue_ = false;
    std::vector<bool> owned(CRAFT_BLUEPRINT_COUNT);
    for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) owned[b] = library_.owned(b);
    const std::filesystem::path channel = library_.gameLog().parent_path();
    const bool marks = gameTextsWanted_, names = gameNamesWanted_;
    gameTextsJob_ = std::async(std::launch::async,
                               [channel, owned, marks, names]() { return GameTexts::write(channel, owned, marks, names); });
}

// UEX's API, at the address its documentation gives (api.uexcorp.space answers too)
static const char* const UEX_API = "https://api.uexcorp.uk/2.0/";

// Bedrock's app token for UEX: uex_token.dat beside the program, which the
// build writes out of uex_token.txt and a release carries (CMakeLists.txt).
// Read once; "" without the file - a build that asks UEX anonymously.
static std::string uexToken() {
    static const std::string token = [] {
        std::ifstream in(AppPaths::program() / "uex_token.dat");
        std::string line;
        if (!in || !std::getline(in, line)) return std::string();
        return UexReports::unmixAppToken(line);
    }();
    return token;
}

// How long ago a file was written, in seconds; -1 where there is none
static long long secondsSinceWritten(const std::filesystem::path& file) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) return -1;
    const auto since = std::filesystem::file_time_type::clock::now() - written;
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(since).count();
}

static std::string wholeFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Written beside it and moved into place: a run that ends halfway leaves the
// list that was kept, not half a new one
static void keepFile(const std::filesystem::path& file, const std::string& bytes) {
    const std::filesystem::path fresh = file.string() + ".new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out.write(bytes.data(), (std::streamsize)bytes.size());
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(fresh, file, ec);
}

// The Ore Finder's prices, off the UI thread: two lists of a megabyte each,
// fetched and read. Each is the one kept in uex\ while that is young enough -
// the price list for half an hour, the terminal list for a week - and UEX's
// after that. One that cannot be fetched leaves the one kept in place,
// however old: an old price says its age, a missing one says nothing.
Application::UexOutcome Application::loadUex(const std::filesystem::path& folder) {
    static const std::string API = UEX_API;
    UexOutcome out;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path pricesFile = folder / "prices.json";
    const std::filesystem::path terminalsFile = folder / "terminals.json";
    std::vector<std::string> headers;
    if (const std::string token = uexToken(); !token.empty()) headers.push_back("Authorization: Bearer " + token);
    const long long now = (long long)std::time(nullptr);
    // A token UEX no longer takes is answered 403, "Invalid bearer token.",
    // where the same question without one is answered in full - so a token
    // withdrawn must not cost every Bedrock its prices: asked again without
    auto ask = [&](const char* endpoint, std::string& body, std::string& why) {
        if (httpsGet(API + endpoint, headers, body, why)) return true;
        if (headers.empty() || (why != "Server responded 401" && why != "Server responded 403")) return false;
        out.tokenRefused = true;
        headers.clear();
        why.clear();
        return httpsGet(API + endpoint, headers, body, why);
    };

    // The terminals first: the prices are read against them for the places
    std::string terminals = wholeFile(terminalsFile);
    const long long terminalsAge = secondsSinceWritten(terminalsFile);
    if (terminals.empty() || terminalsAge < 0 || terminalsAge > 7ll * 24 * 3600) {
        std::string body, why;
        JsonValue answer;
        bool fetched = ask("terminals", body, why);
        if (fetched && (!parseJson(body, answer, why) || answer.textOr("status") != "ok")) {
            fetched = false;
            if (why.empty()) why = "UEX responded: " + answer.textOr("status", "nothing");
        }
        if (fetched) {
            terminals = std::move(body);
            keepFile(terminalsFile, terminals);
        } else if (terminals.empty()) {
            out.placesNote = "No terminal list: " + why;
        }
    }

    const long long pricesAge = secondsSinceWritten(pricesFile);
    auto prices = std::make_shared<UexPrices>();
    std::string why;
    if (pricesAge >= 0 && pricesAge < UEX_PRICES_MINUTES * 60 &&
        prices->read(wholeFile(pricesFile), terminals, now, why)) {
        out.prices = prices;
        out.fetchedAt = now - pricesAge;
        return out;
    }
    std::string body;
    why.clear();
    if (ask("commodities_prices_all", body, why) && prices->read(body, terminals, now, why)) {
        keepFile(pricesFile, body);
        out.prices = prices;
        out.fetchedAt = now;
        return out;
    }
    out.note = why;
    if (pricesAge >= 0) {
        auto kept = std::make_shared<UexPrices>();
        std::string skip;
        if (kept->read(wholeFile(pricesFile), terminals, now, skip)) {
            out.prices = kept;
            out.fetchedAt = now - pricesAge;
        }
    }
    return out;
}

// Collects an answer that has come, and asks when the Ore Finder is open and
// its prices are half an hour old - or there are none yet
void Application::serviceUex() {
    if (!gui_) return;
    if (!uexPushed_) {
        uexPushed_ = true;
        gui_->setUexPrices(uexWanted_, nullptr, 0, "");
    }
    if (uexJob_.valid() && uexJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexOutcome done = uexJob_.get();
        if (done.prices) {
            uexPrices_ = done.prices;
            uexFetchedAt_ = done.fetchedAt;
        }
        uexNote_ = done.note;
        std::cout << "\nUEX prices: "
                  << (uexPrices_ ? std::to_string(((long long)std::time(nullptr) - uexFetchedAt_) / 60) + " min old"
                                 : std::string("none"))
                  << (uexNote_.empty() ? "" : " - " + uexNote_)
                  << (done.placesNote.empty() ? "" : " - " + done.placesNote)
                  << (done.tokenRefused ? " (UEX turned the app token away; asked without it)" : "") << std::endl;
        gui_->setUexPrices(uexWanted_, uexPrices_, uexFetchedAt_, uexNote_);
    }
    if (!uexWanted_ || uexJob_.valid() || !gui_->plannerOpen()) return;
    const long long age = uexPrices_ ? (long long)std::time(nullptr) - uexFetchedAt_ : -1;
    if (uexPrices_ && age >= 0 && age < UEX_PRICES_MINUTES * 60) return;
    const auto now = std::chrono::steady_clock::now();
    if (uexTried_ && now - uexTriedAt_ < std::chrono::minutes(uexNote_.empty() ? 1 : UEX_RETRY_MINUTES)) return;
    uexTried_ = true;
    uexTriedAt_ = now;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    uexJob_ = std::async(std::launch::async, [folder]() { return loadUex(folder); });
}

// A list of UEX's, the one kept in `file` while it is younger than `maxAge`
// seconds, else fetched (and kept); the one kept, however old, where it
// cannot be. "" and `why` when there is neither.
static std::string uexListCached(const std::filesystem::path& file, const char* endpoint, long long maxAge,
                                 std::string& why) {
    std::string kept = wholeFile(file);
    const long long age = secondsSinceWritten(file);
    if (!kept.empty() && age >= 0 && age < maxAge) return kept;
    std::vector<std::string> headers;
    if (const std::string token = uexToken(); !token.empty()) headers.push_back("Authorization: Bearer " + token);
    std::string body;
    bool fetched = httpsGet(std::string(UEX_API) + endpoint, headers, body, why);
    if (!fetched && !headers.empty() && (why == "Server responded 401" || why == "Server responded 403")) {
        why.clear();
        fetched = httpsGet(std::string(UEX_API) + endpoint, {}, body, why);
    }
    JsonValue answer;
    if (fetched && (!parseJson(body, answer, why) || answer.textOr("status") != "ok")) {
        fetched = false;
        if (why.empty()) why = "UEX responded: " + answer.textOr("status", "nothing");
    }
    if (fetched) {
        keepFile(file, body);
        return body;
    }
    return kept;
}

std::string Application::uexTokenForProbe() { return uexToken(); }

std::string Application::uexMarketForProbe(const std::filesystem::path& folder, const std::string& ore) {
    const auto started = std::chrono::steady_clock::now();
    const UexMarketAnswer answer = loadUexMarket(folder, ore, nullptr);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    if (!answer.listings) return ore + ": no listings - " + answer.note + "\n";
    const long long now = (long long)std::time(nullptr);
    int buying = 0, selling = 0;
    for (const UexListing& listing : *answer.listings) (listing.buying ? buying : selling)++;
    std::string out = ore + ": " + std::to_string(buying) + " buying, " + std::to_string(selling) + " selling; " +
                      std::to_string(ms) + " ms\n";
    int shown = 0;
    for (const UexListing& listing : *answer.listings) {
        if (!listing.buying || shown >= 4) continue;
        shown++;
        out += "    buys " + (listing.quality > 0 ? "Q" + std::to_string(listing.quality) + " " : std::string()) +
               "for " + std::to_string((long long)listing.price) + " / " + listing.unit + ", posted " +
               std::to_string((now - listing.added) / 86400) + " d ago, renewed " +
               std::to_string((now - listing.renewed) / 86400) + " d ago\n";
    }
    return out;
}

// One question to UEX's API with Bedrock's app token, and once more without
// it where UEX turns the token away
static bool uexAsk(const std::string& path, std::string& body, std::string& why) {
    std::vector<std::string> headers;
    if (const std::string token = uexToken(); !token.empty()) headers.push_back("Authorization: Bearer " + token);
    if (httpsGet(std::string(UEX_API) + path, headers, body, why)) return true;
    if (headers.empty() || (why != "Server responded 401" && why != "Server responded 403")) return false;
    why.clear();
    return httpsGet(std::string(UEX_API) + path, {}, body, why);
}

// The marketplace's listings of one ore, both ways, off the UI thread - and
// the marketplace's items first where they are not known yet (kept a day)
Application::UexMarketAnswer Application::loadUexMarket(const std::filesystem::path& folder, const std::string& ore,
                                                       std::shared_ptr<const std::map<std::string, int>> items) {
    UexMarketAnswer out;
    out.ore = ore;
    std::string why;
    if (!items) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::string list =
            uexListCached(folder / "market_items.json", "marketplace_prices_averages_all", 24ll * 3600, why);
        auto read = std::make_shared<std::map<std::string, int>>();
        if (list.empty() || !UexMarket::readItems(list, *read, why)) {
            out.note = "Marketplace items: " + why;
            return out;
        }
        out.items = read;
        items = read;
    }
    const int item = UexMarket::itemFor(*items, ore);
    if (!item) {
        out.listings = std::make_shared<std::vector<UexListing>>();
        out.note = "No " + ore + " on the marketplace";
        return out;
    }
    const long long now = (long long)std::time(nullptr);
    auto listings = std::make_shared<std::vector<UexListing>>();
    for (const char* operation : {"buy", "sell"}) {
        std::string body;
        why.clear();
        if (!uexAsk("marketplace_listings?id_item=" + std::to_string(item) + "&operation=" + operation, body, why)) {
            out.note = why;
            return out;
        }
        std::vector<UexListing> read;
        if (!UexMarket::readListings(body, now, read, why)) {
            out.note = why;
            return out;
        }
        listings->insert(listings->end(), read.begin(), read.end());
    }
    out.listings = listings;
    return out;
}

// The player's conversations and notifications, with their key - off the UI
// thread. Null where UEX would not answer.
std::shared_ptr<UexThreads> Application::loadUexThreads(const std::string& key) {
    const std::string token = uexToken();
    std::vector<std::string> headers = {"secret-key: " + key};
    if (!token.empty()) headers.push_back("Authorization: Bearer " + token);
    std::string body, why;
    auto threads = std::make_shared<UexThreads>();
    if (!httpsGet(std::string(UEX_API) + "marketplace_negotiations", headers, body, why) ||
        !UexMarket::readNegotiations(body, *threads, why)) {
        std::cout << "\nUEX conversations: " << why << std::endl;
        return nullptr;
    }
    if (!threads->hashByListing.empty() && httpsGet(std::string(UEX_API) + "user_notifications", headers, body, why)) {
        UexMarket::readNotifications(body, *threads);
    }
    return threads;
}

// Collects listings that have come, and asks for the next ore wanted: the
// UEX tab's first, then the rock's good parts - one at a time, each ore
// again only after ten minutes
void Application::serviceUexMarket() {
    if (!gui_) return;
    if (uexMarketJob_.valid() && uexMarketJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexMarketAnswer done = uexMarketJob_.get();
        if (done.items) uexMarketItems_ = done.items;
        if (done.listings || !done.note.empty()) {
            gui_->setUexMarket(done.ore, done.listings, (long long)std::time(nullptr), done.note);
        }
    }
    // the player's conversations, with their key, while the UEX tab is up
    if (uexThreadsJob_.valid() && uexThreadsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto threads = uexThreadsJob_.get()) gui_->setUexThreads(std::move(threads));
    }
    if (uexWanted_ && !uexKey_.empty() && !uexKeyTrouble_ && !uexThreadsJob_.valid() &&
        !gui_->uexMarketWanted().empty() &&
        (!uexThreadsEver_ || std::chrono::steady_clock::now() - uexThreadsAsked_ > std::chrono::minutes(2))) {
        uexThreadsEver_ = true;
        uexThreadsAsked_ = std::chrono::steady_clock::now();
        const std::string key = uexKey_;
        uexThreadsJob_ = std::async(std::launch::async, [key]() { return loadUexThreads(key); });
    }
    if (!uexWanted_ || uexMarketJob_.valid()) return;
    const auto now = std::chrono::steady_clock::now();
    auto due = [&](const std::string& ore) {
        const auto asked = uexMarketAsked_.find(ore);
        return asked == uexMarketAsked_.end() || now - asked->second >= std::chrono::minutes(UEX_MARKET_MINUTES);
    };
    std::string next;
    const std::string tab = gui_->uexMarketWanted();
    if (!tab.empty() && due(tab)) next = tab;
    for (const std::string& ore : uexRockOres_) {
        if (next.empty() && due(ore)) next = ore;
    }
    if (next.empty()) return;
    uexMarketAsked_[next] = now;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    const auto items = uexMarketItems_;
    uexMarketJob_ = std::async(std::launch::async, [folder, next, items]() { return loadUexMarket(folder, next, items); });
}

// The shops of the gadgets, the laser heads and the modules, off the UI
// thread: three lists of UEX's (a tenth, a quarter and four tenths of a
// megabyte), each the one kept in uex\ while it is younger than half a day.
// A list that cannot be fetched is the one kept, however old; with one of
// them neither, nothing and why - a shop list short of a category would
// say its items are sold nowhere. keptOnly: the three kept, whatever their
// age, and UEX not asked - nothing where one of them is missing.
Application::UexShopsOutcome Application::loadUexShops(const std::filesystem::path& folder, bool keptOnly) {
    UexShopsOutcome out;
    out.keptOnly = keptOnly;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    auto shops = std::make_shared<UexShops>();
    for (const int category : {28, 29, 30}) {
        const std::string number = std::to_string(category);
        const std::string endpoint = "items_prices?id_category=" + number;
        const std::filesystem::path file = folder / ("items_prices_" + number + ".json");
        std::string why;
        const std::string list =
            keptOnly ? wholeFile(file) : uexListCached(file, endpoint.c_str(), UEX_SHOPS_HOURS * 3600ll, why);
        if (list.empty()) {
            out.note = keptOnly ? "none kept" : why.empty() ? "UEX sent nothing" : why;
            return out;
        }
        if (!shops->read(list, why)) {
            out.note = why;
            return out;
        }
        if (!why.empty()) out.note = why;   // not fetched: the one kept was read
    }
    out.shops = shops;
    return out;
}

// Collects the shops that have come, and asks for them from the start - not
// first when the Gear page opens, which then waited on UEX. First the
// lists kept from before, whatever their age and without asking UEX, so
// the page has them at once; then each list as uexListCached has it,
// fetched where it is half a day old; again once what came is half a day
// old - and after a failure every UEX_RETRY_MINUTES
void Application::handGearOrder() {
    if (!gui_) return;
    if (!uexShops_) {
        gui_->setGearOrder(nullptr);
        return;
    }
    gui_->setGearOrder(std::make_shared<const std::map<std::string, std::vector<ShopInOrder>>>(
        shopsInOrder(*uexShops_, gearSystem_, gearPlace_)));
}

void Application::serviceUexShops() {
    if (!gui_) return;
    if (uexShopsJob_.valid() && uexShopsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexShopsOutcome done = uexShopsJob_.get();
        if (done.keptOnly) {
            // what there was before: shown while the fresh ones are fetched,
            // and nothing said where none was kept
            uexShopsKeptRead_ = true;
            if (done.shops) {
                uexShops_ = done.shops;
                std::cout << "\nUEX shops: " << uexShops_->itemCount() << " items kept from before" << std::endl;
                gui_->setGearShops(uexShops_, "");
                handGearOrder();
            }
        } else {
            if (done.shops) uexShops_ = done.shops;
            uexShopsNote_ = done.note;
            std::cout << "\nUEX shops: "
                      << (uexShops_ ? std::to_string(uexShops_->itemCount()) + " items" : std::string("none"))
                      << (uexShopsNote_.empty() ? "" : " - " + uexShopsNote_) << std::endl;
            gui_->setGearShops(uexShops_, uexShopsNote_);
            handGearOrder();
        }
    }
    if (!uexWanted_ || uexShopsJob_.valid()) return;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    if (!uexShopsKeptRead_) {
        uexShopsJob_ = std::async(std::launch::async, [folder]() { return loadUexShops(folder, true); });
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (uexShopsTried_) {
        const bool fresh = uexShops_ && uexShopsNote_.empty();
        if (now - uexShopsTriedAt_ < (fresh ? std::chrono::minutes(UEX_SHOPS_HOURS * 60)
                                            : std::chrono::minutes(UEX_RETRY_MINUTES))) {
            return;
        }
    }
    uexShopsTried_ = true;
    uexShopsTriedAt_ = now;
    uexShopsJob_ = std::async(std::launch::async, [folder]() { return loadUexShops(folder); });
}

// UEX's ids for what is reported, off the UI thread: its terminals,
// commodities and refining methods, each kept a week
std::shared_ptr<UexDirectory> Application::loadUexDirectory(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const long long week = 7ll * 24 * 3600;
    std::string why;
    const std::string terminals = uexListCached(folder / "terminals.json", "terminals", week, why);
    const std::string commodities = uexListCached(folder / "commodities.json", "commodities", week, why);
    const std::string methods = uexListCached(folder / "methods.json", "refineries_methods", week, why);
    auto directory = std::make_shared<UexDirectory>();
    if (terminals.empty() || commodities.empty() || methods.empty() ||
        !directory->read(terminals, commodities, methods, why)) {
        std::cout << "\nUEX reports: UEX's lists could not be read - " << why << std::endl;
        return nullptr;
    }
    return directory;
}

void Application::askUexDirectory() {
    if (uexDirectory_ || uexDirectoryJob_.valid()) return;
    const auto now = std::chrono::steady_clock::now();
    if (uexDirectoryAsked_ && now - uexDirectoryTried_ <= std::chrono::minutes(UEX_RETRY_MINUTES)) return;
    uexDirectoryAsked_ = true;
    uexDirectoryTried_ = now;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    uexDirectoryJob_ = std::async(std::launch::async, [folder]() { return loadUexDirectory(folder); });
}

void Application::readUexReported() {
    if (uexReportedRead_) return;
    uexReportedRead_ = true;
    std::ifstream in(AppPaths::data() / "uex" / "reported.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        uexReported_.insert(line);
        // a Datarunner report's rows: UEX's five minutes, over a restart
        Datarunner::Sent sent;
        if (Datarunner::readSentKey(line, sent)) datarunnerSent_.push_back(sent);
    }
}

// Done, and so never again - kept for the next start. The newest 600 are
// kept; anything older is long past the two minutes a trade is reported in.
void Application::rememberUexReported(const std::string& key) {
    readUexReported();
    if (!uexReported_.insert(key).second) return;
    std::error_code ec;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    std::filesystem::create_directories(folder, ec);
    std::ofstream out(folder / "reported.txt", std::ios::app);
    out << key << "\n";
    if (uexReported_.size() > 1200) {
        // written anew with the newest only: the file is in order, the set is not
        std::ifstream in(folder / "reported.txt");
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        in.close();
        if (lines.size() > 600) lines.erase(lines.begin(), lines.end() - 600);
        std::ofstream fresh(folder / "reported.txt", std::ios::trunc);
        for (const std::string& kept : lines) fresh << kept << "\n";
        uexReported_ = std::set<std::string>(lines.begin(), lines.end());
    }
}

// The game's picture, whole, for the snapshot. The overlay frame would end
// up in it, so it steps aside.

void Application::reportRefineryJob(const UexRefineryJob& job) {
    if (uexReportSince_ <= 0 || uexKey_.empty() || uexKeyTrouble_) return;
    if (job.placed > 0 && job.placed < uexReportSince_) return;   // placed before the player agreed
    readUexReported();
    const std::string key = "refinery|" + job.key();
    if (uexReported_.count(key)) return;
    if (std::any_of(uexQueue_.begin(), uexQueue_.end(), [&](const UexPending& p) { return p.key == key; })) return;
    UexPending item;
    item.key = key;
    item.job = job;
    uexQueue_.push_back(std::move(item));
}

// Collects a report that has gone, and sends the next one - one at a time,
// off the UI thread, and only what can be told for certain: a job whose
// station is not exactly one of UEX's refineries is left out, and the window
// says why
void Application::serviceUexReports() {
    const auto now = std::chrono::steady_clock::now();
    if (uexSendJob_.valid() && uexSendJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexSent sent = uexSendJob_.get();
        if (sent.shot > 0) {
            if (sent.keyTrouble) uexKeyTrouble_ = true;
            if (sent.answered) uexAccountTrouble_ = sent.accountTrouble;
            if (!sent.user.empty()) uexUser_ = sent.user;
            datarunnerAnswered(sent);
            pushUexAccount();
            return;
        }
        if (sent.keyTrouble) {
            uexKeyTrouble_ = true;      // the same key again would only be turned away again
        } else if (!sent.key.empty()) {
            rememberUexReported(sent.key);
        }
        // the account as the last answer had it: refused, or taken again (a
        // report that did not get through says nothing of it)
        if (sent.answered) uexAccountTrouble_ = sent.accountTrouble;
        if (!sent.user.empty()) uexUser_ = sent.user;
        uexReportNote_ = sent.note;
        uexReportNoteAt_ = (long long)std::time(nullptr);
        std::cout << "\nUEX report: " << sent.note << std::endl;
        keepRefineryNotes({sent.note});
        pushUexAccount();
    }
    if (uexDirectoryJob_.valid() && uexDirectoryJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto directory = uexDirectoryJob_.get()) uexDirectory_ = std::move(directory);
    }
    if (uexQueue_.empty() || uexSendJob_.valid()) return;
    // What can no longer go: a refinery job once reporting is off, a
    // Datarunner report once the Datarunner is off - and anything without a
    // key UEX takes. A picture's report says so on its card.
    for (auto it = uexQueue_.begin(); it != uexQueue_.end();) {
        const bool off = uexKey_.empty() || uexKeyTrouble_ || (it->shot > 0 ? datarunnerSince_ <= 0 : uexReportSince_ <= 0);
        if (!off) {
            ++it;
            continue;
        }
        if (it->shot > 0) {
            UexSent dropped;
            dropped.shot = it->shot;
            dropped.note = uexKeyTrouble_ ? "UEX did not accept this key" : "Datarunner off";
            datarunnerAnswered(dropped);
        }
        it = uexQueue_.erase(it);
    }
    if (uexQueue_.empty()) return;
    const std::string token = uexToken();
    // A Datarunner picture's report: written when the player pressed Send
    if (uexQueue_.front().shot > 0) {
        UexPending item = std::move(uexQueue_.front());
        uexQueue_.pop_front();
        if (token.empty()) {
            UexSent none;
            none.shot = item.shot;
            none.note = "This build has no UEX app token";
            datarunnerAnswered(none);
            return;
        }
        const std::string key = uexKey_;
        const std::string url = std::string(UEX_API) + "data_submit/";
        uexSendJob_ = std::async(std::launch::async, [url, token, key, item]() {
            UexSent sent;
            sent.shot = item.shot;
            sent.rows = item.rows;
            HttpAnswer answer;
            const std::vector<std::string> headers = {"Authorization: Bearer " + token, "secret-key: " + key,
                                                      "Content-Type: application/json"};
            if (!httpsPost(url, headers, item.body, answer)) {
                sent.note = "Not sent (" + answer.why + ")";
                return sent;
            }
            sent.answered = true;
            sent.said = UexReports::verdict(answer.status, answer.body, &sent.user);
            sent.keyTrouble = UexReports::keyTrouble(answer.status, answer.body);
            sent.accountTrouble = UexReports::accountTrouble(answer.status, answer.body);
            sent.ids = UexReports::reportIds(answer.body);
            sent.duplicate = UexReports::duplicateNamed(answer.body);
            sent.note = sent.said;
            return sent;
        });
        return;
    }
    askUexDirectory();
    if (!uexDirectory_) return;
    UexPending item = std::move(uexQueue_.front());
    uexQueue_.pop_front();
    const UexDirectory& directory = *uexDirectory_;
    const bool production = DEV_ENV("SCMINER_UEX_DRYRUN") == nullptr;
    std::string why;
    const UexRefineryJob& job = item.job;
    const int terminal = directory.refineryTerminal(job.station, why);
    const int method = directory.method(job.method);
    std::vector<std::pair<int, UexRefineryJob::Item>> items;
    std::string missing;
    for (const UexRefineryJob::Item& part : job.items) {
        const int commodity = directory.commodity(part.material);
        if (!commodity) missing = part.material;
        else if (part.quantity >= 0 && part.yield >= 0) items.push_back({commodity, part});
    }
    const std::string label = "Refinery job at " + job.station;
    if (!terminal || !method || !missing.empty() || items.empty() || job.cost < 0 || job.seconds <= 0) {
        uexReportNote_ = label + ": Not reported (" +
                         (!terminal ? why : !method ? "Method unknown to UEX: " + job.method
                                    : !missing.empty() ? "Not listed on UEX: " + missing
                                                       : "Job not read completely") + ")";
        uexReportNoteAt_ = (long long)std::time(nullptr);
        keepRefineryNotes({uexReportNote_});
        rememberUexReported(item.key);
        pushUexAccount();
        return;
    }
    const std::string body = UexReports::refineryJob(terminal, method, job.cost, (job.seconds + 59) / 60, items, production);
    if (token.empty()) {
        uexReportNote_ = "This build has no UEX app token - Reporting unavailable";
        uexReportNoteAt_ = (long long)std::time(nullptr);
        uexQueue_.clear();
        pushUexAccount();
        return;
    }
    const std::string key = uexKey_;
    const std::string url = std::string(UEX_API) + "user_refineries_jobs_add/";
    const std::string itemKey = item.key;
    uexSendJob_ = std::async(std::launch::async, [url, token, key, body, label, itemKey]() {
        UexSent sent;
        sent.key = itemKey;
        HttpAnswer answer;
        const std::vector<std::string> headers = {"Authorization: Bearer " + token, "secret-key: " + key,
                                                  "Content-Type: application/json"};
        if (!httpsPost(url, headers, body, answer)) {
            sent.note = label + ": Not sent (" + answer.why + ")";
            return sent;
        }
        sent.answered = true;
        const std::string said = UexReports::verdict(answer.status, answer.body, &sent.user);
        sent.keyTrouble = UexReports::keyTrouble(answer.status, answer.body);
        sent.accountTrouble = UexReports::accountTrouble(answer.status, answer.body);
        if (sent.keyTrouble) sent.key.clear();   // not done: it goes again once the key is right
        sent.note = label + (said.empty() ? ": Accepted by UEX" : ": " + said);
        return sent;
    });
}

void Application::pushUexAccount() {
    if (!gui_) return;
    MinerGUI::UexAccount account;
    account.on = uexReportSince_ > 0;
    account.since = uexReportSince_;
    account.keyEnd = uexKey_.size() >= 4 ? uexKey_.substr(uexKey_.size() - 4) : std::string();
    account.keyTrouble = uexKeyTrouble_;
    account.accountTrouble = uexAccountTrouble_;
    account.note = uexReportNote_;
    account.noteAt = uexReportNoteAt_;
    account.user = uexUser_;
    account.hasToken = !uexToken().empty();
    account.keyChanges = uexKeyChanges_;
    account.datarunner = datarunnerSince_ > 0;
    account.datarunnerSince = datarunnerSince_;
    account.introOff = datarunnerIntroOff_;
    gui_->setUexAccount(account);
}

// What the player pressed on a UEX card: 1 a key pasted (the clipboard's
// text), 2 the key forgotten, 3 agreed - reporting on, 4 declined or switched
// off (the key stays), 5 the first start's offer made, whatever came of it
void Application::uexAccountAction(int action, const std::string& text) {
    const long long now = (long long)std::time(nullptr);
    if (action == 1) {
        std::string key = text;
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                  key.end());
        if (!UexReports::keyLooksRight(key)) {
            uexReportNote_ = "Not a UEX secret key. It must be 40 letters and digits.";
        } else if (!UexKey::save(key)) {
            uexReportNote_ = "The key could not be saved on this PC.";
        } else {
            uexKey_ = key;
            uexKeyTrouble_ = false;
            uexAccountTrouble_.clear();   // another key may be another account
            uexKeyChanges_++;
            uexReportNote_ = "Key saved";
        }
    } else if (action == 2) {
        UexKey::forget();
        uexKey_.clear();
        uexKeyTrouble_ = false;
        uexAccountTrouble_.clear();
        uexReportSince_ = 0;
        datarunnerSince_ = 0;
        uexQueue_.clear();
        saveRegionConfig();
        uexReportNote_ = "Key removed, reporting off";
    } else if (action == 3) {
        if (uexKey_.empty()) {
            uexReportNote_ = "Paste your UEX secret key first";
        } else {
            uexReportSince_ = now;
            saveRegionConfig();
            uexReportNote_ = "Reporting on";
        }
    } else if (action == 4) {
        const bool was = uexReportSince_ > 0;
        uexReportSince_ = 0;
        uexAccountTrouble_.clear();   // the next report, reporting on again, tells anew
        uexQueue_.clear();
        saveRegionConfig();
        uexReportNote_ = was ? "Reporting off" : (uexKey_.empty() ? "" : "Account connected, reporting off");
    } else if (action == 5) {
        // the first start's offer was made: not again
        uexOffered_ = true;
        saveRegionConfig();
        pushUexAccount();
        return;
    } else if (action == 8 || action == 9) {
        // the Datarunner's introduction: not to come up by itself any more,
        // or to come up again - nothing to tell
        datarunnerIntroOff_ = action == 8;
        saveRegionConfig();
        pushUexAccount();
        return;
    } else if (action == 6) {
        // UEX Datarunner on: the pictures taken from now on are read
        if (uexKey_.empty()) {
            uexReportNote_ = "Paste your UEX secret key first";
        } else {
            datarunnerSince_ = now;
            datarunnerFolder_.clear();   // looked at anew, what is there now left alone
            saveRegionConfig();
            uexReportNote_ = "Datarunner on";
        }
    } else if (action == 7) {
        datarunnerSince_ = 0;
        saveRegionConfig();
        uexReportNote_ = "Datarunner off";
    }
    uexReportNoteAt_ = now;
    std::cout << "\nUEX reports: " << uexReportNote_ << std::endl;
    pushUexAccount();
}

// A picture of Bedrock's own for the player to open, as UEX gets it: in the
// data folder's debug\uex_pictures, kept 30 days like the ones sent; "" where
// it could not be written
static std::filesystem::path keepPicture(const std::vector<unsigned char>& jpeg, const std::filesystem::path& folder,
                                         const std::string& stem) {
    if (jpeg.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && secondsSinceWritten(entry.path()) > Datarunner::OLDEST_SECONDS) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
    const std::filesystem::path shown = folder / (stem + "_bedrock.jpg");
    std::ofstream out(shown, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(jpeg.data()), (std::streamsize)jpeg.size());
    return out.good() ? shown : std::filesystem::path();
}

// UEX Datarunner: the game's ScreenShots folder, looked at every half second
// while it is on (watchScreenshots, from the main loop). A picture written
// since it was turned on, and since Bedrock started, is read off the UI
// thread once the game has finished writing it - its size the same for half
// a second -, and one of a commodity terminal waits for the player on the
// main window. A picture of anything else is left alone, and the foot of the
// main window says so for a minute. Where the game saves its pictures black
// (seen with HDR on), the picture Bedrock took of the game the moment the
// game's came is read instead. UEX's lists and game versions are asked for
// while it is on (serviceDatarunner), and a picture is matched to them as
// soon as they are in.
//
// The folder, from the main loop: every half second, so that Bedrock's own
// picture of the game is taken as close to the player's Print Screen as it
// can be. True where what the window shows changed.
bool Application::watchScreenshots() {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    // a picture read
    if (datarunnerReadJob_.valid() && datarunnerReadJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::shared_ptr<Datarunner::Shot> shot = datarunnerReadJob_.get();
        if (shot && !shot->rows.empty() && datarunnerSince_ > 0) {
            shot->id = datarunnerNextId_++;
            std::cout << "\nUEX Datarunner: " << shot->file.filename().string() << " - " << shot->rows.size()
                      << " rows read" << (shot->own ? " off Bedrock's own picture (the game's is black)" : "")
                      << std::endl;
            datarunnerShots_.push_back(std::move(shot));
            datarunnerNote_.clear();
            changed = true;
        } else if (shot && datarunnerSince_ > 0) {
            // ...and one without a terminal on it says so for a minute,
            // rather than nothing happening at all
            std::time_t taken = (std::time_t)shot->taken;
            std::tm local = {};
#ifdef _WIN32
            localtime_s(&local, &taken);
#else
            localtime_r(&taken, &local);
#endif
            char clock[8] = "";
            std::strftime(clock, sizeof clock, "%H:%M", &local);
            datarunnerNote_ = std::string("Screenshot ") + clock + "  \xC2\xB7  " +
                              (shot->black ? "The picture is black, nothing to read" : "No commodity terminal found");
            datarunnerNoteAt_ = now;
            std::cout << "\nUEX Datarunner: " << shot->file.filename().string() << " - no commodity terminal"
                      << (shot->own ? " (the game's picture is black, Bedrock's own read)"
                                    : shot->black ? " (the picture is black, and Bedrock took none of its own)" : "")
                      << std::endl;
            changed = true;
        }
    }
    if (datarunnerSince_ <= 0 || uexKey_.empty()) return changed;   // off: serviceDatarunner lets it all go
    // the folder, every half second
    if (now - datarunnerLooked_ >= std::chrono::milliseconds(500)) {
        datarunnerLooked_ = now;
        const std::filesystem::path folder = Datarunner::screenshotsBeside(library_.gameLog());
        if (folder != datarunnerFolder_) {
            datarunnerFolder_ = folder;
            datarunnerKnown_.clear();
            datarunnerGrowing_.clear();
            datarunnerToRead_.clear();
            datarunnerFrom_ = (std::max)(datarunnerSince_, (long long)std::time(nullptr));
        }
        std::error_code ec;
        if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                std::string ext = entry.path().extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") continue;
                const std::string name = entry.path().filename().string();
                if (datarunnerKnown_.count(name)) continue;
                // one from before: never read
                const auto written = entry.last_write_time(ec);
                if (ec) continue;
#ifdef _WIN32
                const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
                const auto system = std::chrono::file_clock::to_sys(written);
#endif
                const long long at =
                    (long long)std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
                if (at < datarunnerFrom_) {
                    datarunnerKnown_.insert(name);
                    continue;
                }
                // a new one: read once its size has stood for half a second.
                // Star Citizen can save it black (seen with HDR on), so
                // Bedrock takes its own picture of the game the moment it
                // first sees it - read instead where the game's is black
                const std::uintmax_t size = entry.file_size(ec);
                if (ec || size == 0) continue;
                auto growing = datarunnerGrowing_.find(name);
                if (growing == datarunnerGrowing_.end()) {
                    DatarunnerGrowing fresh;
                    fresh.size = size;
                    fresh.since = now;
                    if (!inScan_ && scan_->gameWindowFound()) fresh.own = scan_->grabGamePixels();
                    datarunnerGrowing_[name] = std::move(fresh);
                    continue;
                }
                if (growing->second.size != size) {
                    growing->second.size = size;
                    growing->second.since = now;
                    continue;
                }
                if (now - growing->second.since < std::chrono::milliseconds(500)) continue;
                Datarunner::Pixels own = std::move(growing->second.own);
                datarunnerGrowing_.erase(growing);
                datarunnerKnown_.insert(name);
                if ((int)datarunnerShots_.size() + (int)datarunnerToRead_.size() >= DATARUNNER_KEPT) {
                    std::cout << "\nUEX Datarunner: " << name << " not read - " << DATARUNNER_KEPT
                              << " pictures are waiting" << std::endl;
                    continue;
                }
                datarunnerToRead_.push_back({entry.path(), std::move(own)});
            }
        }
    }
    // the next picture, read off the UI thread with a reader of its own
    if (!datarunnerReadJob_.valid() && !datarunnerToRead_.empty()) {
        const std::filesystem::path file = datarunnerToRead_.front().first;
        const Datarunner::Pixels own = datarunnerToRead_.front().second;
        datarunnerToRead_.pop_front();
        // where Bedrock's own picture is the one read, it is written for the
        // player to open, as UEX would get it
        const std::filesystem::path pictures = AppPaths::data() / "debug" / "uex_pictures";
        DatarunnerEyes* eyes = &datarunnerEyes_;
        datarunnerReadJob_ = std::async(std::launch::async, [eyes, file, own, pictures]() {
            auto shot = eyes->readFile(file, own);
            if (shot && shot->own && !shot->rows.empty()) shot->shown = keepPicture(shot->jpeg, pictures, file.stem().string());
            return shot;
        });
    }
    return changed;
}

// The Datarunner at a commodity terminal without Print Screen. The game's
// log has a line of the terminal's whenever it is opened, its list fills or
// a trade is made (TradeBook::terminalUsedMs). From such a line on, Bedrock
// looks at the game - on foot, the game in view, not at a rock, no scan
// running - and reads each look off the UI thread with a reader of its own:
// one after the other while a terminal is in sight, every two seconds while
// none is. The rows a look shows as the one before it did, the same on the
// same list (Datarunner::steadyRowsOf), make a picture for the player to
// check - those of them no picture of that shop's list from the last half
// hour has (Datarunner::newRowsOf), and no look offered before, whether it
// was sent, closed or thrown away. Scrolling down the list with a pause
// now and then gives a picture for each part. A list the player has sent
// or put away is read no more while he stays at the terminal. Twenty
// seconds without a terminal in sight end the looking until the log has a
// terminal's line again. Nothing goes without Send. True where what the
// window shows changed.
bool Application::watchTerminal() {
    bool changed = false;
    const auto now = std::chrono::steady_clock::now();
    const bool on = datarunnerSince_ > 0 && !uexKey_.empty();
    // a look read
    if (terminalJob_.valid() && terminalJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        std::shared_ptr<Datarunner::Shot> look = terminalJob_.get();
        const bool sawTerminal = look && !look->rows.empty();
        if (sawTerminal) terminalSeen_ = now;
        if (on && sawTerminal && terminalLastLook_ && (int)datarunnerShots_.size() < DATARUNNER_KEPT &&
            !Datarunner::listDone(terminalDone_, look->shop, look->side)) {
            Datarunner::Shot steady = Datarunner::steadyRowsOf(*look, *terminalLastLook_);
            if (!steady.rows.empty() && uexDirectory_) {
                // UEX's commodity for each row: one read another way in
                // another look is the same
                Datarunner::Shot named = steady;
                Datarunner::match(named, *uexDirectory_,
                                  library_.trades().terminalAt(look->taken * 1000 + 999, 30ll * 60 * 1000));
                for (size_t i = 0; i < steady.rows.size() && i < named.rows.size(); i++) {
                    steady.rows[i].commodity = named.rows[i].commodity;
                }
            }
            if (!steady.rows.empty()) {
                const long long since = look->taken - 30 * 60;
                terminalOffered_.erase(std::remove_if(terminalOffered_.begin(), terminalOffered_.end(),
                                                      [since](const std::shared_ptr<Datarunner::Shot>& offered) {
                                                          return offered->taken < since;
                                                      }),
                                       terminalOffered_.end());
                std::vector<std::shared_ptr<Datarunner::Shot>> known = datarunnerShots_;
                known.insert(known.end(), terminalOffered_.begin(), terminalOffered_.end());
                auto fresh = std::make_shared<Datarunner::Shot>(Datarunner::newRowsOf(steady, known, since));
                if (!fresh->rows.empty()) {
                    // ...remembered without its pictures
                    auto offered = std::make_shared<Datarunner::Shot>();
                    offered->shop = fresh->shop;
                    offered->side = fresh->side;
                    offered->taken = fresh->taken;
                    offered->rows = fresh->rows;
                    for (Datarunner::Row& row : offered->rows) row.crop.clear();
                    terminalOffered_.push_back(std::move(offered));
                    // One picture and one report for a list: the rows join
                    // the picture of the same shop's same list not sent yet,
                    // and their piece of the look goes under it
                    const std::shared_ptr<Datarunner::Shot> into = terminalPicture(fresh->shop, fresh->side, since);
                    std::vector<Datarunner::CardBox> where;
                    for (const Datarunner::Row& row : fresh->rows) where.push_back(row.card);
                    const int frameHeight = look->montage.height;
                    if (into) {
                        Datarunner::addToMontage(into->montage, look->montage, where);
                        into->jpeg = Datarunner::montageForUex(into->montage, frameHeight);
                        if (!into->shown.empty()) {
                            std::ofstream out(into->shown, std::ios::binary | std::ios::trunc);
                            out.write(reinterpret_cast<const char*>(into->jpeg.data()), (std::streamsize)into->jpeg.size());
                        }
                        for (Datarunner::Row& row : fresh->rows) into->rows.push_back(std::move(row));
                        into->matched = false;   // the new rows to UEX's commodities, and their box sizes
                        std::cout << "\nUEX Datarunner: the terminal at " << (into->shop.empty() ? "?" : into->shop)
                                  << " - " << where.size() << " rows more, " << into->rows.size() << " in all" << std::endl;
                    } else {
                        fresh->montage.clear();
                        Datarunner::addToMontage(fresh->montage, look->montage, where);
                        fresh->jpeg = Datarunner::montageForUex(fresh->montage, frameHeight);
                        fresh->shown = keepPicture(fresh->jpeg, AppPaths::data() / "debug" / "uex_pictures",
                                                   "terminal_" + timeStamp("%Y%m%d_%H%M%S"));
                        fresh->id = datarunnerNextId_++;
                        std::cout << "\nUEX Datarunner: the terminal at " << (fresh->shop.empty() ? "?" : fresh->shop)
                                  << " - " << fresh->rows.size() << " new rows" << std::endl;
                        datarunnerShots_.push_back(std::move(fresh));
                    }
                    datarunnerNote_.clear();
                    changed = true;
                }
            }
        }
        if (sawTerminal) {
            look->montage.clear();   // kept to compare with, not to send
            terminalLastLook_ = look;
            terminalSeenLook_ = look;
        } else {
            terminalLastLook_.reset();
        }
    }
    // what the foot says of the reading, pushed where it changed - by the
    // clock too: a look that missed the terminal ends it only later
    auto reading = [&]() {
        if (terminalReadingShot() != terminalReadingPushed_) changed = true;
        return changed;
    };
    if (!on) {
        terminalLooking_ = false;
        terminalLastLook_.reset();
        terminalSeenLook_.reset();
        terminalDone_.clear();
        return reading();
    }
    // a terminal's line in the log since the looking began last, and lately
    const long long used = library_.trades().terminalUsedMs();
    const long long nowMs = (long long)std::time(nullptr) * 1000;
    if (used > terminalStartedFor_ && nowMs - used < 2 * 60 * 1000) {
        terminalStartedFor_ = used;
        if (!terminalLooking_) {
            terminalLooking_ = true;
            terminalSeen_ = now;
            terminalLastLook_.reset();
            terminalSeenLook_.reset();
            terminalDone_.clear();
        }
    }
    if (!terminalLooking_) return reading();
    // ...until no terminal has been in sight for twenty seconds
    if (now - terminalSeen_ > std::chrono::seconds(20)) {
        terminalLooking_ = false;
        terminalLastLook_.reset();
        terminalSeenLook_.reset();
        terminalDone_.clear();
        return reading();
    }
    // one look after the other while a terminal is in sight
    const auto gap = terminalLastLook_ ? std::chrono::milliseconds(300) : std::chrono::milliseconds(2000);
    if (terminalJob_.valid() || now - terminalLooked_ < gap) return reading();
    // on foot, the game in view, not at a rock, no scan running
    if (!scan_->gameInView() || gameState_.inSeat() || inScan_ ||
        now - scan_->rockPanelAt() < std::chrono::seconds(30)) {
        return reading();
    }
    if (!scan_->gameView().found) return reading();
    terminalLooked_ = now;
    const Datarunner::Pixels frame = scan_->grabViewPixels();
    if (frame.empty()) return reading();
    const long long taken = (long long)std::time(nullptr);
    const std::string channel = Datarunner::channelOf(Datarunner::screenshotsBeside(library_.gameLog()));
    const std::string shop = library_.trades().terminalUsedShop();
    DatarunnerEyes* eyes = &datarunnerEyes_;
    terminalJob_ = std::async(std::launch::async, [eyes, frame, taken, channel, shop]() {
        auto look = eyes->readFrame(frame);
        look->automatic = true;
        look->taken = taken;
        look->channel = channel;
        look->shop = shop;
        return look;
    });
    return reading();
}

// A terminal in use being read: one in sight within the last six seconds,
// its list not done with. A look that misses the terminal while the list
// moves puts the next one two seconds off: six seconds carry the reading
// over two such looks in a row (each read in up to 1.6 s), so the foot
// stays put while the player scrolls.
bool Application::terminalReading() const {
    return terminalLooking_ && terminalSeenLook_ &&
           std::chrono::steady_clock::now() - terminalSeen_ < std::chrono::seconds(6) &&
           !Datarunner::listDone(terminalDone_, terminalSeenLook_->shop, terminalSeenLook_->side);
}

int Application::terminalReadingShot() const {
    if (!terminalReading()) return -1;
    const auto picture =
        terminalPicture(terminalSeenLook_->shop, terminalSeenLook_->side, (long long)std::time(nullptr) - 30 * 60);
    return picture ? picture->id : 0;
}

std::shared_ptr<Datarunner::Shot> Application::terminalPicture(const std::string& shop, int side, long long since) const {
    std::shared_ptr<Datarunner::Shot> into;
    for (const auto& shot : datarunnerShots_) {
        if (shot->automatic && shot->shop == shop && shot->side == side &&
            shot->state == Datarunner::Shot::State::Check && shot->taken >= since && !shot->montage.empty()) {
            into = shot;
        }
    }
    return into;
}

void Application::serviceDatarunner() {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    // (the game's screenshots and the reading of them: watchScreenshots)
    if (!datarunnerNote_.empty() && now - datarunnerNoteAt_ > std::chrono::seconds(60)) {
        datarunnerNote_.clear();
        changed = true;
    }
    // UEX's game versions: which a picture is of
    if (uexVersionsJob_.valid() && uexVersionsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto versions = uexVersionsJob_.get();
        if (!versions.first.empty()) uexVersions_[0] = versions.first;
        if (!versions.second.empty()) uexVersions_[1] = versions.second;
        changed = true;
    }
    const bool on = datarunnerSince_ > 0 && !uexKey_.empty();
    if (!on) {
        datarunnerToRead_.clear();
        datarunnerGrowing_.clear();
        if (!datarunnerShots_.empty()) {
            // turned off: nothing more goes, the pictures waiting with it
            datarunnerShots_.clear();
            changed = true;
        }
        if (changed) pushDatarunner();
        return;
    }
    if (!uexVersionsJob_.valid() &&
        (!uexVersionsEver_ || now - uexVersionsAsked_ > std::chrono::hours(uexVersions_[0].empty() ? 0 : 6) +
                                                            std::chrono::minutes(UEX_RETRY_MINUTES))) {
        uexVersionsEver_ = true;
        uexVersionsAsked_ = now;
        const std::filesystem::path folder = AppPaths::data() / "uex";
        uexVersionsJob_ = std::async(std::launch::async, [folder]() {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            std::string why;
            const std::string body = uexListCached(folder / "game_versions.json", "game_versions", 6 * 3600, why);
            JsonValue answer;
            std::pair<std::string, std::string> versions;
            if (!body.empty() && parseJson(body, answer, why)) {
                if (const JsonValue* data = answer.find("data")) {
                    versions.first = data->textOr("live");
                    versions.second = data->textOr("ptu");
                }
            }
            return versions;
        });
    }
    // UEX's lists, and every picture matched to them and to the game's log
    askUexDirectory();
    if (uexDirectory_) {
        readUexReported();
        for (const auto& shot : datarunnerShots_) {
            if (shot->matched) continue;
            // the terminal the log saw fill last before the picture, within
            // half an hour; else where the player is
            const TradeBook::TerminalFill* fill =
                library_.trades().terminalAt(shot->taken * 1000 + 999, 30ll * 60 * 1000);
            TradeBook::TerminalFill here;
            if (!fill) {
                here.place = TradeBook::placeOf(gameState_);
                if (!here.place.empty()) fill = &here;
            }
            Datarunner::match(*shot, *uexDirectory_, fill);
            shot->matched = true;
            changed = true;
        }
    }
    // what stands in the way of Send changes with the clock: five minutes
    // run out, a picture grows older than UEX takes
    if (!datarunnerShots_.empty() && now - datarunnerPushed_ > std::chrono::seconds(10)) changed = true;
    if (changed) pushDatarunner();
}

void Application::pushDatarunner() {
    datarunnerPushed_ = std::chrono::steady_clock::now();
    const int reading = terminalReadingShot();
    terminalReadingPushed_ = reading;
    if (!gui_) return;
    const long long now = (long long)std::time(nullptr);
    MinerGUI::DatarunnerState state;
    for (const auto& shot : datarunnerShots_) {
        state.shots.push_back(shot);
        std::string blocked;
        if (!shot->matched) blocked = "Waiting for UEX's lists - The picture is checked once they are in";
        else blocked = Datarunner::whyNotSend(*shot, datarunnerSent_, now);
        const std::string& version = shot->channel == "PTU" ? uexVersions_[1] : uexVersions_[0];
        if (blocked.empty() && version.empty()) blocked = "Game version not known yet - Waiting for UEX";
        state.blocked.push_back(blocked);
        std::vector<int> holds;
        for (const Datarunner::Row& row : shot->rows) holds.push_back((int)Datarunner::holdOf(*shot, row, datarunnerSent_, now));
        state.holds.push_back(holds);
        state.versions.push_back(version);
        state.lengths.push_back(
            Datarunner::listLength(*shot, library_.trades().terminalAt(shot->taken * 1000 + 999, 30ll * 60 * 1000)));
    }
    if (uexDirectory_) {
        state.commodities = std::make_shared<const std::vector<std::pair<std::string, int>>>(uexDirectory_->commodityNames());
        state.terminals = std::make_shared<const std::vector<std::pair<int, std::string>>>(uexDirectory_->commodityTerminals());
    }
    state.signUp = uexAccountTrouble_ == "datarunner";
    state.note = datarunnerNote_;
    state.reading = reading >= 0;
    state.readingShot = (std::max)(reading, 0);
    gui_->setDatarunner(state);
}

// What the player did on the Datarunner card (MinerGUI::DatarunnerCallback)
void Application::datarunnerAction(int action, int shotId, int rowIndex, int value) {
    auto it = std::find_if(datarunnerShots_.begin(), datarunnerShots_.end(),
                           [&](const std::shared_ptr<Datarunner::Shot>& s) { return s->id == shotId; });
    if (it == datarunnerShots_.end()) return;
    Datarunner::Shot& shot = **it;
    Datarunner::Row* row = rowIndex >= 0 && rowIndex < (int)shot.rows.size() ? &shot.rows[rowIndex] : nullptr;
    const bool open = shot.state != Datarunner::Shot::State::Sending && shot.state != Datarunner::Shot::State::Sent;
    const long long now = (long long)std::time(nullptr);
    switch (action) {
        case 1: {   // Send: one report, the rows as checked and the picture they were read off
            readUexReported();
            const std::string& version = shot.channel == "PTU" ? uexVersions_[1] : uexVersions_[0];
            if (!open || !shot.matched || version.empty() || !Datarunner::whyNotSend(shot, datarunnerSent_, now).empty()) {
                break;
            }
            const bool production = DEV_ENV("SCMINER_UEX_DRYRUN") == nullptr;
            UexPending item;
            item.shot = shot.id;
            item.body = Datarunner::reportBody(shot, datarunnerSent_, now, version, production);
            for (const Datarunner::Row* sending : Datarunner::rowsToSend(shot, datarunnerSent_, now)) {
                Datarunner::Sent went;
                went.terminal = shot.terminal;
                went.side = shot.side;
                went.commodity = sending->commodity;
                went.taken = shot.taken;
                item.rows.push_back(went);
            }
            shot.state = Datarunner::Shot::State::Sending;
            shot.note.clear();
            // a list Bedrock read by itself: done with while the player is
            // at the terminal
            if (shot.automatic) terminalDone_.push_back({shot.shop, shot.side});
            std::cout << "\nUEX Datarunner: " << shot.file.filename().string() << " - sending " << item.rows.size()
                      << " rows to UEX" << (production ? "" : " (test report)") << std::endl;
            uexQueue_.push_back(std::move(item));
            break;
        }
        case 2:     // Discard, or Close once sent
            if (shot.state == Datarunner::Shot::State::Sending) break;
            if (shot.automatic) terminalDone_.push_back({shot.shop, shot.side});
            datarunnerShots_.erase(it);
            break;
        case 3:     // a row taken out, or put back
            if (row && open) row->removed = !row->removed;
            break;
        case 4:     // a row's values to check, checked against the picture
            if (row && open) {
                for (Certainty* sure : {&row->nameSure, &row->levelSure, &row->stockSure, &row->priceSure}) {
                    if (*sure == Certainty::Check) *sure = Certainty::Sure;
                }
            }
            break;
        case 5:     // the level, picked from the game's words
            if (row && open && value >= 1 && value <= 7) {
                row->level = value;
                row->levelSure = Certainty::Sure;
            }
            break;
        case 6:     // the stock, typed
            if (row && open && value >= 0) {
                row->stock = value;
                row->stockSure = Certainty::Sure;
            }
            break;
        case 7:     // the price, typed - a SCU's: a cargo box's stays out
            if (row && open && value >= 0 && !row->perUnit) {
                row->price = value;
                row->priceSure = Certainty::Sure;
            }
            break;
        case 8:     // the commodity, picked from UEX's
            if (row && open && uexDirectory_) {
                for (const auto& [name, id] : uexDirectory_->commodityNames()) {
                    if (id != value) continue;
                    row->commodity = id;
                    row->name = name;
                    row->nameSure = Certainty::Sure;
                }
            }
            break;
        case 9:     // the terminal, picked
            if (open && uexDirectory_ && !uexDirectory_->terminalName(value).empty()) shot.terminal = value;
            break;
        case 10:    // the list: BUY or SELL
            if (open && (value == 1 || value == 2)) {
                shot.side = value;
                shot.sideSure = true;
            }
            break;
        default:
            break;
    }
    pushDatarunner();
}

// What came of a picture's report: on UEX, all of it or the rows before a
// duplicate, or not at all - in UEX's words
void Application::datarunnerAnswered(const UexSent& sent) {
    const long long now = (long long)std::time(nullptr);
    auto it = std::find_if(datarunnerShots_.begin(), datarunnerShots_.end(),
                           [&](const std::shared_ptr<Datarunner::Shot>& s) { return s->id == sent.shot; });
    Datarunner::Shot* shot = it == datarunnerShots_.end() ? nullptr : it->get();
    // the rows UEX stored: all of them, or - turned down as a duplicate -
    // the ones before the entry it names. That entry is on UEX too, from
    // this player's report of the last five minutes, so it is held like a
    // row that went: a second Send carries only the rows after it, which
    // UEX did not get.
    std::vector<Datarunner::Sent> stored, onUex;
    if (sent.answered && sent.said.empty()) {
        stored = sent.rows;
    } else if (sent.answered && !sent.duplicate.empty()) {
        for (const Datarunner::Sent& row : sent.rows) {
            std::string name;
            if (uexDirectory_) {
                for (const auto& [written, id] : uexDirectory_->commodityNames()) {
                    if (id == row.commodity) name = written;
                }
            }
            if (sent.duplicate == std::to_string(row.commodity) || sent.duplicate == name) {
                onUex.push_back(row);
                break;
            }
            stored.push_back(row);
        }
    }
    onUex.insert(onUex.begin(), stored.begin(), stored.end());
    for (Datarunner::Sent row : onUex) {
        row.sent = now;
        datarunnerSent_.push_back(row);
        rememberUexReported(Datarunner::sentKey(row));
    }
    std::string line;
    if (sent.answered && sent.said.empty()) {
        line = "On UEX - " + std::to_string(stored.size()) + (stored.size() == 1 ? " row" : " rows");
    } else {
        line = sent.note.empty() ? std::string("Not sent") : sent.note;
        if (!stored.empty()) line += " - " + std::to_string(stored.size()) + " rows before it are on UEX";
    }
    std::cout << "\nUEX Datarunner: " << (shot ? shot->file.filename().string() : std::string("a picture")) << " - "
              << line << std::endl;
    if (shot) {
        // the ids of every report the picture went in: after a duplicate,
        // the rest goes in a second one
        shot->ids.insert(shot->ids.end(), sent.ids.begin(), sent.ids.end());
        shot->note = sent.answered && sent.said.empty() ? std::string() : (sent.note.empty() ? "Not sent" : sent.note);
        shot->state = sent.answered && sent.said.empty() ? Datarunner::Shot::State::Sent
                      : !stored.empty()                  ? Datarunner::Shot::State::Partly
                      : sent.answered                    ? Datarunner::Shot::State::Refused
                                                         : Datarunner::Shot::State::Failed;
        if (!stored.empty()) keepSentPicture(*shot);
    }
    pushDatarunner();
}

// The pictures that went to UEX, as they went, in the data folder's debug\uex_sent
// - for the player to look back at what was sent - for 30 days
void Application::keepSentPicture(const Datarunner::Shot& shot) {
    if (shot.jpeg.empty()) return;
    std::error_code ec;
    const std::filesystem::path folder = AppPaths::data() / "debug" / "uex_sent";
    std::filesystem::create_directories(folder, ec);
    const long long now = (long long)std::time(nullptr);
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && secondsSinceWritten(entry.path()) > Datarunner::OLDEST_SECONDS) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
    const std::string name = std::to_string(now) + "_" + std::to_string(shot.terminal) + "_" + shot.file.stem().string() + ".jpg";
    std::ofstream out(folder / name, std::ios::binary);
    out.write((const char*)shot.jpeg.data(), (std::streamsize)shot.jpeg.size());
}

// A patch moves the game's text on, and Bedrock's file - which the game reads
// in place of its own - stays on the build it was written from until it is
// written again: at Bedrock's next start, or with the next blueprint. What
// the patch added or changed is missing from the game until then, and on a
// channel Bedrock does not follow at the moment (the PTU while the log is
// LIVE's) until it is played with Bedrock open.
// So every half minute: on the channel followed the file is written anew,
// out of the patched game; on any other it is taken away - that channel's
// library is not the one at hand - and the game shows its own text there
// until it is played with Bedrock open again, rather than an old one.
void Application::keepGameTextsFresh() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastGameTextsCheck_ < std::chrono::seconds(30)) return;
    lastGameTextsCheck_ = now;
    if (library_.gameLog().empty()) return;
    const std::filesystem::path followed = library_.gameLog().parent_path();
    for (const auto& channel : GameTexts::channelsBeside(followed)) {
        const long long patch = GameTexts::outdatedSince(channel, std::chrono::minutes(2));
        if (patch == 0) continue;
        long long& answered = gameTextsAnswered_[channel.string()];
        if (answered == patch) continue;
        answered = patch;
        if (channel == followed && (gameTextsWanted_ || gameNamesWanted_)) {
            std::cout << "\nGame texts: the game was updated - written anew from its own text" << std::endl;
            gameTextsDue_ = true;
            gameTextsDueAt_ = now;
        } else {
            const GameTexts::Outcome taken = GameTexts::remove(channel);
            std::cout << "\nGame texts: " << channel.filename().string() << " was updated - " << taken.note << std::endl;
        }
    }
}

void Application::pushLibrary() {
    if (!gui_) return;
    std::string note;
    if (library_.gameLog().empty()) {
        note = "No Game.log found - Tick manually, or set gamelog=<path> in region_config.txt";
    } else if (library_.historyPending()) {
        note = "Reading old game logs, " + std::to_string(library_.historyRead()) + " so far";
    } else {
        note = "From game log (" + library_.channel() + ")";
    }
    // Say that the evidence is being kept, and where. The game prunes its own
    // logbackups folder; once it has, this file is the only thing left that
    // says what was received.
    if (library_.archived() > 0) {
        note += "  -  " + std::to_string(library_.archived()) +
                " lines kept in blueprint_log_backup.txt";
    }
    // ...and what was concluded rather than read: a contract that paid
    // nothing had nothing left to give (see ContractScan)
    if (library_.assumedCount() > 0) {
        note += "  -  " + std::to_string(library_.assumedCount()) + " from contracts without payout";
    }
    gui_->setOwnedBlueprints(library_.stamps(), note);
    libraryPushed_ = true;
}

// The contracts of the moment, for the window: what is running in the game,
// and what of the player's own was finished in the last three hours - the
// longest an outfit blocks a contract just finished is two. A gathering
// order newly taken is said in the log once.
void Application::pushContracts() {
    runningSeen_ = library_.runningChanges();
    contractsPushed_ = true;
    if (!gui_) return;
    std::vector<MinerGUI::ContractNow> list;
    for (const auto& r : library_.runningContracts()) {
        if (sessionQuiet_) break;      // the game is closed: nothing runs
        const RepContract* c = Reputation::contract(r.contract);
        if (!c) continue;
        MinerGUI::ContractNow now;
        now.contract = c;
        now.running = true;
        now.shared = r.shared;
        list.push_back(now);
        if (c->oreCount > 0 && ordersSaid_.insert(r.mission).second) {
            std::cout << "\n[order] " << c->title << (r.shared ? " (shared)" : "") << " wants";
            for (int k = 0; k < c->oreCount; k++) {
                const OrderOre& ore = ORDER_ORES[c->oreFirst + k];
                std::cout << (k ? (c->oreChoice ? " or " : ", ") : " ") << ore.least;
                if (ore.most > ore.least) std::cout << "-" << ore.most;
                std::cout << (ore.scu ? " SCU " : " ") << ore.name;
            }
            std::cout << std::endl;
        }
    }
    const long long nowUtc = (long long)std::time(nullptr);
    for (const auto& [key, e] : library_.contracts()) {
        (void)key;
        if (!e.completed) continue;
        const long long at = Reputation::epochOf(e.stamp);
        if (at < 0 || nowUtc - at > 3 * 3600 || at > nowUtc + 300) continue;
        const RepContract* c = Reputation::contract(e.contract);
        if (!c) continue;
        MinerGUI::ContractNow done;
        done.contract = c;
        done.finishedAt = at;
        list.push_back(done);
    }
    gui_->setContractsNow(std::move(list));
}

// A mining gadget stuck on a rock in the game, or taken off it again
// (GameState::gadgetOnRock): the rock view takes it from there. One the log
// put on a rock more than ten minutes ago - its tail, read at the start,
// reaches back - says nothing of the rock in front now.
void Application::applyGadget() {
    gadgetChangesSeen_ = gameState_.gadgetChanges();
    const std::string& cls = gameState_.gadgetOnRock();
    const MiningGadget* gadget = nullptr;
    if (cls.size() > 14) {
        // "Mining_Gadget_SHIN_Sabir" is "shin_sabir" in the tables
        std::string id = cls.substr(14);
        for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        gadget = gadgetById(id.c_str());
    }
    long long age = 0;
    if (const long long at = Reputation::epochOf(gameState_.gadgetStamp()); at > 0) {
        age = (std::max)(0LL, (long long)std::time(nullptr) - at);
    }
    const bool stale = gadget && age > 10 * 60;
    std::cout << "\n[gadget] ";
    if (cls.empty()) std::cout << "off the rock";
    else std::cout << cls << " on " << (gameState_.gadgetRock().empty() ? "a rock" : gameState_.gadgetRock());
    if (!cls.empty() && !gadget) std::cout << " - not one of the tables'";
    if (stale) std::cout << " - " << age / 60 << " min ago, left alone";
    std::cout << std::endl;
    if (gui_) gui_->setGadgetPlaced(stale ? nullptr : gadget, gameState_.gadgetRock(), age);
}

void Application::pushRanks() {
    ranksKnown_ = !library_.gameLog().empty() && !library_.historyPending();
    ranksPushed_ = true;
    if (gui_) gui_->setRanks(library_.standings(), ranksKnown_, library_.countsFrom());
}

void Application::describeSetup(std::ostream& out) const {
    out << "Bedrock " << BEDROCK_VERSION << "  " << timeStamp("%Y-%m-%d %H:%M:%S") << "\n";
    out << "commit:  " << BEDROCK_GIT_SHA << "\n";
    // Where the player's things are. First question in every support
    // conversation, and the answer moved in 4.9.
    out << "program: " << AppPaths::program().string() << "\n";
    out << "data:    " << AppPaths::data().string()
        << (AppPaths::portable() ? "  (portable.txt beside the exe)" : "") << "\n";
    out << "virtual screen: " << GetSystemMetrics(SM_XVIRTUALSCREEN) << ","
        << GetSystemMetrics(SM_YVIRTUALSCREEN) << " " << GetSystemMetrics(SM_CXVIRTUALSCREEN) << "x"
        << GetSystemMetrics(SM_CYVIRTUALSCREEN) << ", " << GetSystemMetrics(SM_CMONITORS)
        << " monitor(s)\n";
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        MONITORINFOEXA info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoA(monitor, &info)) {
            auto& out = *reinterpret_cast<std::ostream*>(param);
            out << "  " << info.szDevice << ": " << info.rcMonitor.left << "," << info.rcMonitor.top
                << " " << (info.rcMonitor.right - info.rcMonitor.left) << "x"
                << (info.rcMonitor.bottom - info.rcMonitor.top)
                << ((info.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << "\n";
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    const ScanSession::Facts facts = scan_->facts();
    if (facts.haveView) {
        out << "game viewport: " << facts.viewW << "x" << facts.viewH << " at "
            << facts.viewX << "," << facts.viewY << "\n";
    } else if (scan_->gameMinimised()) {
        out << "game viewport: none, the game is minimised\n";
    } else {
        out << "game viewport: not found (no visible CryENGINE window)\n";
    }
    out << "aim box: " << facts.aimX << "," << facts.aimY << " " << facts.aimW
        << "x" << facts.aimH << (facts.aimAuto ? " (auto)" : " (manual)") << ", search margin "
        << facts.margin << "\n";
    if (facts.haveSearch) {
        out << "search area: " << facts.searchX << "," << facts.searchY << " " << facts.searchW << "x"
            << facts.searchH << "\n";
    }
    if (gui_) {
        out << "ui scale: " << gui_->uiScale() << " %"
            << (uiScalePercent_ > 0 ? " (from region_config.txt)" : " (worked out from the screen)")
            << "\n";
    } else {
        out << "ui scale: no window in this run"
            << (uiScalePercent_ > 0 ? ", region_config.txt says " + std::to_string(uiScalePercent_) + " %" : "")
            << "\n";
    }
    out << "snapshot hotkey: "
        << ((gui_ && gui_->snapshotHotkeyHeld()) ? "Ctrl+Alt+S held"
                                                 : "NOT held - taken by another program")
        << "\n";
    out << "scan: every " << captureIntervalMs_ << " ms at most, "
        << (easeIdle_ ? "eased when nothing is found" : "never eased (ease=0)")
        << ", " << facts.threads << " threads\n";
    out << "hud scale: " << facts.hudScale << " (hudscale=" << facts.hudScalePercent
        << "; 100 = as captured, 0 = follow the picture, reference "
        << facts.referenceHeight << " px tall)\n";
    out << "capture path: " << facts.source << "\n";
    if (!facts.duplication.empty()) out << "  duplication: " << facts.duplication << "\n";
    if (!facts.light.empty()) out << "  light: " << facts.light << "\n";
    if (!facts.picture.empty()) out << "  picture: " << facts.picture << "\n";
    out << "templates: " << (facts.embeddedTemplates ? "built in" : "from ./glyphs") << "\n";
    out << "game log: "
        << (library_.gameLog().empty()
                ? std::string("not found (no game running, no launcher log, no gamelog= in region_config.txt)")
                : library_.gameLog().string())
        << (gameLogOverride_.empty() ? "" : " (pinned in region_config.txt)")
        << (library_.channel().empty() ? "" : " - channel " + library_.channel() + ", file " + library_.fileName())
        << "\n";
    out << "blueprint library: " << library_.ownedCount() << " owned (" << library_.byHandCount()
        << " by hand), " << library_.unknownNames().size() << " received but unknown to this build, "
        << library_.historyRead() << " log files read, "
        << library_.archived() << " evidence lines kept"
        << (library_.historyPending() ? " (still reading)" : "") << "\n";
    out << "ranks: " << library_.contractsKept() << " contracts of the player's own kept, counted from "
        << Reputation::versionName(library_.countsFrom()) << ", old logs checked for the game they are of: "
        << (library_.logsChecked() ? "yes" : "not yet") << "\n";
    for (const auto& [name, stamp] : library_.unknownNames()) {
        out << "  unknown: " << name << " (" << stamp << ")\n";
    }
    out << "recording: " << (recordScans_ ? "on" : "off") << ", live: "
        << (liveModeActive_ ? "on" : "off") << ", last value: " << lastRecognizedSignature_ << "\n";
    // The rock view reads a panel's figures by their shapes and needs nothing
    // for that; only the ore's name wants Tesseract's data beside the exe.
    // Noted here so a "the rock view does nothing" report can be diagnosed.
    {
        std::error_code ec;
        const bool data = std::filesystem::exists("./tessdata/eng.traineddata", ec);
        out << "rock view: " << (facts.rockView ? "on" : "off") << ", ore names: "
            << (data ? "read (tessdata beside the program)" : "not read (no tessdata beside the program)") << "\n";
    }
}

// What the capture looked like: a black picture means the capture path
// sees nothing (exclusive full screen, wrong adapter), a white one that HDR
// highlights were clipped - both read as "nothing recognised" otherwise

void Application::writeSessionInfo() {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;
    {
        std::ofstream out(dataPath("debug/session.txt"), std::ios::trunc);
        if (out.is_open()) describeSetup(out);
    }
    mirrorConfigToDebug();
}

void Application::mirrorConfigToDebug() const {
    std::error_code ec;
    if (!std::filesystem::exists(dataPath(REGION_CONFIG_FILE), ec)) return;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;
    std::filesystem::copy_file(dataPath(REGION_CONFIG_FILE), dataPath(std::string("debug/") + REGION_CONFIG_FILE),
                               std::filesystem::copy_options::overwrite_existing, ec);
}

void Application::snapshotFromConsole() {
    saveSnapshot();
    writeSessionInfo();   // now with the capture path the snapshot used
}

void Application::requestSnapshot() {
    // The hotkey arrives through the message pump, which a running scan
    // drives from inside itself: capturing again in there would cut into
    // the scan's own capture. Taken at the start of the next scan instead.
    if (inScan_) snapshotPending_ = true;
    else saveSnapshot();
}

void Application::saveSnapshot() {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;

    // Ten at most, the oldest making room: a report needs one or two, and a
    // 4K picture is ten megabytes
    std::vector<std::filesystem::path> older;
    // dataPath("debug"), not "debug": the snapshots are written beside the
    // player data, while a bare relative path points at the folder the exe
    // sits in, where nothing is written and nothing would be pruned.
    for (const auto& entry : std::filesystem::directory_iterator(dataPath("debug"), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("snapshot_", 0) == 0 && entry.path().extension() == ".png") older.push_back(entry.path());
    }
    std::sort(older.begin(), older.end());
    while (older.size() >= static_cast<size_t>(snapshotKeep_)) {
        std::filesystem::path note = older.front();
        note.replace_extension(".txt");
        std::filesystem::remove(older.front(), ec);
        std::filesystem::remove(note, ec);
        older.erase(older.begin());
    }

    const std::string base = dataPath("debug/snapshot_") + timeStamp("%Y%m%d_%H%M%S");
    const ScanSession::SnapshotWrite shot = scan_->writeSnapshot(base + ".png");
    std::ofstream note(base + ".txt", std::ios::trunc);
    if (note.is_open()) {
        describeSetup(note);
        const char* what = shot.haveFrame ? "the game picture" : "capture FAILED";
        if (shot.skipped) {
            what = shot.minimised ? "none, Star Citizen is minimised"
                                  : "none, Star Citizen is not running";
        }
        note << "snapshot: " << what << " at " << shot.x << "," << shot.y << "\n";
    }
    const bool saved = shot.wrote;
    std::cout << "\n[snapshot] " << base << (saved ? ".png" : ".txt (no picture)") << std::endl;
    MessageBeep(saved ? MB_ICONASTERISK : MB_ICONHAND);
}

} // namespace SCMiner
