#include "ScanSession.h"

#include "AppPaths.h"
#include "DatarunnerRead.h"
#include "DevSwitch.h"
#include "GameFiles.h"
#include "GrabHub.h"
#include "LaserRange.h"
#include "LoadoutReader.h"
#include "PanelKeep.h"
#include "LoadoutResolve.h"
#include "ReadLimits.h"
#include "RockContentsRead.h"
#include "RockScans.h"
#include "ScanPanel.h"

#include <opencv2/core/utility.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace SCMiner {

namespace {

std::string dataPath(const std::string& name) {
    return (AppPaths::data() / name).string();
}


// Empty scans in a row before the search widens to the whole monitor
constexpr int REACQUIRE_AFTER_MISSES = 8;

// The rolling record: five clips of a minute, overwriting each other
constexpr int RECORD_SEGMENT_SECONDS = 60;
constexpr int RECORD_SEGMENTS = 5;
// Nothing under debug/ may grow without bound - this build goes to other
// people's machines. The ring overwrites itself (~450 MB for five lossless
// minutes), the image rings are 40 and 20 deep, the scan log rotates at
// SCAN_LOG_MAX_BYTES, and the copies Ctrl+Alt+K makes of the ring are capped:
// the newest few stay (keeps= in region_config.txt, default 3), older ones
// go, and together they stay under keepgb= (default 2 GB). Recording and
// keeping also refuse to fill a drive.
// (the caps themselves are members - maxKeeps_ and maxKeepGb_ - so that
// region_config.txt can raise them on a machine that collects footage)
constexpr std::uintmax_t RECORD_MIN_FREE_BYTES = 1536ull * 1024 * 1024;
constexpr std::uintmax_t SCAN_LOG_MAX_BYTES = 10ull * 1024 * 1024;

// Once a marker has been read, the search sticks to it: a small window around
// where it was, rather than the wide area around the aim point. The marker is
// anchored to a rock in 3D, so head movement and the ship's own sway shift it
// by a few centimetres at most between two scans - but the wide window also
// contained chat text and cockpit labels, and the scan kept locking onto those
// instead. Held for PATIENCE scans of nothing before the search opens up again.
constexpr int LOCK_MARGIN_PX = 150;
// How many scans that read nothing the lock survives.
//
// The marker travels 29 px between scans on average, 130 px at worst,
// while the pinned window reaches 150 px in every direction, so it almost
// never leaves the window. A short patience is what loses it: at 12 (~4 s)
// a stretch of unreadable frames - the ship's lamp, a bright planet, a hull
// whose HUD is a colour the templates have not seen - throws the search
// back to the aim point, where it settles on the dashboard and stays.
//
// Holding longer costs nothing: the full-screen sweep is driven by its own
// counter and still fires every 8 unread scans, so a lock that really is dead
// does not block re-acquisition.
constexpr int LOCK_PATIENCE = 60;   // ~20 s
// How well the last picture of the marker has to match before the lock is
// moved onto it. High enough that terrain or a different HUD element does not
// pull the lock away, low enough to survive the marker changing size slightly
// as the ship closes in.
constexpr double LOCK_FOLLOW_NCC = 0.65;
constexpr int LOCK_FOLLOW_WINDOW = 150;     // px about the lock the follow searches, after the camera's shift
// Scans in a row the locked marker's picture is found nowhere in the area -
// the whole area is matched once the window about it missed - before the
// lock is let go: ~6 s at 300 ms. LOCK_PATIENCE only counts scans whose
// locator found no label at all, and in a cockpit it nearly always finds
// one, the HUD's own figures; without this cap a lock outlives its marker
// by minutes and keeps the whole-area match and locator running (354 ms a
// scan against 99 without them).
constexpr int LOCK_LOST_AFTER = 20;

// How many of the located candidates a scan actually tries.
//
// An unscanned contact carries the SAME map pin and a line of text
// ("UNKNOWN 6.2km"), and a planet ping puts a dozen of them up at once, all
// of them perfectly marker-shaped. The one rock that has actually been
// scanned - the only one carrying a number - is then just one candidate
// among many, and with three slots handed out by distance to the aim point
// it is routinely never looked at.
//
// Widening this is cheap because the template reader throws a contact out in
// microseconds: "6.2km" is three digits, a signature is four to six. Tesseract
// is the expensive part, so it stays rationed - see OCR_FALLBACK_CANDIDATES.
constexpr int SCAN_CANDIDATES = 12;
constexpr int OCR_FALLBACK_CANDIDATES = 3;
// How many of the candidates nearest the aim may be read a second time with
// the locator's row hint dropped - see GlyphReader::read. The hint is
// usually right; where it is wrong it caught the HUD line above or below
// and the row is scored across two baselines. Three, because the marker is
// what the player is pointing at and the list is sorted by that.
constexpr int SECOND_CHANCE_CANDIDATES = 3;
// The second reader's idle pace: after this many seconds without a find it
// reads on its own one row a scan at most, and none it has looked at lately
// (see the scan)
constexpr double LINE_IDLE_AFTER_S = 5.0;
constexpr int LINE_IDLE_ROWS = 1;
// ...and how long a row it looked at and found nothing in is left
// alone, and how many such rows are remembered. Resting is per ROW:
// a HUD row that stands still is read once every few seconds instead
// of never, and a row that was not there before is read at once.
constexpr double LINE_LOOK_AGAIN_S = 5.0;
constexpr size_t LINE_LOOKED_MOST = 12;

// After this long with nothing read, most scans look only at the area
// around the aim box rather than at everything the margin allows, and every
// WIDE_SWEEP_EVERY-th scan goes wide again to catch a marker off to the
// side. The capture stays the same rectangle either way - the tracking
// compares frame with frame and needs that - only the locator is handed
// less to sift.
//
// Measured under the six threads a scan may use, on the busiest frames
// there are (an ice field, 3,600 bright specks): the whole area costs
// 740 ms of processor time a sweep, the near area 155 ms. Seven near
// sweeps and one wide one, at 300 ms each, come to three quarters of a
// core.
constexpr double NEAR_SWEEP_AFTER = 15.0;   // seconds without a reading
constexpr int WIDE_SWEEP_EVERY = 8;         // ...and every eighth scan still goes wide
constexpr int NEAR_SWEEP_MARGIN = 150;      // px around the aim box

// A marker is anchored to a rock in 3D. The ship is never perfectly still -
// even parked it sways, and the HUD with it - so the plate never stops moving
// by a pixel or two. A row of digits that reads the same value at the SAME
// pixel over and over is therefore not a rock: it is part of the cockpit.
//
// This is what the ship's own status pills are - a coloured icon, white digits,
// a rounded plate, sitting in the corner forever. They pass every test the
// marker passes: the pin matches at 73 % (real markers run 52-100 %, so no
// threshold separates them), and what they read - 8390 - is an exact table
// value, 2x Tin. Standing still is the one thing they do that a rock cannot,
// and it is measurable; unchecked, the scan sits on them while the real
// marker is never looked at.
//
// Once a marker HAS been read, keeping the frame on it is worth far more than
// reading it again. The plate is the same handful of pixels from frame to
// frame, so matching the picture of it survives what the reader does not: a
// ship's lamp bleaching the plate, a different hull's HUD colour, a bright
// planet, the marker shrinking with distance. Recognition then only has to
// succeed ONCE instead of on every frame.
//
// So a successful match keeps the lock alive on its own. The cap is a
// safeguard, not a schedule: without one, a patch of terrain that happens to
// correlate would hold the lock for the rest of the session.
constexpr int LOCK_FOLLOW_PATIENCE = 200;   // ~60 s of tracking with no reading
// A head turn moves the marker much further than the ship's own sway. When
// the picture is not found in the pinned window, the next scan looks in a
// wider one before the lock is given up.
constexpr int LOCK_WIDEN_FACTOR = 3;

// Reading the camera movement out of the picture. The response is phase
// correlation's own confidence that the two frames really are the same scene
// shifted; the cap rejects a jump no head turn could produce between two
// scans, which is what a scene change looks like.
// Measured against a recorded approach, comparing the shift this reports with
// how far the marker really travelled between the same two frames:
//
//     response 0.24 -> camera 14.3 px, marker 15 px
//     response 0.19 -> camera 25.3 px, marker 26 px
//     response 0.17 -> camera 33.2 px, marker 34 px
//     response 0.30 -> camera  2.1 px, marker  2 px
//
// Above about 0.15 the two agree to a pixel. Below 0.10 the answer is noise -
// jumps of several hundred pixels that no head turn produces. So the bar sits
// between them: when the view movement can be measured the box is carried by
// it, and when it cannot the box simply stays put, which is never worse.
constexpr double CAMERA_MIN_RESPONSE = 0.15;
constexpr double CAMERA_MAX_SHIFT_PX = 400.0;
// The picture is split into this many horizontal bands, and this many of them
// have to answer confidently before the box is moved. Measured on the live
// picture: with a region framed around the signature and the compass ribbon,
// four to six bands out of six answer on every single frame.
constexpr int CAMERA_BANDS = 6;
constexpr size_t CAMERA_MIN_BANDS = 3;

constexpr int STUCK_LIMIT = 40;          // ~15 s at the usual scan interval
constexpr int STUCK_DRIFT_PX = 3;        // what still counts as "the same pixel"
constexpr int BLACKLIST_SCANS = 150;     // ~1 min before it may be tried again

static bool g_debugImages = false;

void saveDebugCapture(const std::string& name, const cv::Mat& image) {
    if (!g_debugImages) return;
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (!ec) {
        cv::imwrite(dataPath("debug/" + name), image);
    }
}

// Appends one line per live scan to debug/scan_log.csv. The image ring
// buffer only holds the last 40 captures, so this log is the durable
// record of what was read, and why it was shown or held back.
std::uintmax_t folderBytes(const std::filesystem::path& folder) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
        std::error_code fec;
        if (entry.is_regular_file(fec)) total += entry.file_size(fec);
    }
    return total;
}

// Free space on the drive a folder lives on; 0 when it cannot be told
std::uintmax_t freeBytesAt(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto info = std::filesystem::space(dir, ec);
    return ec ? 0 : info.available;
}

// The duration of the scan being logged, set by the scan itself, split by
// stage (grab, recorder, tracking, locator) so the log shows where the time
// goes; the locator is usually most of it.
static double g_scanMs = 0.0;
static double g_captureMs = 0.0;   // the screen grab alone
static double g_recordMs = 0.0;    // handing the frame to ffmpeg
static double g_intervalMs = 0.0;  // what the scan waited before this one
// Where the marker that was read sat, against the aim box the player
// marked. off_x/off_y is its centre against the box's centre; outside is
// how far it reached past the box's edge, 0 while it is inside.
//
// The search area is the aim box plus trackmargin in every direction; this
// column sizes that margin. 250 px for a 1440-line picture (defaultAimBox):
// further out stand mostly the cockpit's own figures, read as signatures -
// one value at one spot for minutes, such as 10,110 (Ouratite x3) low in a
// fighter's canopy. 73 of 78 such readings stood more than 250 px out, and
// real readings that far out mostly reappear nearer the box within half a
// minute.
static int g_offX = 0, g_offY = 0, g_outside = 0;
static bool g_havePos = false;
// When something was last found - anything at all, including a reading
// that was held back. Set in logScan so no path can forget it.
static std::chrono::steady_clock::time_point g_lastFind =
    std::chrono::steady_clock::now();
static double g_trackMs = 0.0;     // camera motion + following the lock
static double g_locateMs = 0.0;    // finding the marker in the picture
// Whether the pin under this reading only cleared the bar once the
// comparison was allowed to slide. Such a reading is deliberately held
// back until it repeats, and without this column the log shows the
// holding back but never the reason for it.
static bool g_pinSlid = false;
// How many candidates the locator handed the reader on this scan, and how
// the best of them scored as a pin. Without these two a "no-read" line
// says nothing about WHY: a marker on screen that no channel could cut
// looks exactly like empty space.
static int g_found = 0;
static int g_bestPin = 0;
static std::string g_ship;         // the ship Game.log names, for the scan log ("" when unknown)
static std::string g_shipLabel;    // ...and as a name, for the notes
static bool g_drake = false;       // the log's maker is Drake, for the font note
static std::string g_place;        // where Game.log puts the player, "System - Place", for the scan log
static const char* g_font = "";    // "panel" or "canopy": the set the accepted row was read against
// What the second reader said on this scan - agrees, differs, doubts, reads,
// none; "agreed before" when it was not asked again, "resting" when the idle
// pace kept it from reading on its own, blank when it was not asked - and
// what it cost (TextLineReader)
static std::string g_line;
static double g_lineMs = 0.0;

void logScan(int slot, const std::string& result, int value,
             double confidence, int agreement) {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;

    // One line per scan adds up to megabytes a day, so the log rotates:
    // over the cap it becomes scan_log.old.csv and a fresh one starts.
    // Checked every 500 lines, not on every one.
    static int lines = 0;
    if (lines++ % 500 == 0) {
        std::error_code sec;
        const auto size = std::filesystem::file_size(dataPath("debug/scan_log.csv"), sec);
        if (!sec && size > SCAN_LOG_MAX_BYTES) {
            std::filesystem::remove(dataPath("debug/scan_log.old.csv"), sec);
            std::filesystem::rename(dataPath("debug/scan_log.csv"), dataPath("debug/scan_log.old.csv"), sec);
        }
    }

    static const char* const LOG_HEADER =
        "time,slot,result,value,confidence,agreement,scan_ms,capture_ms,record_ms,track_ms,"
        "locate_ms,pin_slid,interval_ms,found,best_pin,off_x,off_y,outside,second,second_ms,ship,font,place";
    bool writeHeader = !std::filesystem::exists(dataPath("debug/scan_log.csv"), ec);
    // A log from an older build has fewer columns. Appending to it would
    // leave both halves unreadable, so it is put aside once and started over.
    if (!writeHeader) {
        static bool headerChecked = false;
        if (!headerChecked) {
            headerChecked = true;
            std::string first;
            std::ifstream old(dataPath("debug/scan_log.csv"));
            if (old.is_open() && std::getline(old, first)) {
                if (!first.empty() && first.back() == 13) first.pop_back();
                if (first != LOG_HEADER) {
                    old.close();
                    std::error_code rec;
                    std::filesystem::remove(dataPath("debug/scan_log.old.csv"), rec);
                    std::filesystem::rename(dataPath("debug/scan_log.csv"),
                                            dataPath("debug/scan_log.old.csv"), rec);
                    writeHeader = !rec;
                }
            }
        }
    }
    std::ofstream log(dataPath("debug/scan_log.csv"), std::ios::app);
    if (!log.is_open()) return;

    if (writeHeader) {
        log << LOG_HEADER << "\n";
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);
    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

    log << timeStr << "," << slot << "," << result << "," << value << ","
        << static_cast<int>(confidence) << "," << agreement << ","
        << static_cast<int>(g_scanMs) << "," << static_cast<int>(g_captureMs) << ","
        << static_cast<int>(g_recordMs) << "," << static_cast<int>(g_trackMs) << ","
        << static_cast<int>(g_locateMs) << "," << (g_pinSlid ? 1 : 0) << ","
        << static_cast<int>(g_intervalMs) << "," << g_found << "," << g_bestPin << ",";
    // Blank rather than 0 where there was no marker: a zero offset is a
    // marker dead centre, which is the opposite of not having one.
    if (g_havePos) log << g_offX << "," << g_offY << "," << g_outside;
    else log << ",,";
    log << "," << g_line << "," << static_cast<int>(g_lineMs);
    log << "," << g_ship << "," << g_font << "," << g_place;
#ifdef BEDROCK_DEV_TOOLS
    // the recording's frame on show, while one is replayed (SCMINER_REPLAY)
    if (ScreenCapture::replayFrame() >= 0) log << "," << ScreenCapture::replayFrame();
#endif
    log << "\n";

    // Anything but a blank screen counts as the player being at work,
    // and puts the scan back to full speed on the very next round.
    if (result != "no-read") g_lastFind = std::chrono::steady_clock::now();
}

}  // namespace

static bool gameCovered(HWND game) {
#ifdef _WIN32
    HWND front = GetForegroundWindow();
    if (!front || front == game) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(front, &process);
    if (process == GetCurrentProcessId()) return false;
    char cls[64] = {};
    GetClassNameA(front, cls, sizeof(cls));
    for (const char* shell : {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}) {
        if (std::strcmp(cls, shell) == 0) return false;
    }
    if (IsIconic(front) || !IsWindowVisible(front)) return false;
    RECT f{}, g{}, both{};
    if (!GetWindowRect(front, &f) || !GetWindowRect(game, &g)) return true;
    if (!IntersectRect(&both, &f, &g)) return false;
    const long long overlap = (long long)(both.right - both.left) * (both.bottom - both.top);
    const long long whole = (long long)(g.right - g.left) * (g.bottom - g.top);
    return whole > 0 && overlap > whole / 20;   // more than a twentieth of it
#else
    (void)game;
    return false;
#endif
}


static void logRockLook(double lookMs, double sinceLastMs, int waitMs, bool panelUp, bool quick, int misses,
                        double glance, const ScanPanel& panel, const LaserRangeLook& range) {
#ifndef BEDROCK_DEV_TOOLS
    // The notes in this log say in words how the reader works. A release
    // keeps the figures of a scan (scan_log.csv) and not this.
    (void)lookMs; (void)sinceLastMs; (void)waitMs; (void)panelUp; (void)quick; (void)misses; (void)glance; (void)panel;
    (void)range;
    return;
#endif
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        opened = true;
        std::error_code ec;
        std::filesystem::create_directories(dataPath("debug"), ec);
        const std::string path = dataPath("debug") + "/rock_looks.csv";
        const char* HEADER =
            "time,look_ms,since_last_ms,wait_ms,panel_up,quick,misses,found,figures,labels_x,labels_y,scale,ink,glance,"
            "range_m,note,range_note";
        // a long session's worth, then it starts over - and a log with other
        // columns in it starts over too
        bool stale = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 20u * 1024u * 1024u;
        if (!stale && std::filesystem::exists(path, ec)) {
            std::ifstream in(path);
            std::string first;
            std::getline(in, first);
            while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
            stale = first != HEADER;
        }
        if (stale) std::filesystem::rename(path, dataPath("debug") + "/rock_looks.old.csv", ec);
        const bool fresh = !std::filesystem::exists(path, ec);
        out.open(path, std::ios::app);
        if (fresh && out.is_open()) out << HEADER << std::endl;
    }
    if (!out.is_open()) return;
    const auto clock = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(clock);
    const int millis = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(clock.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &secs);
    char when[16] = {};
    std::strftime(when, sizeof(when), "%H:%M:%S", &local);
    std::string note = panel.note.substr(0, 110);
    std::string rangeNote = range.note.substr(0, 160);
    for (std::string* text : {&note, &rangeNote}) {
        for (char& c : *text) {
            if (c == '"') c = '\'';
            if (c == '\n' || c == '\r') c = ' ';
        }
    }
    char line[220];
    std::snprintf(line, sizeof(line), "%s.%03d,%.0f,%.0f,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%d,%.2f,%d,", when, millis, lookMs,
                  (std::min)(sinceLastMs, 99999.0), waitMs, panelUp ? 1 : 0, quick ? 1 : 0, misses, panel.found ? 1 : 0,
                  panel.figuresRead, panel.labels.x, panel.labels.y, panel.labelScale, panel.ink, glance, range.metres);
    out << line << '"' << note << "\",\"" << rangeNote << '"' << std::endl;
}


static void logRockList(const char* event, float jobMass, float rockMass, const RockContents* read, int tries,
                        const std::string& why) {
#ifndef BEDROCK_DEV_TOOLS
    (void)event; (void)jobMass; (void)rockMass; (void)read; (void)tries; (void)why;
    return;
#endif
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        opened = true;
        std::error_code ec;
        std::filesystem::create_directories(dataPath("debug"), ec);
        const std::string path = dataPath("debug") + "/rock_lists.csv";
        const char* HEADER = "time,event,job_mass,rock_mass,whole,lines,sum,ms,tries,note";
        bool stale = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 20u * 1024u * 1024u;
        if (!stale && std::filesystem::exists(path, ec)) {
            std::ifstream in(path);
            std::string first;
            std::getline(in, first);
            while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
            stale = first != HEADER;
        }
        if (stale) std::filesystem::rename(path, dataPath("debug") + "/rock_lists.old.csv", ec);
        const bool fresh = !std::filesystem::exists(path, ec);
        out.open(path, std::ios::app);
        if (fresh && out.is_open()) out << HEADER << std::endl;
    }
    if (!out.is_open()) return;
    const auto clock = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(clock);
    const int millis = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(clock.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &secs);
    char when[16] = {};
    std::strftime(when, sizeof(when), "%H:%M:%S", &local);
    std::string note = why + (read ? (why.empty() ? "" : "; ") + read->note : std::string());
    note = note.substr(0, 160);
    for (char& c : note) {
        if (c == '"') c = '\'';
        if (c == '\n' || c == '\r') c = ' ';
    }
    char line[200];
    std::snprintf(line, sizeof(line), "%s.%03d,%s,%.0f,%.0f,%d,%d,%.2f,%.0f,%d,", when, millis, event, jobMass, rockMass,
                  read ? (read->whole ? 1 : 0) : -1, read ? (int)read->parts.size() : -1, read ? read->sum : -1.0f,
                  read ? read->ms : -1.0, tries);
    out << line << '"' << note << '"' << std::endl;
}


static bool sameMassRead(float a, float b) {
    if (a <= 0 || b <= 0) return false;
    const std::string x = std::to_string(std::lround(a)), y = std::to_string(std::lround(b));
    if (x == y) return true;
    if (x.size() != y.size()) return false;
    int differ = 0;
    for (size_t i = 0; i < x.size(); i++) differ += x[i] != y[i] ? 1 : 0;
    return differ == 1;
}


CaptureRegion defaultAimBox(const cv::Rect& viewport, int* margin) {
    const double h = viewport.height;
    CaptureRegion box;
    box.width = (std::max)(40, static_cast<int>(std::lround(h * 352.0 / 1440.0)));
    box.height = (std::max)(20, static_cast<int>(std::lround(h * 180.0 / 1440.0)));
    box.x = viewport.x + viewport.width / 2 - box.width / 2;
    box.y = viewport.y + static_cast<int>(std::lround(h * 475.0 / 1440.0)) - box.height / 2;
    box.autoDetect = true;
    if (margin) *margin = (std::max)(50, static_cast<int>(std::lround(h * 250.0 / 1440.0)));
    return box;
}


static cv::Rect pictureFor(const CaptureRegion& aim) {
    if (auto viewport = ScreenCapture::gameViewport()) return *viewport;
    const POINT centre{aim.x + aim.width / 2, aim.y + aim.height / 2};
    if (HMONITOR monitor = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
            return cv::Rect(info.rcMonitor.left, info.rcMonitor.top,
                            info.rcMonitor.right - info.rcMonitor.left,
                            info.rcMonitor.bottom - info.rcMonitor.top);
        }
    }
    return cv::Rect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}


static const char* sourceName(ScreenCapture::Source source) {
    switch (source) {
        case ScreenCapture::Source::DxgiHdr: return "Desktop Duplication, HDR (tone mapped in the app)";
        case ScreenCapture::Source::Dxgi: return "Desktop Duplication, SDR";
        case ScreenCapture::Source::Gdi: return "GDI BitBlt (duplication unavailable)";
        case ScreenCapture::Source::Composite: return "the game's X11 window (Composite)";
        default: return "nothing captured yet";
    }
}


struct ScanSession::Impl {

    using Event = ScanSession::Event;
    using Rect = ScanSession::Rect;
    using Hooks = ScanSession::Hooks;
    using SignatureReport = ScanSession::SignatureReport;

    struct LineLooked {
        cv::Rect where;
        std::chrono::steady_clock::time_point when;
    };
    struct LabelCandidate {
        cv::Mat image;
        cv::Rect screenRect;
        cv::Rect row;
        cv::Mat source;
        cv::Point origin;
    };

    static constexpr int RANGE_KEPT_SECONDS = 15;
    static constexpr int OWN_READINGS = 3;
    static constexpr int TURRET_GONE_READINGS = 6;
    static constexpr int LOADOUT_ROCK_CHECK_S = 30;
    static constexpr const char* PANEL_SPOT_FILE = "panel_spot.txt";

    Hooks hooks_{};
    ReadingPolicy policy_{};
    SignatureReport signatureOut_{};
    std::vector<Event> events_;
    std::vector<Rect> covers_;
    bool liveMode_ = false;
    bool askInSeat_ = false;
    bool askCanMine_ = false;
    int askHeadCount_ = 0;
    std::string askShip_, askHull_;
    int askBoardings_ = 0;
    int askRocksHere_ = 0;          // RockAsk::rocksHere
    int askGameGeneration_ = 0;
    std::string askChannel_;
    bool pendingLoadout_ = false;
    std::string pendingKey_;

    bool ocrAvailable_ = false;
    int agreedValue_ = 0;
    cv::Rect agreedRect_;
    std::vector<LineLooked> lineLooked_;
    bool recordScans_ = false;
    int maxKeeps_ = 3;
    int maxKeepGb_ = 2;
    int captureIntervalMs_ = 300;
    bool easeIdle_ = true;
    CaptureRegion signatureRegion_{};
    bool aimAuto_ = true;
    cv::Rect aimViewport_;
    double hudScale_ = 1.0;
    int hudScalePercent_ = 100;
    std::string captureStats_;
    bool autoTrack_ = true;
    int trackMargin_ = 250;
    cv::Rect lastLabelRect_;
    bool hasLastLabel_ = false;
    int lockMisses_ = 0;
    cv::Mat lockPatch_;
    int followOnly_ = 0;
    bool followMissed_ = false;
    int lockLost_ = 0;
    bool followedNow_ = false;
    cv::Mat previousView_;
    int stuckValue_ = 0;
    cv::Rect stuckRect_;
    int stuckCount_ = 0;
    cv::Rect blacklistRect_;
    int blacklistLeft_ = 0;
    bool lastScanTracked_ = false;
    mutable int missStreak_ = 0;
    int lastRecognizedSignature_ = 0;

    std::deque<std::pair<std::chrono::steady_clock::time_point, int>> rangeReadings_;
    RockStats shownRock_;
    float shownMass_ = 0;
    std::string shownOre_;
    int otherLooks_ = 0;
    float otherMass_ = 0;
    std::deque<RockStats> rockReadings_;
    int rockMissShots_ = 0;
    int rockUnsettled_ = 0;
    RockScans::LookCheck rockCheck_;
    RockScans::MassCheck massCheck_;
    PanelKeep panelKeep_;
    PanelShipWitness panelShip_;
    RockScans::ListReadings listReadings_;
    bool rockLabelsOnly_ = false;
    int rockQuickLooks_ = 0;
    bool rockSettling_ = false;
    int rockSettlingLooks_ = 0;
    int glanceQuiet_ = 0;
    PanelSpot keptSpot_;
    RockViewHold rockHold_;
    OCREngine rockOcr_;
    bool rockOcrTried_ = false;
    bool rockOcrReady_ = false;
    bool rockSeen_ = false;
    PanelSteadier rockSteadier_;
    std::chrono::steady_clock::time_point lastRockLook_{};
    std::chrono::steady_clock::time_point lastRockPanel_{};

    std::future<SeenLoadout> loadoutJob_;
    std::string loadoutJobSeat_;
    std::string loadoutJobKindId_;
    std::chrono::steady_clock::time_point lastLoadoutRead_{};
    std::string loadoutSeat_;
    std::string loadoutSeen_;
    std::string loadoutSaid_;
    int loadoutAgreeing_ = 0;
    int loadoutUnsure_ = 0;
    int loadoutDisagreeing_ = 0;
    bool loadoutNoHud_ = false;
    bool loadoutSettled_ = false;
    std::string loadoutLogSeat_;
    bool loadoutRockCheck_ = false;
    std::chrono::steady_clock::time_point loadoutRockCheckSince_{};

    std::future<std::string> turretJob_;
    std::chrono::steady_clock::time_point lastTurretRead_{};
    std::string turretSeen_;
    int turretAgreeing_ = 0;
    std::string turretSaid_;
    int turretMissing_ = 0;

    std::future<RockContents> contentsJob_;
    float contentsJobMass_ = 0;
    float contentsFor_ = 0;
    std::string contentsOre_;
    std::string contentsShown_;
    float contentsWanted_ = 0;
    std::string contentsWantedOre_;
    std::string contentsSeen_;
    int contentsTries_ = 0;
    std::chrono::steady_clock::time_point lastContentsRead_{};

    cv::Mat grabRegion(const CaptureRegion& region);
    void breathe();
    void flagNote(const RockStats& raw, int rows, float labelScale);
    void pushClearRock();
    void pushClearContents();
    void rockModelFor(RockStats& rock) const;
    void keepPanelPicture(const cv::Mat& frame, const ScanPanel& panel, bool toneMapped,
                          std::chrono::steady_clock::time_point now);
    bool collectRockContents();
    void readLoadoutOnHud(const cv::Mat& frame, const ScanPanel& panel);
    void readTurretOnHud(const cv::Mat& frame);
    void takeTurretLine(const std::string& line);
    void takeSeenLoadout(const SeenLoadout& read);
    void noteLoadoutResult(const std::string& fit, bool changed);

    bool lineLookedAt(const cv::Rect& where);
    void rememberLineLook(const cv::Rect& where);
    int effectiveIntervalMs(int baseMs, bool ease) const;
    RockStats steadyRock(const std::deque<RockStats>& readings);
    bool rockViewWanted();
    void logRockReading(const ScanPanel& panel, const RockStats* steady, int rangeMetres,
                        const std::string& notTaken = std::string());
    float laserRange(std::chrono::steady_clock::time_point now) const;
    void rockToView(RockStats& rock, std::chrono::steady_clock::time_point look);
    void lookForRock();
    void rockGone(const char* why);
    void loadoutLookAgain();
    void loadoutCheckForRock(float mass);
    void clearRockView();
    void readContentsOnHud(const cv::Mat& frame, const ScanPanel& panel, const RockStats& rock);
    void takeRockContents(const RockContents& read, const RockStats& rock);
    void loadPanelSpot(const cv::Size& viewport);
    void keepPanelSpot(const cv::Size& viewport);
    void readSignature();
    bool refreshAutoAim(bool announce);
    cv::Rect allowedArea() const;
    CaptureRegion trackingSearchArea() const;
    void keepRecording();
    void trackCameraMotion(const cv::Mat& view);
    void followLockedMarker(const cv::Mat& searchImage, const CaptureRegion& search);
    std::vector<LabelCandidate> captureSignatureCandidates(int maxCandidates);
    cv::Mat captureSignatureImage(cv::Rect* outScreenRect);
    bool refreshHudScale();
    cv::Mat grabGamePicture();
    void noteCapture(const cv::Mat& image);
};

bool ScanSession::Impl::lineLookedAt(const cv::Rect& where) {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = lineLooked_.size(); i-- > 0;) {
        if (std::chrono::duration<double>(now - lineLooked_[i].when).count() > LINE_LOOK_AGAIN_S) {
            lineLooked_.erase(lineLooked_.begin() + (std::ptrdiff_t)i);
        }
    }
    for (const LineLooked& seen : lineLooked_) {
        const int slackX = (std::max)(6, where.width / 3);
        const int slackY = (std::max)(4, where.height / 2);
        if (std::abs(seen.where.x - where.x) <= slackX && std::abs(seen.where.y - where.y) <= slackY &&
            std::abs(seen.where.width - where.width) <= slackX) {
            return true;
        }
    }
    return false;
}


void ScanSession::Impl::rememberLineLook(const cv::Rect& where) {
    lineLooked_.push_back({where, std::chrono::steady_clock::now()});
    while (lineLooked_.size() > LINE_LOOKED_MOST) lineLooked_.erase(lineLooked_.begin());
}


int ScanSession::Impl::effectiveIntervalMs(int baseMs, bool ease) const {
    if (!ease) return baseMs;
    const double quiet = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_lastFind).count();

    // Is the game in view? Only meaningful when one was actually FOUND:
    // with none, this says nothing about whether the game is running, and
    // guessing from silence is how a scanner talks itself into never
    // scanning. In view unless another window lies over it (gameCovered) -
    // not merely "in front", which a window on another screen is too.
    const HWND game = ScreenCapture::gameWindow();
    const bool watching = !game || !gameCovered(game);

    // Cost per scan on six threads: a sweep of the whole search area ~740 ms
    // of CPU, the area around the crosshair ~155 ms. With near sweeps doing
    // most of the work, 300 ms between quiet scans costs about 3/4 of a core
    // and sees a new marker within ~1/3 s. So while the game is in view a
    // quiet stretch does not slow the scan at all: a deeper step would only
    // add a wait after every rock.
    int ms = baseMs;

    // Deeper than a second only when the game is covered.
    // A signature sometimes flashes up for a moment and is gone - that is
    // what accepted-flash is for, and it is a real reading. While the game
    // is in front, one second is as slow as this gets, so a flash is still
    // caught; once the window is behind something else there is nothing on
    // screen to flash at all, and two seconds is free.
    if (!watching) ms = (std::max)(ms, quiet > 60.0 ? 2000 : 1000);

    return (std::max)(ms, baseMs);
}


RockStats ScanSession::Impl::steadyRock(const std::deque<RockStats>& readings) {
    RockStats out;
    if (readings.empty()) return out;

    auto middle = [&](float RockStats::*field) {
        std::vector<float> seen;
        for (const RockStats& r : readings) {
            if (r.*field > 0) seen.push_back(r.*field);
        }
        if (seen.empty()) return 0.0f;
        std::sort(seen.begin(), seen.end());
        return seen[seen.size() / 2];
    };
    out.mass = middle(&RockStats::mass);
    out.instability = middle(&RockStats::instability);
    out.scu = middle(&RockStats::scu);
    // Resistance is the one figure that is legitimately zero - plenty of
    // rocks have none - so it cannot be filtered on "greater than zero". It
    // CAN be missing, though, and a reader that failed hands back -1: that is
    // not a reading and must not be averaged with ones that are, or shown as
    // "-1 %".
    //
    // And a reading from the cockpit and one from the turret are two figures
    // for one rock - 75 % white and 52 % green, or red - whose middle is
    // neither. The kind seen more often wins, the newer one on a tie, and only
    // its own readings go into the middle.
    {
        int lowered = 0, plain = 0;
        for (const RockStats& r : readings) {
            if (r.resistance < 0) continue;
            if (r.resistanceAdjusted) lowered++; else plain++;
        }
        bool useLowered = lowered > plain;
        if (lowered == plain && lowered > 0) {
            for (auto it = readings.rbegin(); it != readings.rend(); ++it) {
                if (it->resistance >= 0) { useLowered = it->resistanceAdjusted; break; }
            }
        }
        std::vector<float> seen;
        for (const RockStats& r : readings) {
            if (r.resistance >= 0 && r.resistanceAdjusted == useLowered) {
                seen.push_back(r.resistance);
            }
        }
        std::sort(seen.begin(), seen.end());
        out.resistance = seen.empty() ? -1.0f : seen[seen.size() / 2];
        out.resistanceAdjusted = useLowered && !seen.empty();
        // 5.0 prints the laser's modifiers beside the figures: the newest
        // reading of the kind taken carries them on
        for (auto it = readings.rbegin(); it != readings.rend(); ++it) {
            if (!it->deltasRead) continue;
            out.deltasRead = true;
            if (it->resistance >= 0 && it->resistanceAdjusted == useLowered) {
                out.resistanceDelta = it->resistanceDelta;
                break;
            }
        }
        for (auto it = readings.rbegin(); it != readings.rend(); ++it) {
            if (it->deltasRead && it->instability >= 0) {
                out.instabilityDelta = it->instabilityDelta;
                break;
            }
        }
    }
    {
        std::map<std::string, int> votes;
        for (const RockStats& r : readings) {
            if (!r.ore.empty()) votes[r.ore]++;
        }
        int best = 0;
        for (const auto& v : votes) {
            if (v.second > best) { best = v.second; out.ore = v.first; }
        }
    }
    return out;
}


bool ScanSession::Impl::rockViewWanted() {
    return true;
}


void ScanSession::Impl::logRockReading(const ScanPanel& panel, const RockStats* steady, int rangeMetres,
                                 const std::string& notTaken) {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    const std::string path = dataPath("debug") + "/rock_readings.csv";
    const char* HEADER = "time,figures,ore,mass,res,res_printed,inst,scu,"
                         "steady_mass,steady_res,steady_res_printed,steady_inst,steady_scu,"
                         "range_m,steady_range_m,box_x,box_y,box_w,box_h,note";
    // a log with other columns in it starts over
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path);
        std::string first;
        std::getline(in, first);
        in.close();
        while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
        if (first != HEADER) std::filesystem::rename(path, dataPath("debug") + "/rock_readings.old.csv", ec);
    }
    const bool fresh = !std::filesystem::exists(path, ec);
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;
    if (fresh) out << HEADER << std::endl;

    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int millis = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &secs);
    char when[40] = {};
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);

    // A name or a note may hold a comma; quoted, and a quote inside turned
    // into an apostrophe rather than escaped, so any spreadsheet opens it
    auto quoted = [](std::string text) {
        for (char& c : text) {
            if (c == '"') c = '\'';
            if (c == '\n' || c == '\r') c = ' ';
        }
        return "\"" + text + "\"";
    };
    auto colour = [](const RockStats& r) {
        return r.resistance < 0 ? "" : (r.resistanceAdjusted ? "colour" : "white");
    };

    const RockStats& read = panel.stats;
    char line[512];
    std::snprintf(line, sizeof(line), "%s.%03d,%d,%s,%.0f,%.0f,%s,%.2f,%.2f,",
                  when, millis, panel.figuresRead, quoted(read.ore).c_str(), read.mass,
                  read.resistance, colour(read), read.instability, read.scu);
    out << line;
    if (steady) {
        std::snprintf(line, sizeof(line), "%.0f,%.0f,%s,%.2f,%.2f,", steady->mass,
                      steady->resistance, colour(*steady), steady->instability, steady->scu);
        out << line;
    } else {
        out << ",,,,,";
    }
    out << rangeMetres << "," << (steady ? std::to_string((int)std::lround(steady->distance)) : std::string()) << ",";
    out << panel.box.x << "," << panel.box.y << "," << panel.box.width << ","
        << panel.box.height << ","
        << quoted(notTaken.empty() ? panel.note : "not taken: " + notTaken + "; " + panel.note) << std::endl;
}


float ScanSession::Impl::laserRange(std::chrono::steady_clock::time_point now) const {
    std::vector<int> recent;
    for (const auto& r : rangeReadings_) {
        if (now - r.first <= std::chrono::seconds(RANGE_KEPT_SECONDS)) recent.push_back(r.second);
    }
    if (recent.empty()) return 0.0f;
    std::sort(recent.begin(), recent.end());
    return (float)recent[recent.size() / 2];
}


void ScanSession::Impl::rockToView(RockStats& rock, std::chrono::steady_clock::time_point look) {
    // a look of this rock without its SCU keeps the one on view - and so does
    // the caller's rock, for the composition's kg per SCU
    if (shownMass_ > 0) RockScans::keepScu(shownRock_, rock);
    // another rock: the range read at the last one is not this one's
    if (shownMass_ > 0 && RockScans::anotherRock(shownRock_, rock)) {
        while (!rangeReadings_.empty() && rangeReadings_.front().first < look) rangeReadings_.pop_front();
    }
    rock.distance = laserRange(look);
    { Event ev; ev.kind = Event::Kind::UpdateRock; ev.rock = rock; events_.push_back(std::move(ev)); }
    shownRock_ = rock;
}


// Nyx I's rocks are made of the 5.0 numbers (RockModel in MiningAdvice.h):
// where the place says so, or in Nyx before the log names the body, where
// the figures fit only those. rocksHere is SignatureAnalyzer::Rocks: 0 the
// usual rocks, 1 a place's own, 2 either.
void ScanSession::Impl::rockModelFor(RockStats& rock) const {
    rock.v5 = askRocksHere_ == 1 || (askRocksHere_ == 2 && RockScans::v5Fits(rock));
}

// The cut is written here, on the scan's own thread, once a rock at most: a
// PNG of the panel alone. The frame is tone mapped where the desktop is HDR.
// Nothing read depends on it.
void ScanSession::Impl::keepPanelPicture(const cv::Mat& frame, const ScanPanel& panel, bool toneMapped,
                                         std::chrono::steady_clock::time_point now) {
    const PanelKeep::Kept kept =
        panelKeep_.look(panel, frame.size(), std::chrono::duration<double>(now.time_since_epoch()).count());
    if (kept == PanelKeep::Kept::None) return;
    PanelKeep::About about;
    about.channel = askChannel_;
    about.ship = g_shipLabel;
    about.seated = askInSeat_;
    about.toneMapped = toneMapped;
    std::string name;
    if (panelKeep_.write(dataPath("debug"), frame, panel, kept, about, &name)) {
        std::cout << "[rock] kept a picture of the panel" << (kept == PanelKeep::Kept::Unread ? " it could not read" : "")
                  << ": debug/" << name << ".png" << std::endl;
    }
}

bool ScanSession::Impl::collectRockContents() {
    if (!contentsJob_.valid() || contentsJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    // (as the loadout's: an exception in the job is a list not read)
    RockContents read;
    try {
        read = contentsJob_.get();
    } catch (const std::exception& e) {
        std::cerr << "[rock] list reading failed: " << e.what() << std::endl;
    }
    if (DEV_ENV("SCMINER_CONTENTS_LOG")) {
        std::cout << "\n[rock] list for " << (int)contentsJobMass_ << " kg at " << (int)shownMass_ << " kg: "
                  << (read.whole ? "whole" : "not whole") << ", " << read.parts.size() << " lines, sum " << read.sum
                  << "; " << read.note << std::endl;
    }
    // The rock's line of rock_scans.csv: two readings alike for the mass the
    // view had when each picture was taken, whichever rock is on view now -
    // a pilot who scans the next rock within a second or two would leave
    // every list behind otherwise
    if (read.whole) {
        std::vector<RockScans::ScanPart> parts;
        for (const RockPartRead& part : read.parts)
            parts.push_back({part.ore, part.inert, part.percent, part.quality});
        if (listReadings_.confirms(contentsJobMass_, parts)) {
            Event ev;
            ev.kind = Event::Kind::ListRecorded;
            ev.rock.mass = contentsJobMass_;
            ev.contents = read;
            events_.push_back(std::move(ev));
        }
    }
    const bool thisRock = rockSeen_ && sameMassRead(contentsJobMass_, shownMass_);
    logRockList("result", contentsJobMass_, shownMass_, &read, contentsTries_,
                thisRock ? "for this rock" : "for another mass than on view: dropped");
    if (!thisRock) return false;
    RockStats rock;
    rock.mass = shownMass_;
    rock.ore = shownOre_;
    const bool firstOfTwo = read.whole && contentsFor_ <= 0;
    takeRockContents(read, rock);
    return firstOfTwo && contentsFor_ <= 0;
}

void ScanSession::Impl::lookForRock() {
    if (!rockViewWanted()) return;
    const auto now = std::chrono::steady_clock::now();
    // A list reading come back is taken before the look; where it was the
    // first of two for the rock on view, the look comes now and the second
    // reading starts off it
    const bool listAgain = collectRockContents();
    // While a panel is up its figures settle over several looks, so it is
    // looked at twice as often then; with none on screen, every second and a
    // half is enough to notice one come up. A panel whose labels are up and
    // whose figures are not - the zeros while the scan runs, or a look caught
    // while the HUD swayed - is looked at again on the next scan: a rock's
    // figures can stand for only two seconds after four of zeros, and the
    // regular looks can miss them altogether.
    // A panel the face reader read counts as up too: in the MOLE's turrets
    // that is the only reader of the figures, and at a look every second and
    // a half the rock comes in late and goes out late.
    const bool panelUp = rockHold_.misses() < 4 && (rockSteadier_.looks() > 0 || !rockReadings_.empty());
    // Only where a rock panel can come up: in a ship that mines, or before the
    // log has named a ship at all. A full look with nothing on screen costs
    // 216 ms of the whole viewport every second and a half - for nothing in a
    // ship that cannot mine.
    if (!panelUp && !askCanMine_) return;
    // ...three times running at most: labels whose figures cannot be read at
    // all would otherwise be looked at on every scan, and the signatures wait
    const bool quick = rockLabelsOnly_ && rockQuickLooks_ < 3;
    // A panel found whose figures have not settled is looked at again on the
    // very next scan, four times at most: the steadier wants a second look
    // that agrees, and at 700 ms between looks that second look comes 0.9 to
    // 1.0 s after the first.
    const bool settling = rockSettling_ && rockSettlingLooks_ < 4;
    int waitMs = quick || settling || listAgain ? 0 : (panelUp ? 700 : 1500);
    // The glance: between two full looks, with nothing on screen, the place
    // the last panel stood is captured on its own - a few hundred pixels
    // each way - and looked at for that one column at that one size. A few
    // milliseconds; the full look with nothing on screen costs a tenth of a
    // second, which is why it waits a second and a half, and that wait is
    // most of the time to a rock (1.8 s between looks in the median). When
    // the column is there the full look comes now; when that finds nothing
    // the glance rests for three scans, so a cockpit that looks like a
    // column at that spot costs no more than the plain wait.
    double glance = -1;
    if (!panelUp && !quick && now - lastRockLook_ < std::chrono::milliseconds(waitMs)) {
        if (glanceQuiet_ > 0) {
            glanceQuiet_--;
        } else if (const PanelSpot spot = lastPanelSpot(); spot.known) {
            if (auto viewport = ScreenCapture::gameViewport()) {
                const cv::Rect window = panelSpotWindow(spot, cv::Size(viewport->width, viewport->height));
                if (window.width > 20 && window.height > 20) {
                    CaptureRegion part;
                    part.x = viewport->x + window.x;
                    part.y = viewport->y + window.y;
                    part.width = window.width;
                    part.height = window.height;
                    part.autoDetect = false;
                    const cv::Mat piece = grabRegion(part);
                    setPanelToneMapped(liveCapture().lastSource() == ScreenCapture::Source::DxgiHdr);
                    glance = glanceAtPanelSpot(spot, piece);
                    if (glance >= glanceBar(spot)) {
                        waitMs = 0;
                        glanceQuiet_ = 3;
                        preferPanelInk(spot.ink);
                    }
                }
            }
        }
    }
    if (now - lastRockLook_ < std::chrono::milliseconds(waitMs)) return;
    const double sinceLastMs = std::chrono::duration<double, std::milli>(now - lastRockLook_).count();
    lastRockLook_ = now;

    auto viewport = ScreenCapture::gameViewport();
    if (!viewport) return;          // the game is not up; leave the view alone

    CaptureRegion whole;
    whole.x = viewport->x;
    whole.y = viewport->y;
    whole.width = viewport->width;
    whole.height = viewport->height;
    whole.autoDetect = false;
    const cv::Mat frame = grabRegion(whole);
    if (frame.empty()) return;
    // an HDR desktop comes tone mapped - see readLaserRange, and the Golem's
    // lettering (setPanelToneMapped)
    const bool toneMapped = liveCapture().lastSource() == ScreenCapture::Source::DxgiHdr;
    setPanelToneMapped(toneMapped);
    // Which game draws the panel, by the live log's header (41002 is 4.10.2,
    // 50000 5.0.0, 0 not read yet: the 4.10 readers)
    if (askGameGeneration_ != panelGeneration()) {
        setPanelGeneration(askGameGeneration_);
        std::cout << "[rock] panel of game "
                  << (askGameGeneration_ > 0 ? std::to_string(askGameGeneration_) : std::string("unknown"))
                  << std::endl;
    }

    if (!rockOcrTried_) {
        rockOcrTried_ = true;
        OCRConfig config;
        rockOcrReady_ = rockOcr_.initialize(config);
        throttlePanelSearch(true);
        loadPanelSpot(cv::Size(viewport->width, viewport->height));
        std::cout << "[rock] text reader: " << (rockOcrReady_ ? "on" : "off - no tessdata beside the exe; "
                     "the figures are read by their shapes, the ore's name is not read") << std::endl;
    }
    // NOT a condition for looking. The shapes read the panel's figures on
    // their own; Tesseract only names the ore and backs the figures up where
    // it is there, and an engine that did not start answers every question
    // with nothing. Packages carry no tessdata: returning here without one
    // blinds the rock view for every download while it still works beside a
    // build tree that has one.
    // ...and the second reader behind the shapes, for a HUD whose numerals
    // they were not cut for - the MOLE's turret (see readPanelByLines)
    const ScanPanel panel = readScanPanel(frame, rockOcr_, &liveGlyphs(), &sharedLines());
    if (panel.found || panel.labels.x >= 0) lastRockPanel_ = std::chrono::steady_clock::now();
    // The app's own picture of the panel, for a bug report: one per rock, and
    // one of a panel seen and not read (PanelKeep.h)
    keepPanelPicture(frame, panel, toneMapped, now);
    if (panel.found) keepPanelSpot(cv::Size(viewport->width, viewport->height));
    // The panel's own ship, where the log has the player in no seat
    if (const char* id = panelShip_.look(panel.found && panel.labels.x >= 0 ? panel.labelGlyph : 0, askInSeat_)) {
        Event ev;
        ev.kind = Event::Kind::ShipSeen;
        ev.shipId = id;
        events_.push_back(std::move(ev));
        std::cout << "\n[rock] the panels are a " << id << "'s and the log has no seat: that fit" << std::endl;
    }
    readLoadoutOnHud(frame, panel);
    rockLabelsOnly_ = !panel.found && panel.labels.x >= 0;
    rockQuickLooks_ = quick ? rockQuickLooks_ + 1 : 0;
    rockSettlingLooks_ = settling ? rockSettlingLooks_ + 1 : 0;
    rockSettling_ = false;
    if (panel.found) glanceQuiet_ = 0;
    // The laser's range, off the gauge the panel's labels place: on every
    // look that found them, whether or not its figures were read, and kept
    // for the rock on view - see laserRange
    const LaserRangeLook range =
        panel.labels.x >= 0 ? readLaserRange(frame, panel, toneMapped) : LaserRangeLook();
    if (range.metres > 0) {
        rangeReadings_.emplace_back(now, range.metres);
        while (rangeReadings_.size() > 5) rangeReadings_.pop_front();
    }
    logRockLook(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count(), sinceLastMs,
                waitMs, panelUp, quick, rockHold_.misses(), glance, panel, range);

    // The shape reader's looks go to the steadier, which says when the
    // figures are settled; until then the view keeps what it had
    if (panel.found && !panel.figures[0].likeness.empty()) {
        rockHold_.read();
        rockReadings_.clear();
        RockStats rock;
        const bool settled = rockSteadier_.add(panel, rock);
        rockModelFor(rock);
        // ...and settled figures that cannot be the rock's are waited past
        // the same way: the scan finishes, or the misread goes
        const std::string why = settled ? rockCheck_.notTaken(rock) : std::string();
        rockSettling_ = !settled || !why.empty();
        if (rockSettling_) {
            logRockReading(panel, nullptr, range.metres, why);
            // A panel that will not settle for twelve seconds is not the
            // rock the view shows any more
            if (++rockUnsettled_ >= 16) {
                rockUnsettled_ = 0;
                if (rockSeen_) {
                    rockSeen_ = false;
                    std::cout << "\n[rock] gone" << std::endl;
                }
                clearRockView();
                shownMass_ = 0;
                rangeReadings_.clear();
            }
            return;
        }
        rockUnsettled_ = 0;
        rockToView(rock, now);
        logRockReading(panel, &shownRock_, range.metres);
        flagNote(panel.stats, frame.rows, panel.labels.x >= 0 ? (float)panel.labelScale : 0.0f);
        loadoutCheckForRock(rock.mass);
        shownMass_ = rock.mass;
        shownOre_ = rock.ore;
        readContentsOnHud(frame, panel, rock);
        if (!rockSeen_) {
            rockSeen_ = true;
            std::cout << "\n[rock] " << (int)rock.mass << " kg, " << (int)rock.resistance
                      << " % resistance, instability " << rock.instability
                      << (rock.ore.empty() ? "" : "  - " + rock.ore) << std::endl;
        }
        return;
    }

    // The mass is the one figure nothing works without, so it is the test for
    // "this is a panel and it was read" rather than the box alone.
    // A panel that named at least three of its four figures is a panel, mass
    // or no mass on this particular look - see readScanPanel. Anything less
    // is a pair of rules somewhere in the cockpit, and taking that for a
    // panel leaves the view stuck on a rock that is no longer in front of the
    // ship.
    if (panel.found && panel.figuresRead >= 3) {
        rockHold_.read();
        // A panel the face reader read and the shapes did not see at all is
        // the one picture worth keeping: the shapes are what the app is fast
        // with, and the only way to find out why they miss the MOLE's pilot
        // seat is to look at a frame where they did. Two a session, and only
        // in a build with the rock view on.
        if (panel.labels.x < 0 && rockMissShots_ < 2) {
            rockMissShots_++;
            std::error_code ec;
            std::filesystem::create_directories(dataPath("debug"), ec);
            cv::imwrite(dataPath("debug/panel_miss_" + std::to_string(rockMissShots_) + ".png"), frame);
            std::cout << "[rock] kept a picture of the panel the shapes missed: debug/panel_miss_"
                      << rockMissShots_ << ".png" << std::endl;
        }
        // A look whose figures cannot be the rock's stays out of the five:
        // the game's first picture of a rock it has not finished scanning -
        // looked at again on the next scan, the figures follow in a moment -
        // or a misread. One that names no ore is told by the ore the looks
        // before it named. Twelve seconds of nothing else, and the rock on
        // view is not the one in front of the ship any more.
        {
            RockStats probe = panel.stats;
            if (probe.ore.empty() && !rockReadings_.empty()) probe.ore = steadyRock(rockReadings_).ore;
            rockModelFor(probe);
            const std::string why = rockCheck_.notTaken(probe);
            if (!why.empty()) {
                logRockReading(panel, nullptr, range.metres, why);
                rockSettling_ = true;
                if (++rockUnsettled_ >= 16) {
                    rockUnsettled_ = 0;
                    if (rockSeen_) {
                        rockSeen_ = false;
                        std::cout << "\n[rock] gone" << std::endl;
                    }
                    clearRockView();
                    shownMass_ = 0;
                    rangeReadings_.clear();
                }
                return;
            }
            rockUnsettled_ = 0;
        }
        // A mass the view's with a numeral more, less or other - what the
        // second reader makes of the pilot seat's fringed figures now and
        // then - waits for a second look that reads it the same
        // (RockScans::MassCheck)
        if (shownMass_ > 0 && !massCheck_.takes(shownRock_, panel.stats)) {
            logRockReading(panel, nullptr, range.metres, "a numeral off the mass on view, once");
            return;
        }
        // Another rock's look starts the window again rather than average
        // two rocks: its mass off by more than a misread, and another figure
        // with it (RockScans::anotherRock)
        if (!rockReadings_.empty() && RockScans::anotherRock(steadyRock(rockReadings_), panel.stats)) {
            rockReadings_.clear();
        }
        rockReadings_.push_back(panel.stats);
        while (rockReadings_.size() > 5) rockReadings_.pop_front();

        RockStats rock = steadyRock(rockReadings_);
        rockModelFor(rock);
        // ...but the view needs a mass to say anything at all, so it waits
        // for one rather than showing a rock with no figures on it.
        if (rock.mass <= 0) {
            logRockReading(panel, &rock, range.metres);
            return;
        }
        // The distance is not in the panel: it comes off the LASER RANGE
        // gauge beside it, where that is read - see laserRange
        rockToView(rock, now);
        logRockReading(panel, &shownRock_, range.metres);
        flagNote(panel.stats, frame.rows, panel.labels.x >= 0 ? (float)panel.labelScale : 0.0f);
        loadoutCheckForRock(rock.mass);
        shownMass_ = rock.mass;
        shownOre_ = rock.ore;
        readContentsOnHud(frame, panel, rock);
        if (!rockSeen_) {
            rockSeen_ = true;
            std::cout << "\n[rock] " << (int)rock.mass << " kg, "
                      << (int)rock.resistance << " % resistance, instability "
                      << rock.instability
                      << (rock.ore.empty() ? "" : "  - " + rock.ore) << std::endl;
        }
        return;
    }

    // A panel seen but not read well enough to count is logged all the same:
    // those are exactly the looks where the reader is losing the rock.
    if (panel.found) logRockReading(panel, nullptr, range.metres);

    // A look that found the panel's labels and read none of its figures is
    // not a miss: the panel is still on screen. In the MOLE's turrets the
    // face reader reads the figures in one look of ten or fewer, and counted
    // as misses those looks would send the view back to the signatures
    // between two readings.
    if (panel.labels.x >= 0) {
        rockHold_.seen();
        // ...unless its mass - white on every HUD, and read here - says
        // another rock is in front of the ship than the one on view, twice
        // running. While its laser is on the Golem prints a rock's
        // resistance and instability in red over its labels, which nothing
        // reads, so the rock looked at before would otherwise stay on view
        // for the next one.
        if (rockSeen_ && shownMass_ > 0 && panel.figures[0].placed) {
            const std::string& text = panel.figures[0].text;
            const bool digits = !text.empty() && text.size() <= 7 &&
                                std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
            const float mass = digits ? (float)std::atof(text.c_str()) : 0.0f;
            if (mass > 0 && std::fabs(mass - shownMass_) > shownMass_ * 0.25f) {
                if (otherMass_ > 0 && std::fabs(mass - otherMass_) <= otherMass_ * 0.05f) {
                    otherLooks_++;
                } else {
                    otherMass_ = mass;
                    otherLooks_ = 1;
                }
                if (otherLooks_ >= 2) {
                    rockSeen_ = false;
                    std::cout << "\n[rock] another rock (" << (int)mass << " kg) whose figures cannot be read"
                              << std::endl;
                    rockReadings_.clear();
                    rockSteadier_.clear();
                    rockUnsettled_ = 0;
                    shownMass_ = 0;
                    otherMass_ = 0;
                    otherLooks_ = 0;
                    rangeReadings_.clear();
                    clearRockView();
                }
            } else {
                otherMass_ = 0;
                otherLooks_ = 0;
            }
        }
        // A look that reads the gauge and none of the figures moves the
        // range on view all the same
        if (rockSeen_ && shownMass_ > 0 && otherLooks_ == 0 && range.metres > 0) {
            const float metres = laserRange(now);
            if (std::fabs(metres - shownRock_.distance) >= 0.5f) {
                shownRock_.distance = metres;
                { Event ev; ev.kind = Event::Kind::UpdateRock; ev.rock = shownRock_; events_.push_back(std::move(ev)); }
            }
        }
        // The list under the labels is the rock's on view all the same, and
        // is read on these looks too: the Golem's figures stand in red for
        // as long as its laser is on, and for three Golem rocks tested not one
        // list came in on the looks that read them. Not while the mass says
        // another rock may be there.
        if (rockSeen_ && shownMass_ > 0 && otherLooks_ == 0) {
            RockStats shown;
            shown.mass = shownMass_;
            shown.ore = shownOre_;
            shown.scu = shownRock_.mass == shownMass_ ? shownRock_.scu : 0.0f;
            shown.v5 = shownRock_.v5;
            readContentsOnHud(frame, panel, shown);
        }
        return;
    }

    // Eight misses - twelve seconds - before the view goes back: fewer made
    // the window flick between the two layouts whenever a reading was
    // missed. Nobody scans a new rock inside twelve seconds, and a layout
    // that changes under the eye is worse than one that is late. A signature
    // read after a miss takes it back sooner - see RockViewHold.
    if (rockHold_.missed()) rockGone("");
}


void ScanSession::Impl::rockGone(const char* why) {
    if (rockSeen_) {
        rockSeen_ = false;
        std::cout << "\n[rock] gone" << why << std::endl;
    }
    rockReadings_.clear();
    rockSteadier_.clear();
    rockUnsettled_ = 0;
    rangeReadings_.clear();
    clearRockView();
    shownMass_ = 0;
    rockHold_.gone();
}


void ScanSession::Impl::loadoutLookAgain() {
    loadoutRockCheck_ = false;
    loadoutSettled_ = false;
    loadoutDisagreeing_ = 0;
    lastLoadoutRead_ = {};
}


void ScanSession::Impl::loadoutCheckForRock(float mass) {
    const bool newRock = shownMass_ <= 0 || std::fabs(mass - shownMass_) > shownMass_ * 0.25f;
    if (!newRock || !loadoutSettled_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastLoadoutRead_ >= std::chrono::seconds(LOADOUT_ROCK_CHECK_S)) {
        // ...but after the rock's list: see readLoadoutOnHud
        loadoutRockCheck_ = true;
        loadoutRockCheckSince_ = now;
    }
}


void ScanSession::Impl::clearRockView() {
    if (contentsFor_ > 0) logRockList("off", contentsFor_, 0, nullptr, contentsTries_, "the rock went off view");
    pushClearRock();
    contentsFor_ = 0;
    contentsWanted_ = 0;
    contentsTries_ = 0;
}


void ScanSession::Impl::readContentsOnHud(const cv::Mat& frame, const ScanPanel& panel, const RockStats& rock) {
    if (!sharedLines().ready() || rock.mass <= 0) return;
    // Under 900 lines the list's figures are a few pixels high: at 720 a
    // reading can pass every check with a figure wrong, twice running, where
    // from 1080 up none did across nine resolutions tested. No list rather
    // than a wrong one.
    if (frame.rows < 900) return;
    if (contentsJob_.valid()) return;   // a reading under way: collectRockContents takes it
    if (sameMassRead(rock.mass, contentsFor_)) return;   // read, and on view
    // Another mass than the list on view was read at. It may be that rock
    // still - with less of it left, or its mass read another way: the same
    // ore, where both looks named one, and the mass no more than a quarter
    // off - and then its list stays while it is read again. Of any other
    // rock it says nothing.
    const auto alike = [&](float mass, const std::string& ore) {
        return mass > 0 && (ore.empty() || rock.ore.empty() || ore == rock.ore) &&
               std::fabs(rock.mass - mass) <= mass * 0.25f;
    };
    bool shown = alike(contentsFor_, contentsOre_);
    if (!shown && contentsFor_ > 0) {
        logRockList("off", contentsFor_, rock.mass, nullptr, contentsTries_, "the rock on view is another");
        contentsFor_ = 0;
        pushClearContents();
    }
    if (!alike(contentsWanted_, contentsWantedOre_)) {
        contentsTries_ = 0;
        lastContentsRead_ = {};
    }
    contentsWanted_ = rock.mass;
    contentsWantedOre_ = rock.ore;
    const cv::Rect part = rockContentsRegion(panel, frame.size());
    if (part.area() <= 0) return;   // this look found no label column to place the list by
    // How often. A rock without a list: on every look, eight running, then
    // every five seconds - a list over ground too busy to read it against
    // keeps several cores busy for nothing. A rock that may be the one whose
    // list is on view: at once, and then every five seconds for as long as
    // the readings say it is - a mass that goes down or flickers would have
    // the list read on every look for good. Three readings running that do
    // not say so, and the list comes off.
    const auto now = std::chrono::steady_clock::now();
    if (shown && contentsTries_ >= 3) {
        logRockList("off", contentsFor_, rock.mass, nullptr, contentsTries_, "three readings did not say it again");
        contentsFor_ = 0;
        pushClearContents();
        shown = false;
    }
    if (shown ? contentsSeen_ == contentsShown_ && now - lastContentsRead_ < std::chrono::seconds(5)
              : contentsTries_ >= 8 && now - lastContentsRead_ < std::chrono::seconds(5)) {
        return;
    }
    lastContentsRead_ = now;
    contentsTries_++;
    contentsJobMass_ = rock.mass;
    logRockList("start", rock.mass, rock.mass, nullptr, contentsTries_, shown ? "again, its list on view" : "");
    {
        Event ev;
        ev.kind = Event::Kind::ListTried;
        ev.rock.mass = rock.mass;
        events_.push_back(std::move(ev));
    }
    ScanPanel moved = panel;
    moved.labels -= part.tl();
    // ...and its kg per SCU, which names the lines Hephaestanite and Quartz
    // share (nameContentsByRock) - as a rock of the usual numbers would weigh
    const double kgPerScu = RockScans::usualKgPerScu(rock);
    try {
        contentsJob_ = std::async(std::launch::async,
                                  [this, picture = frame(part).clone(), moved, ore = rock.ore, kgPerScu]() {
                                      RockContents read;
                                      readRockContents(picture, moved, ore, sharedLines(), read, kgPerScu);
                                      return read;
                                  });
    } catch (const std::system_error&) {
        // no thread to be had: the reading waits for the next look
    }
}


void ScanSession::Impl::takeRockContents(const RockContents& read, const RockStats& rock) {
    if (!read.whole) return;
    std::string key, words;
    char text[96];
    for (const RockPartRead& part : read.parts) {
        std::snprintf(text, sizeof(text), "%s:%.2f:%d;", part.inert ? "-" : part.ore.c_str(), part.percent, part.quality);
        key += text;
        if (part.inert) std::snprintf(text, sizeof(text), "%.2f %% inert", part.percent);
        else std::snprintf(text, sizeof(text), "%.2f %% %s at %d", part.percent, part.ore.c_str(), part.quality);
        words += (words.empty() ? "" : ", ") + std::string(text);
    }
    if (key != contentsSeen_) {
        // Once: the next look says whether it stands. And a whole list that
        // is not the one on view says that one is another rock's.
        contentsSeen_ = key;
        logRockList("first", rock.mass, rock.mass, &read, contentsTries_, "waits for a second that agrees");
        if (contentsFor_ > 0 && key != contentsShown_) {
            logRockList("off", contentsFor_, rock.mass, nullptr, contentsTries_, "a whole list of another rock read");
            contentsFor_ = 0;
            pushClearContents();
        }
        return;
    }
    const bool said = contentsFor_ > 0 && key == contentsShown_;
    if (!said) logRockList("shown", rock.mass, rock.mass, &read, contentsTries_, "");
    contentsFor_ = rock.mass;
    contentsOre_ = rock.ore;
    contentsShown_ = key;
    contentsTries_ = 0;
    { Event ev; ev.kind = Event::Kind::SetContents; ev.contents = read; ev.rock = rock; events_.push_back(std::move(ev)); }
    if (!said) std::cout << "\n[rock] holds " << words << std::endl;
}


void ScanSession::Impl::loadPanelSpot(const cv::Size& viewport) {
    std::ifstream in(dataPath(PANEL_SPOT_FILE));
    if (!in.is_open()) return;
    int w = 0, h = 0, x = -1, y = -1, ink = 0;
    double scale = 0;
    std::string glyph;
    in >> w >> h >> x >> y >> scale >> glyph >> ink;
    if (!in || w != viewport.width || h != viewport.height || x < 0 || y < 0 || scale <= 0 || glyph.size() != 1) return;
    PanelSpot spot;
    spot.known = true;
    spot.labels = cv::Point(x, y);
    spot.scale = scale;
    spot.glyph = glyph[0];
    spot.ink = ink;
    setPanelSpot(spot);
    preferPanelInk(spot.ink);
    keptSpot_ = spot;
}


void ScanSession::Impl::keepPanelSpot(const cv::Size& viewport) {
    const PanelSpot spot = lastPanelSpot();
    if (!spot.known) return;
    // a few pixels of sway are the same place; the file is written for a
    // seat change, not for every rock
    if (keptSpot_.known && keptSpot_.glyph == spot.glyph && keptSpot_.ink == spot.ink &&
        std::abs(keptSpot_.scale - spot.scale) < 0.03 && std::abs(keptSpot_.labels.x - spot.labels.x) < 20 &&
        std::abs(keptSpot_.labels.y - spot.labels.y) < 20) {
        return;
    }
    std::ofstream out(dataPath(PANEL_SPOT_FILE), std::ios::trunc);
    if (!out.is_open()) return;
    out << viewport.width << ' ' << viewport.height << ' ' << spot.labels.x << ' ' << spot.labels.y << ' ' << spot.scale
        << ' ' << spot.glyph << ' ' << spot.ink << '\n';
    keptSpot_ = spot;
}


void ScanSession::Impl::readSignature() {
    // Messages are pumped between the candidates below, so a click on SCAN
    // or a hotkey can arrive while a scan runs - it must not start another
    // one inside this one
    // Clocked from here to the log line: a scan slower than the interval is
    // what the player experiences as a slow reading. The stages start at
    // zero because not every scan runs all of them, and carrying the last
    // scan's figure into a stage that never ran is how a log starts lying.
    const auto scanStart = std::chrono::steady_clock::now();
    g_captureMs = g_recordMs = g_trackMs = g_locateMs = 0.0;
    g_havePos = false;
    g_offX = g_offY = g_outside = 0;
    g_pinSlid = false;
    g_found = 0;
    g_bestPin = 0;
    g_font = "";
    auto msSince = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    struct ScanClock {
        std::chrono::steady_clock::time_point start;
        ~ScanClock() {
            g_scanMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    } scanClock{scanStart};

    // Debug: print the region once
    static bool showRegionOnce = true;
    if (showRegionOnce) {
        std::cout << "\n[Scan region: x=" << signatureRegion_.x << " y=" << signatureRegion_.y
                  << " w=" << signatureRegion_.width << " h=" << signatureRegion_.height << "]" << std::endl;
        showRegionOnce = false;
    }

    std::cout << "." << std::flush;  // scan heartbeat

    static int debugCounter = 0;
    static int unreadCounter = 0;
    // Recent readings, for the consistency window below - each with the
    // moment it was read, because six readings are not a length of time: a
    // scan that finds nothing puts nothing in the window, so through a quiet
    // stretch the six slots can span ten minutes.
    //
    // In a replayed scan log of ~3,600 readings, a confirmed reading found its
    // partner one second old half the time, and five seconds old at the 95th
    // percentile. The exceptions are the cockpit's own furniture. "111,000" is
    // the mining gauge - "0.00 SCU/S" with the gauge arc for a pin, named in
    // goodEnough above - and "10,110" is the MOLE's right-hand display, whose
    // thin tick marks read as ones and zeroes beside an icon that passes for a
    // pin at 81 %. Both are table values, so two agreeing readings are enough
    // to show them; both turn up once a minute or so, far outside the aim box,
    // at 62-81 %. One of eleven 111,000s there was confirmed by a reading 32
    // seconds older.
    //
    // 20 seconds is the bar. In that log every confirmed reading whose partner
    // was older - and which was not read again a moment later anyway - was one
    // of these phantoms, bar a 7,200 whose two readings were 19 and 16 seconds
    // apart. A real marker sits on a rock the player is looking at; it comes
    // back in the same breath, not in the next minute.
    static constexpr int RECENT_SLOTS = 6;
    static constexpr double RECENT_SECONDS = 20.0;
    // `alone` marks a value only the second reader saw. The fragment rule
    // below may not lean on such a value: where that reader takes the pin
    // for a leading figure, every correct reading afterwards looks like a
    // fragment of its mistake and is rewritten into it - a 26,800 then
    // holds the display for twenty seconds over a marker that says 6,800.
    struct RecentRead {
        int value = 0;
        std::chrono::steady_clock::time_point when{};
        bool alone = false;
    };
    static RecentRead recentReads[RECENT_SLOTS];
    static int recentPos = 0;

    // Hide the overlay before capturing (its frame would end up in the image!)
    // Only when it is actually up: in live mode it goes once and stays gone,
    // and waiting 50 ms on every scan for a window long gone costs a sixth of
    // the median scan on a 5120x1440 screen.
    if (hooks_.overlayVisible && hooks_.overlayVisible()) {
        if (hooks_.showOverlay) hooks_.showOverlay(false);
        Sleep(50);
    }

    // The game may have appeared, moved to another monitor or changed size
    // since the last scan: an automatic box follows it, and the reader's
    // scale follows the picture
    if (refreshAutoAim(false) && hooks_.aimMoved) hooks_.aimMoved();
    refreshHudScale();
    if (hooks_.beforeCapture) hooks_.beforeCapture();

    auto labelCandidates = captureSignatureCandidates(SCAN_CANDIDATES);
    g_found = static_cast<int>(labelCandidates.size());
    g_scanMs = msSince(scanStart);
    // The capture path is only known once something was captured; the
    // settings are written out here too, so debug/ carries them from the
    // first scan on, not only from the first orderly exit
    static bool sessionNoted = false;
    if (!sessionNoted) {
        sessionNoted = true;
        if (hooks_.firstCapture) hooks_.firstCapture();
    }


    // Show the overlay again outside of live mode
    if (!liveMode_ && hooks_.haveOverlay && hooks_.haveOverlay() && hooks_.showOverlay)
        hooks_.showOverlay(true);

    if (labelCandidates.empty()) {
        std::cout << "[no capture]" << std::flush;
        return;
    }

    // Debug: rolling buffer of the last 40 captures. The image is saved after
    // the candidate loop so it shows the capture the reading came from, not
    // whichever candidate happened to be first.
    const int debugSlot = debugCounter % 40;
    debugCounter++;

    // Try the candidates in order and keep the first one that reads as a real
    // table value. This is what makes tracking self-correcting: locking onto
    // some other warm HUD element simply yields no valid signature, so the
    // scan moves on to the next candidate.
    std::optional<int> recognizedValue;
    double confidence = 0.0;
    int agreement = 0;
    cv::Rect takenFrom = labelCandidates.front().screenRect;
    bool exactValue = false;
    bool fromGlyphs = false;
    bool repairedRead = false;
    // The template reader read a value the table does not know, and read it
    // cleanly: no hesitant glyph resolved, no pin slid into place. See the
    // read count below - this is what lets such a value be shown at all.
    bool cleanUnknown = false;
    double winningPin = 0.0;        // the pin beside the winning row, 0..1
    bool winningSeparator = false;  // its thousands comma was seen
    cv::Rect winningPlate;   // where the plate sat inside the winning crop
    int ocrTries = 0;
    cv::Mat winningImage = labelCandidates.front().image;


    // A hinted read that comes back weak gets one pass with the hint dropped
    // - but only for the candidates nearest the aim, where the marker is.
    // See GlyphReader::read; the pass costs as much as the first one.
    int secondChancesLeft = SECOND_CHANCE_CANDIDATES;
    // The second reader: how many candidates it may still read on its own
    // this scan, and what it said (for the scan log).
    // A line costs it 110-180 ms of processor time (one thread to six), and
    // away from the rocks the HUD offers pinned rows that are no marker on
    // most scans - "UNKNOWN", "TARGET", "70m/s" - which it would read twice
    // a scan, three scans a second: a core of the game's, for nothing. So
    // once nothing at all has been found for a while, the rows it has looked
    // at lately rest on every scan, and it reads one row a scan at most, a
    // new one: a marker just come into view is still read at once, and
    // another new row waits a scan.
    // From its first reading on - which counts as a find - it reads on
    // every scan again, as many rows as before.
    const bool linesIdle =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_lastFind).count() > LINE_IDLE_AFTER_S;
    int linesLeft = linesIdle ? LINE_IDLE_ROWS : TextLineReader::FALLBACK_CANDIDATES;
    auto lineKnown = [this](int v) { return policy_.explainsExactly(v); };
    g_line = "";
    g_lineMs = 0.0;
    // Both readers read this value off the same row: evidence enough for
    // a single frame, even where the glyph reader alone would have had to
    // repeat itself (172 of 172 such values right on the labelled frames)
    bool bothAgreed = false;
    // The marker-like candidates the glyph reader could not read, for the
    // second reader to try once the glyph reader has tried them all - not
    // before, or a row it gives up on would take the place of a later one
    // it reads cleanly
    struct LineTry {
        const LabelCandidate* candidate;
        GlyphReader::Reading reading;
    };
    std::vector<LineTry> lineTries;
    for (const auto& candidate : labelCandidates) {
        // Keep the window alive. A scan with twelve candidates and a
        // Tesseract pass or two held this thread for seconds, and every
        // message in that time - a drag of the title bar above all - waited
        // for it. The callbacks this can fire are deferred or harmless: SCAN
        // only sets a flag, a region edit takes effect with the next capture.
        breathe();

        // Template reader first: no OCR involved, and exact whenever the
        // plate and its white digits are intact. Tesseract only gets what
        // the template reader cannot segment.
        // Accepted when every glyph is a fair match and the row as a whole a
        // good one; the live consistency check still demands a second frame
        // unless the weakest glyph is strong.
        // The reader resolves its own hesitations against the signature
        // table, which is narrower than repairing the finished number: it
        // only ever swaps in the digit the classifier nearly chose.
        SCMiner::GlyphReader::Reading glyphReading;
        struct PinNote {   // the best pin of the scan, read or not
            const SCMiner::GlyphReader::Reading& r;
            ~PinNote() { g_bestPin = (std::max)(g_bestPin, static_cast<int>(r.pinScore * 100)); }
        } pinNote{glyphReading};
        auto isSignature = [this](int v) { return policy_.isSingleSignature(v); };
        auto isSum = [this](int v) { return policy_.isKnownSignature(v); };
        // where the ship is: Nyx I's own rocks read other values
        const std::vector<int> known = policy_.knownSignatures();
        if (auto glyphValue = liveGlyphs().read(candidate.image, &glyphReading, isSignature, &known, isSum,
                                                candidate.row.area() > 0 ? &candidate.row : nullptr,
                                                secondChancesLeft-- > 0)) {
            std::string glyphName;
            int glyphCluster = 0;
            // A reading that needed a hesitant glyph resolved has to be
            // cleaner than one read outright: in sun glare the digits bloat
            // into lumps that fit several templates, and letting those be
            // repaired turns a blur into a confident wrong number.
            // ...or read cleanly and known to no table (Reading::unknown): a
            // value the table lacks, shown as read once four scans agree,
            // with the nearest table entries beside it - never a repaired
            // neighbour of it.
            // One ore or one field, base times count, on the reader's own
            // bar; anything the table only reaches by adding two things - or
            // not at all - on the strict one (see strictlyGood)
            std::string exactName; int exactCount = 0;
            const bool single = policy_.isExactMatch(*glyphValue, exactName, exactCount);
            const bool sum = !single && policy_.isKnownSignature(*glyphValue);
            if (GlyphReader::goodEnough(glyphReading) &&
                (single || (sum && GlyphReader::wellPinned(glyphReading)) ||
                 (glyphReading.unknown && GlyphReader::strictlyGood(glyphReading))) &&
                !GlyphReader::ambiguousValue(glyphReading, [this](int v) { return policy_.isKnownSignature(v); },
                                             GlyphReader::ambiguityLead())) {
                recognizedValue = *glyphValue;
                confidence = glyphReading.weakest;
                // A two-ore sum is rarer than a single signature and is only
                // ever accepted as read, so it earns the slower path a
                // repaired reading takes: three agreeing scans, never one.
                // A pin that only matched once the comparison was allowed to
                // slide counts as weak evidence, the same as a repaired
                // glyph: shown only after the reading repeats itself.
                repairedRead = glyphReading.repaired || glyphReading.pinSlid ||
                               !policy_.isSingleSignature(*glyphValue);
                cleanUnknown = glyphReading.unknown && !glyphReading.repaired &&
                               !glyphReading.pinSlid && GlyphReader::strictlyGood(glyphReading);
                g_pinSlid = glyphReading.pinSlid;
                g_font = glyphReading.mfd ? "panel" : "canopy";
                // The ship from the log says which font to expect - the Drake's panel
                // face, everyone else's canopy face. The picture decides (the comma
                // gap, see GlyphReader); a disagreement is worth a line in the console
                // because it is either a new hull or a misread.
                if (!g_ship.empty() && g_drake != glyphReading.mfd) {
                    std::cout << "\nnote: Game.log says " << g_shipLabel << ", the row was read in the "
                              << (glyphReading.mfd ? "panel" : "canopy") << " font" << std::endl;
                }
                agreement = 4;
                takenFrom = candidate.screenRect;
                winningImage = candidate.image;
                exactValue = policy_.isKnownSignature(*glyphValue);
                fromGlyphs = true;
                winningPlate = glyphReading.plate;
                winningPin = glyphReading.pinScore;
                winningSeparator = glyphReading.separator;

                // The second reader reads the same row. On 230 labelled frames,
                // where both read, they agreed 172 times and were right every
                // time - so an agreed value stands on one frame. Where they
                // differ, TextLineReader::judge says who is heard: the second
                // reader overrules a glyph reader that hesitated over a digit or
                // lost one (Differs) - every wrong value the glyph reader showed
                // was of that kind - and its value is then held to what a repair
                // is held to: shown once later scans repeat it. Against a glyph
                // reader that was sure it is only noted (Doubts): in all four
                // such labelled cases the glyph reader was right.
                // Not asked again while the value it last agreed to stands
                // where it stood.
                const bool agreedHere = *glyphValue == agreedValue_ &&
                                        (candidate.screenRect & agreedRect_).area() > 0;
                if (sharedLines().ready() && !agreedHere && !candidate.source.empty()) {
                    const auto opinion = sharedLines().judge(candidate.source, candidate.origin, glyphReading,
                                                          *glyphValue, lineKnown);
                    g_lineMs += opinion.line.ms;
                    g_line = TextLineReader::name(opinion.verdict);
                    if (opinion.verdict == TextLineReader::Verdict::Agrees) {
                        agreedValue_ = *glyphValue;
                        agreedRect_ = candidate.screenRect;
                        bothAgreed = true;
                    } else if (opinion.verdict == TextLineReader::Verdict::Doubts) {
                        agreedValue_ = 0;
                    } else if (opinion.verdict == TextLineReader::Verdict::Differs) {
                        std::cout << "\n[second reader: " << opinion.value << " where the glyphs read "
                                  << *glyphValue << "]" << std::flush;
                        recognizedValue = opinion.value;
                        confidence = opinion.line.mean * 100.0;
                        agreement = 1;
                        exactValue = policy_.isKnownSignature(opinion.value);
                        fromGlyphs = false;
                        repairedRead = true;
                        cleanUnknown = false;
                        agreedValue_ = 0;
                    }
                } else if (agreedHere) {
                    g_line = "agreed before";
                    bothAgreed = true;
                    // ...and "where it stood" moves with it. Kept at the spot where
                    // the second reader first agreed, it would ask again as soon as
                    // the marker slid off it - every other scan while a rock is read
                    // from a moving ship, 40 ms each - only to hear the same value.
                    agreedRect_ = candidate.screenRect;
                }
                break;
            }
        }

        // No row of digits with the pin beside it means this is not a
        // signature marker at all - the OCR would only hallucinate a value
        // out of whatever text or instrument the tracker is looking at
        // Tesseract only gets a marker the pin vouches for; it turned a
        // "CPLD" button into 4,210 when asked on weaker evidence
        if (!glyphReading.markerLikely || glyphReading.pinScore < GlyphReader::OCR_MIN_PIN) continue;

        // A marker the glyph reader could not read: kept for the second
        // reader, which tries the first few of them once every candidate
        // has had its turn with the glyph reader (below)
        if (sharedLines().ready() && linesLeft > 0 && glyphReading.digitBox.area() > 0 &&
            !candidate.source.empty()) {
            // Resting is per ROW, not per scan. A row already looked
            // at and found empty gives the same answer again and is
            // left alone; a row not seen before is read at once,
            // resting or not - that is a marker just come into view.
            // With per-scan resting a new marker once went unread for
            // 55 scans (18 s), 22 of them spent resting.
            if (linesIdle && lineLookedAt(candidate.screenRect)) {
                if (g_line.empty()) g_line = "resting";
            } else {
                linesLeft--;
                lineTries.push_back({&candidate, glyphReading});
            }
        }

        // Tesseract on twelve candidates would blow the scan interval, and
        // the ones past the first few are the least likely anyway - they sit
        // furthest from both the previous marker and the aim point.
        // Tesseract may only CONFIRM the cutter (see below). When the
        // cutter's own value is no signature there is nothing to confirm, and
        // the scan's most expensive step is skipped where it could not help
        if (!policy_.isKnownSignature(glyphReading.value)) continue;

        if (++ocrTries > OCR_FALLBACK_CANDIDATES) continue;
        // Tesseract is opt-in (SCMINER_OCR=1): measured on 924 live frames
        // it confirmed none of the 68 readings, and one attempt held the
        // scan for up to 1.5 s.
        static const bool useOcr = DEV_ENV("SCMINER_OCR") != nullptr;
        if (!useOcr || !ocrAvailable_) continue;

        auto renderings = liveCapture().preprocessCandidates(candidate.image);

        auto value = liveOcr().recognizeBest(renderings, policy_);
        if (!value) continue;
        // The fallback only ever sees verified markers, so a reading this
        // uncertain is not a hard marker - it is glare the OCR guessed at,
        // and guesses that happen to land on a table value are the worst kind
        if (liveOcr().getLastConfidence() < MIN_OCR_CONFIDENCE) continue;
        // Tesseract may only CONFIRM what the cutter nearly read. Left to
        // its own devices over a rejected row it turned a "CPLD" button into
        // 4,210 and a chat line into 39,000 - both table values, both
        // shown with confidence. Two readers agreeing is evidence.
        if (*value != glyphReading.value) continue;

        std::string exactName;
        int exactCluster = 0;
        const bool exact = policy_.isKnownSignature(*value);

        if (exact || !recognizedValue) {
            recognizedValue = value;
            confidence = liveOcr().getLastConfidence();
            agreement = liveOcr().getLastAgreement();
            takenFrom = candidate.screenRect;
            winningImage = candidate.image;
            exactValue = exact;
            fromGlyphs = false;
        }
        if (exact) break;   // a table hit settles it
    }

    // Nothing the glyph reader would stand behind on any candidate: the
    // second reader reads the marker-like rows it kept. On the labelled
    // frames that found 25 values the glyph reader missed, 2 of them wrong
    // (a 6 read as 8, a 3 as 2) - so what it reads alone is held to what a
    // repair is held to: shown only once later scans repeat it.
    if (!recognizedValue) {
        for (const auto& tried : lineTries) {
            const LabelCandidate& candidate = *tried.candidate;
            const auto opinion = sharedLines().judge(candidate.source, candidate.origin, tried.reading, std::nullopt,
                                                  lineKnown);
            g_lineMs += opinion.line.ms;
            if (g_line.empty() || g_line == "none") g_line = TextLineReader::name(opinion.verdict);
            if (opinion.verdict != TextLineReader::Verdict::Reads) {
                rememberLineLook(candidate.screenRect);
                continue;
            }
            recognizedValue = opinion.value;
            confidence = opinion.line.mean * 100.0;
            agreement = 1;
            takenFrom = candidate.screenRect;
            winningImage = candidate.image;
            exactValue = policy_.isKnownSignature(opinion.value);
            fromGlyphs = false;
            repairedRead = true;
            cleanUnknown = false;
            winningPlate = tried.reading.plate;
            winningPin = tried.reading.pinScore;
            winningSeparator = tried.reading.separator;
            break;
        }
    }

    saveDebugCapture("region_" + std::to_string(debugSlot) + ".png", winningImage);

    // Remember where the winning label sits so the next scan starts there
    if (recognizedValue && lastScanTracked_) {
        lastLabelRect_ = takenFrom;
        hasLastLabel_ = true;
        lockMisses_ = 0;
        followOnly_ = 0;
        followMissed_ = false;
        lockLost_ = 0;
        // Track the PLATE, not the crop it was cut from. The crop carries a
        // margin of terrain and sky around the marker, and that margin is
        // exactly the part that changes when the view turns - matching it
        // dragged the correlation down to 0.3-0.6 and the lock fell off after
        // a couple of frames. The plate itself is the same pixels every time.
        cv::Rect plate = winningPlate & cv::Rect(0, 0, winningImage.cols, winningImage.rows);
        if (plate.width < 12 || plate.height < 6) plate = cv::Rect();
        if (!winningImage.empty()) {
            lockPatch_ = plate.area() > 0 ? winningImage(plate).clone() : winningImage.clone();
        }

    }

    // Where that marker sat against the box the player marked. Taken
    // once the winning candidate is settled, so it describes the reading
    // that is about to be logged and not whichever candidate came first.
    if (recognizedValue && takenFrom.area() > 0) {
        const cv::Rect aim(signatureRegion_.x, signatureRegion_.y,
                           signatureRegion_.width, signatureRegion_.height);
        g_offX = (takenFrom.x + takenFrom.width / 2) - (aim.x + aim.width / 2);
        g_offY = (takenFrom.y + takenFrom.height / 2) - (aim.y + aim.height / 2);
        g_outside = (std::max)({0,
                                aim.x - takenFrom.x,
                                takenFrom.x + takenFrom.width - (aim.x + aim.width),
                                aim.y - takenFrom.y,
                                takenFrom.y + takenFrom.height - (aim.y + aim.height)});
        g_havePos = true;
    }

    // Report where the label currently sits so the GUI can show it
    signatureOut_.haveTrack = true;
    signatureOut_.sawLabels = autoTrack_;
    signatureOut_.locked = lastScanTracked_ && recognizedValue.has_value();
    signatureOut_.trackX = takenFrom.x;
    signatureOut_.trackY = takenFrom.y;

    if (!recognizedValue) {
        std::cout << "x" << std::flush;  // OCR failed
        g_scanMs = msSince(scanStart);
        logScan(debugSlot, "no-read", 0, 0.0, 0);
        // Keep failures in their own ring: the normal buffer is overwritten
        // within seconds of scanning, so the frames worth investigating would
        // be gone by the time anyone looks at them
        saveDebugCapture("unread_" + std::to_string(unreadCounter % 20) + ".png",
                         labelCandidates.front().image);
        unreadCounter++;
        missStreak_++;
        return;
    }

    int scannedValue = *recognizedValue;   // already repaired by the reading policy

    // Valid signature range. Nothing above MAX_VALID_SIGNATURE exists in the
    // published data, but keep a margin so the GUI can show and explain an
    // out-of-range reading instead of dropping it silently.
    if (scannedValue < 1500 || scannedValue > MAX_VALID_SIGNATURE + 5000) {
        std::cout << "." << std::flush;  // out of range
        g_scanMs = msSince(scanStart);
        logScan(debugSlot, "out-of-range", scannedValue, confidence, agreement);
        return;
    }

    // "Moments ago" is what the window means: a reading older than
    // RECENT_SECONDS is not in it.
    const auto readAt = std::chrono::steady_clock::now();
    auto stillCounts = [&](const RecentRead& recent) {
        return recent.value != 0 &&
               std::chrono::duration<double>(readAt - recent.when).count() <= RECENT_SECONDS;
    };

    // A digit fallen off a value read moments ago is not a new value:
    // "17,080" with its thin 1 lost reads 7,080 (Beryl x2), "63,000" reads
    // 3000, "39,000" 3900. Each is what a recent reading becomes with its
    // first or last digit gone, and the table cannot object: every multiple
    // of 3000 or 4000 is a signature. So a reading that is such a fragment
    // of a value in the window counts FOR that value - the marker is still
    // there and still says the whole number - and never as a value of its
    // own.
    {
        auto fragmentOf = [](int part, int whole) {
            if (part <= 0 || whole <= part) return false;
            if (whole == part * 10) return true;                    // last digit gone
            for (int p = 10; p <= 100000 && p <= whole; p *= 10) {
                if (p > part && whole % p == part) return true;     // leading digit(s) gone
            }
            return false;
        };
        for (const RecentRead& recent : recentReads) {
            if (!stillCounts(recent) || recent.alone) continue;
            const int value = recent.value;
            if (fragmentOf(scannedValue, value)) {
                std::cout << "f" << std::flush;
                g_scanMs = msSince(scanStart);
                logScan(debugSlot, "fragment-of-" + std::to_string(value), scannedValue, confidence, agreement);
                scannedValue = value;
                exactValue = policy_.isKnownSignature(value);
                repairedRead = true;   // a fragment is never single-frame proof
                cleanUnknown = false;  // ...nor is it what the glyphs actually said
                bothAgreed = false;    // ...nor what the two readers agreed on
                break;
            }
        }
    }

    // Consistency check: how often this exact value appears among the recent
    // readings. Misreads scatter across different wrong values, so seeing the
    // identical number more than once is what separates them.
    //
    // This counts over a WINDOW rather than consecutive frames: the marker is
    // anchored to the rock, so with head tracking a good frame is routinely
    // followed by one that caught terrain. Demanding two reads in a row threw
    // away values that had already been read correctly.
    int consistentCount = 1;
    for (const RecentRead& recent : recentReads) {
        if (stillCounts(recent) && recent.value == scannedValue) consistentCount++;
    }
    recentReads[recentPos % RECENT_SLOTS] = {scannedValue, readAt, !fromGlyphs && !bothAgreed};
    recentPos++;

    // In-game values are always exact base x cluster multiples (exactValue was
    // determined while picking the label). A read that hits a real table value
    // only needs one confirmation; anything else (leftover misreads, aggregated
    // multi-cluster blips) must hold still for longer before it is shown.
    //
    // Signatures sometimes flash up for a single frame (in-game bug), so a
    // second frame to confirm may never come. Accept immediately when this one
    // frame is already strong evidence: every reading comes from a capture
    // whose pin was matched, so it IS a signature marker rather than some text
    // the tracker drifted onto. An exact table value on such a capture - read
    // either by the template reader or by several agreeing OCR renderings -
    // needs no repetition.
    // A reading whose ambiguous glyph had to be resolved against the table
    // is NOT single-frame proof. Two table values can fit the same picture -
    // at 13 px a 6 and an 8 score alike - and showing whichever won this
    // frame makes the display flip between them. Let the window decide which
    // one keeps coming back.
    // A reading that matches nothing in the table is normally noise. But the
    // signature data is not guaranteed complete - 39,350 was read identically
    // by twenty independent renderings at 87 %, and no entry explains it - so
    // a reading that strong is shown as an unknown signature rather than
    // swallowed. The GUI lists the nearest known values beside it.
    // What makes an unknown strong is HOW its glyphs matched (cleanUnknown),
    // not a count of agreeing renderings: the template reader hands back one
    // reading, and asking an unknown for four agreeing scans is more than
    // flying past a rock gives.
    const bool strongUnknown = !exactValue && cleanUnknown;

    // A single frame may settle it only when the reading is not merely valid
    // but CLEAN.
    //
    // With the locator's wide reach, markers beside the sun are found in 9 of
    // 21 recorded frames, and two of those nine read wrong: a 10,000 as 7080,
    // an 8,390 as 8360. Both are real values in the signature table, so
    // nothing downstream can tell them from a correct reading - and a wrong
    // number sends the player to the wrong rock, which is worse than no number.
    //
    // What separates them is how well the glyphs actually matched. A marker
    // stays on screen for many scans, so demanding a second frame for anything
    // less than clean costs a third of a second and no readings at all, while
    // a misread rarely repeats itself the same way twice.
    //
    // The bar is 90, not 95: some real markers stand in the picture for
    // exactly one scan (64,000 and 21,200 on live footage), and 95 with pin
    // and comma would hide them.
    //
    // An unknown value never settles on one frame, though: two readers can
    // agree once on HUD text such as "2074".
    constexpr double SINGLE_FRAME_CONFIDENCE = 90.0;
    // ...or when both readers read it: two readers that fail in different
    // ways agreeing on a table value is stronger than any glyph score (see
    // bothAgreed above)
    const bool singleFrameProof = (exactValue && !repairedRead &&
                                   confidence >= SINGLE_FRAME_CONFIDENCE &&
                                   (fromGlyphs || agreement >= 3)) ||
                                  (exactValue && bothAgreed);
    (void)winningPin;
    (void)winningSeparator;

    // A repaired reading repeats itself: the same plate in the same light
    // is read the same wrong way again ("16,960" repaired to 18,000 twice in
    // ten seconds), so a repair needs three matching reads, not two.
    // A value no table knows is still never shown off one frame - two
    // readers can agree once on HUD text such as "2074" - but a clean one
    // asks for two scans rather than four.
    const int requiredReads = exactValue ? (repairedRead ? 3 : 2)
                                         : (strongUnknown ? 2 : 4);

    if (!singleFrameProof && consistentCount < requiredReads) {
        std::cout << "?" << std::flush;  // uncertain, wait for confirmation
        g_scanMs = msSince(scanStart);
        logScan(debugSlot, "held-back", scannedValue, confidence, agreement);
        return;
    }

    std::cout << "\n[OCR: " << scannedValue << " (" << (int)confidence << "%)]" << std::flush;
    g_scanMs = msSince(scanStart);
    logScan(debugSlot, singleFrameProof ? "accepted-flash" : "accepted",
            scannedValue, confidence, agreement);

    // A signature read while the window shows a rock whose panel the last
    // look did not find: the player is scanning again
    if (rockSeen_ && rockHold_.signatureRead()) rockGone(" - a signature was read");

    // Has the lock settled on something that never moves? Then it is cockpit
    // furniture wearing a marker's clothes, and holding on to it costs every
    // real marker on screen - the search never looks anywhere else again.
    if (scannedValue == stuckValue_ &&
        std::abs(takenFrom.x - stuckRect_.x) <= STUCK_DRIFT_PX &&
        std::abs(takenFrom.y - stuckRect_.y) <= STUCK_DRIFT_PX) {
        stuckCount_++;
    } else {
        stuckValue_ = scannedValue;
        stuckRect_ = takenFrom;
        stuckCount_ = 1;
    }
    if (stuckCount_ >= STUCK_LIMIT) {
        std::cout << "\n[stuck on " << scannedValue << " at " << takenFrom.x << ","
                  << takenFrom.y << " - cockpit, not a rock; ignoring it for a while]"
                  << std::endl;
        blacklistRect_ = takenFrom;
        blacklistLeft_ = BLACKLIST_SCANS;
        hasLastLabel_ = false;
        lockPatch_.release();
        lockMisses_ = 0;
        missStreak_ = REACQUIRE_AFTER_MISSES;   // sweep the whole screen next
        stuckCount_ = 0;
        stuckValue_ = 0;
        lastRecognizedSignature_ = 0;
    }

    // Only update on change
    if (scannedValue == lastRecognizedSignature_) {
        return;
    }

    lastRecognizedSignature_ = scannedValue;

    signatureOut_.updated = true;
    signatureOut_.value = scannedValue;
    signatureOut_.confidence = confidence;
}


bool ScanSession::Impl::refreshAutoAim(bool announce) {
    if (!aimAuto_) return false;
    cv::Rect viewport;
    if (auto found = ScreenCapture::gameViewport()) {
        viewport = *found;
    } else {
        // No game window yet: the primary monitor, until there is one
        viewport = cv::Rect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
    if (viewport == aimViewport_) return false;
    aimViewport_ = viewport;
    int margin = trackMargin_;
    signatureRegion_ = defaultAimBox(viewport, &margin);
    trackMargin_ = margin;
    hasLastLabel_ = false;   // the box moved, tracking starts over
    lockPatch_.release();
    if (announce) {
        std::cout << "Aim box from the " << viewport.width << "x" << viewport.height << " viewport at "
                  << viewport.x << "," << viewport.y << ": " << signatureRegion_.x << ","
                  << signatureRegion_.y << " " << signatureRegion_.width << "x" << signatureRegion_.height
                  << ", margin " << trackMargin_ << std::endl;
    }
    return true;
}


cv::Rect ScanSession::Impl::allowedArea() const {
    // The signature always appears in one part of the HUD. The user marks that
    // part with the overlay, and this is the hard boundary drawn from it: that
    // region plus the tracking margin, and nothing else, ever.
    //
    // A search that may sweep the whole monitor locks onto anything
    // plate-shaped: the power management panel, the mining laser readout, the
    // status pills in the corner, chat. All of those produce digits, and
    // digits that resolve against the signature table get shown as a reading.
    // Bounding the search stops that at the source instead of trying to tell
    // a real reading from a false one after the fact.
    const cv::Rect aim(signatureRegion_.x, signatureRegion_.y,
                       signatureRegion_.width, signatureRegion_.height);
    cv::Rect area(aim.x - trackMargin_, aim.y - trackMargin_,
                  aim.width + 2 * trackMargin_, aim.height + 2 * trackMargin_);

    // ...and inside the game picture, never the desktop, the taskbar or a
    // window in front of it
    const POINT aimCentre{aim.x + aim.width / 2, aim.y + aim.height / 2};
    cv::Rect screen(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (HMONITOR monitor = MonitorFromPoint(aimCentre, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
            screen = cv::Rect(info.rcMonitor.left, info.rcMonitor.top,
                              info.rcMonitor.right - info.rcMonitor.left,
                              info.rcMonitor.bottom - info.rcMonitor.top);
        }
    }
    if (auto viewport = ScreenCapture::gameViewport()) {
        if (viewport->contains(cv::Point(aimCentre.x, aimCentre.y))) screen = *viewport;
    }

    area &= screen;
    return area;
}


CaptureRegion ScanSession::Impl::trackingSearchArea() const {
    const cv::Rect aim(signatureRegion_.x, signatureRegion_.y,
                       signatureRegion_.width, signatureRegion_.height);
    const cv::Rect screen = allowedArea();

    // Lost the marker for a while? Search the whole ALLOWED AREA again - the
    // marked region and its margin, not the monitor. That is what the boundary
    // is for: re-acquiring may look anywhere the signature can legitimately
    // appear, and nowhere else.
    if (missStreak_ >= REACQUIRE_AFTER_MISSES) {
        missStreak_ = 0;
        CaptureRegion whole;
        whole.x = screen.x;
        whole.y = screen.y;
        whole.width = screen.width;
        whole.height = screen.height;
        return whole;
    }

    // Pinned to the marker while the lock holds, wide around the aim point
    // once it is gone
    cv::Rect anchor = aim;
    int margin = trackMargin_;
    if (hasLastLabel_ && (lastLabelRect_ & screen).area() > 0) {
        anchor = lastLabelRect_;
        margin = (std::max)(LOCK_MARGIN_PX, anchor.height * 3);
        // The picture was not where it was left - a head turn, most likely.
        // Look further before concluding the marker is gone.
        if (followMissed_) margin *= LOCK_WIDEN_FACTOR;
    }


    cv::Rect area(anchor.x - margin, anchor.y - margin,
                  anchor.width + 2 * margin, anchor.height + 2 * margin);
    area &= screen;

    CaptureRegion region;
    region.x = area.x;
    region.y = area.y;
    region.width = area.width;
    region.height = area.height;
    return region;
}


void ScanSession::Impl::keepRecording() {
    const std::string dir = dataPath("debug/recording");
    const bool wasRunning = liveRecorder().active();

    // Nothing is being recorded, so the clips on disk are from some earlier
    // session. Copying those and beeping success is worse than doing nothing:
    // it hands over a folder that looks like the moment just captured and is
    // not.
    if (!wasRunning) {
        std::cout << "\n[recorder] not running - nothing to keep. "
                     "Live mode records; turn it on first." << std::endl;
        Beep(400, 160);
        Beep(300, 320);
        return;
    }

    // Finalise the clip being written: the most recent seconds - the ones
    // worth keeping - are still inside ffmpeg until its input is closed.
    liveRecorder().stop();

    std::time_t now = std::time(nullptr);
    std::tm parts{};
    localtime_s(&parts, &now);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &parts);
    const std::string target = dir + "/keep_" + stamp;

    std::error_code ec;

    // Room for the copy? The ring is up to ~450 MB, and a drive filled by
    // a diagnostic recording is worse than a recording lost.
    std::uintmax_t ringBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fec;
        if (entry.is_regular_file(fec) && entry.path().extension() == ".mkv") ringBytes += entry.file_size(fec);
    }
    if (freeBytesAt(dir) < ringBytes + 512ull * 1024 * 1024) {
        std::cout << "\n[recorder] not enough free space to keep the recording" << std::endl;
        Beep(400, 160);
        Beep(300, 320);
        liveRecorder().start(dir, 1000.0 / (std::max)(1, captureIntervalMs_),
                        RECORD_SEGMENT_SECONDS, RECORD_SEGMENTS);
        return;
    }

    std::filesystem::create_directories(target, ec);

    int kept = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mkv") continue;
        std::filesystem::copy_file(entry.path(),
                                   target + "/" + entry.path().filename().string(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) kept++;
    }

    // The copies are capped - see maxKeeps_. Oldest first: the stamp in the
    // name sorts them.
    std::vector<std::filesystem::path> keeps;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fec;
        if (entry.is_directory(fec) && entry.path().filename().string().rfind("keep_", 0) == 0) {
            keeps.push_back(entry.path());
        }
    }
    std::sort(keeps.begin(), keeps.end());
    std::uintmax_t total = 0;
    for (const auto& keep : keeps) total += folderBytes(keep);
    const std::uintmax_t keepBudget = static_cast<std::uintmax_t>(maxKeepGb_) << 30;
    while (keeps.size() > 1 &&
           (static_cast<int>(keeps.size()) > maxKeeps_ || total > keepBudget)) {
        total -= folderBytes(keeps.front());
        std::filesystem::remove_all(keeps.front(), ec);
        std::cout << "[recorder] dropped old " << keeps.front().filename().string() << std::endl;
        keeps.erase(keeps.begin());
    }

    // Audible, because the whole point is not having to leave the game to see
    // whether it worked: two rising notes for saved, two falling for nothing.
    std::cout << "\n[recorder] kept " << kept << " clip(s) in " << target << std::endl;
    if (kept > 0) {
        Beep(1200, 90);
        Beep(1600, 90);
    } else {
        Beep(400, 160);
        Beep(300, 320);
    }

    liveRecorder().start(dir, 1000.0 / (std::max)(1, captureIntervalMs_),
                    RECORD_SEGMENT_SECONDS, RECORD_SEGMENTS);
}


void ScanSession::Impl::trackCameraMotion(const cv::Mat& view) {
    // The marker hangs on a rock in the world. When the head turns it slides
    // across the screen - and so does everything else the world contains:
    // terrain, the horizon, the other contacts, the compass ribbon. That whole
    // shift IS the camera movement, and phase correlation reads it straight
    // out of two consecutive frames without knowing what any of it depicts.
    //
    // So the box does not need to recognise the marker again to stay on it;
    // it needs to move exactly as far as the view moved. Recognition then
    // only has to succeed once.
    //
    // This works because the area is captured at a FIXED rectangle, so a
    // shift measured between two frames is a shift on screen, with no window
    // bookkeeping in between.
    // Nothing to move without a lock: the shift measured then goes nowhere,
    // and measuring it costs 36 ms a scan over the 1152 x 965 area.
    // The view kept from the last scan is let go too; the scan that finds a
    // marker starts the pair afresh, and the box it draws is fresh anyway.
    if (!hasLastLabel_) {
        previousView_.release();
        return;
    }
    cv::Mat grey;
    cv::cvtColor(view, grey, cv::COLOR_BGR2GRAY);
    grey.convertTo(grey, CV_32F, 1.0 / 255.0);

    if (previousView_.size() == grey.size() && hasLastLabel_) {
        // Measured live while the view was turning: one correlation over the
        // whole area answers with a confidence of 0.26, while single
        // horizontal bands answer with 0.57 to 0.66 - and all of them agree on
        // the shift to within half a pixel. The area contains things that do
        // NOT move with the view (the canopy frame, the ship's instruments)
        // and they drag a single whole-picture correlation towards zero.
        //
        // So each band is asked separately and the median of those that answer
        // confidently is taken. The median is the point: a band that gets
        // fooled by a static instrument cannot move the result, because the
        // bands that saw the world outvote it.
        std::vector<double> dx, dy;
        const int bandHeight = grey.rows / CAMERA_BANDS;
        for (int b = 0; bandHeight > 8 && b < CAMERA_BANDS; b++) {
            const cv::Rect strip(0, b * bandHeight, grey.cols, bandHeight);
            double response = 0.0;
            const cv::Point2d shift =
                cv::phaseCorrelate(previousView_(strip), grey(strip), cv::noArray(), &response);
            if (response < CAMERA_MIN_RESPONSE) continue;
            if (std::abs(shift.x) >= CAMERA_MAX_SHIFT_PX ||
                std::abs(shift.y) >= CAMERA_MAX_SHIFT_PX) continue;
            dx.push_back(shift.x);
            dy.push_back(shift.y);
        }

        // Too few bands agreeing means the picture changed rather than moved -
        // a menu opening, a cut, the area going dark. Then the box stays where
        // it is, which is never worse than moving it on a guess.
        if (dx.size() >= CAMERA_MIN_BANDS) {
            std::sort(dx.begin(), dx.end());
            std::sort(dy.begin(), dy.end());
            lastLabelRect_.x += static_cast<int>(std::lround(dx[dx.size() / 2]));
            lastLabelRect_.y += static_cast<int>(std::lround(dy[dy.size() / 2]));
        }
    }
    previousView_ = grey;
}


void ScanSession::Impl::followLockedMarker(const cv::Mat& searchImage, const CaptureRegion& search) {
    followedNow_ = false;
    if (!hasLastLabel_ || lockPatch_.empty() || searchImage.empty()) return;
    if (lockPatch_.type() != searchImage.type()) return;
    if (searchImage.cols < lockPatch_.cols || searchImage.rows < lockPatch_.rows) return;
    // A full-monitor sweep is both far too big to match a template across and
    // the wrong situation for it: sweeping means the marker has been gone for
    // a while, not that it moved a little since the last scan.
    if (static_cast<long long>(searchImage.total()) > 1200000LL) return;

    // About where the lock was - the camera's shift is already in it - and
    // not over the whole area: the marker moves a few pixels between scans,
    // and a match over the whole 1152 x 965 area costs 189 ms a scan where
    // one over 420 x 340 costs 19. When the last follow missed, the whole
    // area is searched.
    cv::Rect window(0, 0, searchImage.cols, searchImage.rows);
    if (!followMissed_) {
        const cv::Rect about(lastLabelRect_.x - search.x - LOCK_FOLLOW_WINDOW, lastLabelRect_.y - search.y - LOCK_FOLLOW_WINDOW,
                             lastLabelRect_.width + 2 * LOCK_FOLLOW_WINDOW, lastLabelRect_.height + 2 * LOCK_FOLLOW_WINDOW);
        const cv::Rect clipped = about & window;
        if (clipped.width > lockPatch_.cols + 8 && clipped.height > lockPatch_.rows + 8) window = clipped;
    }
    cv::Mat corr;
    cv::matchTemplate(searchImage(window), lockPatch_, corr, cv::TM_CCOEFF_NORMED);
    double best = 0.0;
    cv::Point at;
    cv::minMaxLoc(corr, nullptr, &best, nullptr, &at);
    at += window.tl();
    if (best < LOCK_FOLLOW_NCC) {
        followMissed_ = true;   // widen the window for the next scan
        // ...and a picture found nowhere for LOCK_LOST_AFTER scans has left
        // the screen: the lock goes with it
        if (++lockLost_ >= LOCK_LOST_AFTER) {
            hasLastLabel_ = false;
            lockPatch_.release();
            lockLost_ = 0;
            lockMisses_ = 0;
            followOnly_ = 0;
            followMissed_ = false;
        }
        return;
    }

    lockLost_ = 0;
    followMissed_ = false;
    followedNow_ = true;
    lastLabelRect_ = cv::Rect(at.x + search.x, at.y + search.y,
                              lockPatch_.cols, lockPatch_.rows);

    // Finding the plate again IS the confirmation. The value it carries has
    // not changed just because this frame could not be read - the marker is
    // still there, in view, and now the frame is back on top of it. Letting
    // the lock die here throws the search back onto the dashboard every time
    // the light changes.
    lockMisses_ = 0;
    if (++followOnly_ > LOCK_FOLLOW_PATIENCE) {
        hasLastLabel_ = false;
        lockPatch_.release();
        followOnly_ = 0;
    }
}


std::vector<ScanSession::Impl::LabelCandidate> ScanSession::Impl::captureSignatureCandidates(int maxCandidates) {
    const cv::Rect fixedRect(signatureRegion_.x, signatureRegion_.y,
                             signatureRegion_.width, signatureRegion_.height);

    auto fixedOnly = [&]() -> std::vector<LabelCandidate> {
        lastScanTracked_ = false;
        cv::Mat image = grabRegion(signatureRegion_);
        if (image.empty()) return {};
        return {{image, fixedRect, cv::Rect(), image, cv::Point()}};
    };

    if (!autoTrack_) {
        return fixedOnly();
    }

    // Always the same rectangle: the whole allowed area. A window that moves
    // with the lock makes every frame incomparable with the last, and
    // comparing them is exactly how the view movement is measured below.
    const cv::Rect bounds = allowedArea();
    CaptureRegion search;
    search.x = bounds.x;
    search.y = bounds.y;
    search.width = bounds.width;
    search.height = bounds.height;
    if (search.width < 40 || search.height < 20) {
        return fixedOnly();
    }

    const auto stageClock = std::chrono::steady_clock::now();
    auto stageMs = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t).count();
    };
    cv::Mat searchImage = grabRegion(search);
    g_captureMs = stageMs(stageClock);
    // Every frame is wider than the pipe to ffmpeg, so this waits for the
    // encoder on every scan. Worth seeing as its own number: recording is
    // a diagnosis tool, and it should be obvious what it costs to leave on.
    const auto recStart = std::chrono::steady_clock::now();
    liveRecorder().write(searchImage);
    g_recordMs = stageMs(recStart);
    if (captureStats_.empty()) noteCapture(searchImage);
    breathe();   // the capture is in; let the window breathe
    if (searchImage.empty()) {
        lastScanTracked_ = false;
        hasLastLabel_ = false;
        return {};
    }

    // Before anything is located: drag the lock onto wherever the marker has
    // moved to since the last scan. Done here rather than only when the scan
    // comes up empty, because it serves three purposes at once - it keeps the
    // window travelling with the marker through unreadable frames, it gives
    // the candidate ordering below the marker's CURRENT position to measure
    // against, and it covers the case where candidates were found but none of
    // them read. A successful reading overwrites it further down anyway.
    // First how far the view turned - that applies whether or not anything is
    // recognisable this frame - then refine on the marker itself if it can be
    // found. The order matters: the picture match starts from the position the
    // camera movement already corrected.
    const auto trackStart = std::chrono::steady_clock::now();
    trackCameraMotion(searchImage);
    followLockedMarker(searchImage, search);
    g_trackMs = stageMs(trackStart);
    if (blacklistLeft_ > 0) blacklistLeft_--;

    // How much of the captured area the locator is handed - worked out after
    // the tracking, so that a lock stands where the marker is now. Everything
    // on every WIDE_SWEEP_EVERY-th scan, and whenever the follow lost the
    // marker this scan. Otherwise:
    //  - nothing read for a while and no lock: the area around the aim box -
    //    see NEAR_SWEEP_AFTER;
    //  - a marker locked and found again: the area around the aim box and
    //    around the marker. Searching the whole area here costs 115 ms of a
    //    170 ms scan for the one marker the lock already has; a new one comes
    //    up at the aim box, where the player turns to it, and one anywhere
    //    else is picked up by the next wide sweep.
    cv::Rect lookIn(0, 0, searchImage.cols, searchImage.rows);
    {
        const double quiet = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_lastFind).count();
        static int sweepTick = 0;
        const bool wide = (++sweepTick % WIDE_SWEEP_EVERY) == 0;
        // Not "near": <windows.h> still defines that as nothing at all
        const cv::Rect aimArea(signatureRegion_.x - search.x - NEAR_SWEEP_MARGIN,
                               signatureRegion_.y - search.y - NEAR_SWEEP_MARGIN,
                               signatureRegion_.width + 2 * NEAR_SWEEP_MARGIN,
                               signatureRegion_.height + 2 * NEAR_SWEEP_MARGIN);
        cv::Rect around;
        if (!wide && !hasLastLabel_ && quiet > NEAR_SWEEP_AFTER) {
            around = aimArea & lookIn;
        } else if (!wide && hasLastLabel_ && followedNow_) {
            const cv::Rect lockArea(lastLabelRect_.x - search.x - NEAR_SWEEP_MARGIN,
                                    lastLabelRect_.y - search.y - NEAR_SWEEP_MARGIN,
                                    lastLabelRect_.width + 2 * NEAR_SWEEP_MARGIN,
                                    lastLabelRect_.height + 2 * NEAR_SWEEP_MARGIN);
            around = (aimArea | lockArea) & lookIn;
        }
        if (around.width > 40 && around.height > 20) lookIn = around;
    }

    breathe();   // ...and again after the tracking, before the locator
    const auto locStart = std::chrono::steady_clock::now();
    const cv::Mat lookImage = (lookIn.width == searchImage.cols && lookIn.height == searchImage.rows)
                                  ? searchImage : searchImage(lookIn);
    auto labels = liveCapture().locateSignatureLabels(lookImage, maxCandidates);
    g_locateMs = stageMs(locStart);

    if (labels.empty()) {
        // Nothing in view right now. Keep the lock for a while: a marker
        // blinks out for a frame all the time, and letting go immediately
        // sends the search back to the wide area, where it picks up chat text.
        // Only a run of empty scans really means the marker is gone.
        lastScanTracked_ = false;
        if (hasLastLabel_ && ++lockMisses_ > LOCK_PATIENCE) {
            hasLastLabel_ = false;
            lockMisses_ = 0;
            lockPatch_.release();
        }
        cv::Rect local(signatureRegion_.x - search.x, signatureRegion_.y - search.y,
                       signatureRegion_.width, signatureRegion_.height);
        local &= cv::Rect(0, 0, searchImage.cols, searchImage.rows);
        if (local.width > 4 && local.height > 4) {
            return {{searchImage(local).clone(), fixedRect, cv::Rect(), searchImage, local.tl()}};
        }
        return fixedOnly();
    }

    // Several rocks can be marked at once, and other warm HUD elements can
    // look like markers. Order by plausibility: prefer the label the previous
    // scan used (stable readings while the view drifts), then the one closest
    // to the aim point the user configured.
    const cv::Point aim(signatureRegion_.x + signatureRegion_.width / 2 - search.x - lookIn.x,
                        signatureRegion_.y + signatureRegion_.height / 2 - search.y - lookIn.y);
    const cv::Point previous(hasLastLabel_
                                 ? lastLabelRect_.x + lastLabelRect_.width / 2 - search.x - lookIn.x
                                 : aim.x,
                             hasLastLabel_
                                 ? lastLabelRect_.y + lastLabelRect_.height / 2 - search.y - lookIn.y
                                 : aim.y);

    std::sort(labels.begin(), labels.end(),
              [&](const cv::Rect& a, const cv::Rect& b) {
                  auto cost = [&](const cv::Rect& r) {
                      const cv::Point c(r.x + r.width / 2, r.y + r.height / 2);
                      const double toAim = cv::norm(c - aim);
                      const double toPrev = hasLastLabel_ ? cv::norm(c - previous) : toAim;
                      // Weight continuity higher than the configured aim point
                      return toPrev * 0.7 + toAim * 0.3;
                  };
                  return cost(a) < cost(b);
              });

    std::vector<LabelCandidate> candidates;
    for (const auto& label : labels) {
        if (static_cast<int>(candidates.size()) >= maxCandidates) break;
        const cv::Rect onScreen(label.x + lookIn.x + search.x, label.y + lookIn.y + search.y,
                                label.width, label.height);
        // Outside the marked region it is not a signature, whatever it looks
        // like. Checked per candidate as well as per window, because the
        // window can be widened past the boundary while chasing a head turn.
        if ((onScreen & bounds).area() < onScreen.area()) continue;
        // Skip whatever the scan proved to be cockpit furniture, until the
        // ban runs out - otherwise the very next sweep locks onto it again
        if (blacklistLeft_ > 0 && (onScreen & blacklistRect_).area() > 0) continue;
        cv::Rect row;
        if (auto r = liveCapture().rowOf(label)) {
            row = cv::Rect(r->x - label.x, r->y - label.y, r->width, r->height);
        }
        candidates.push_back({lookImage(label).clone(), onScreen, row, lookImage, label.tl()});
    }

    lastScanTracked_ = !candidates.empty();
    return candidates;
}


cv::Mat ScanSession::Impl::captureSignatureImage(cv::Rect* outScreenRect) {
    auto candidates = captureSignatureCandidates(1);
    if (candidates.empty()) {
        return {};
    }

    if (lastScanTracked_) {
        lastLabelRect_ = candidates.front().screenRect;
        hasLastLabel_ = true;
        lockPatch_ = candidates.front().image.clone();
    }
    if (outScreenRect) *outScreenRect = candidates.front().screenRect;
    return candidates.front().image;
}


bool ScanSession::Impl::refreshHudScale() {
    const cv::Rect picture = pictureFor(signatureRegion_);
    double scale = picture.height > 0
        ? static_cast<double>(picture.height) / GlyphReader::REFERENCE_VIEWPORT_HEIGHT
        : 1.0;
    if (hudScalePercent_ > 0) scale = hudScalePercent_ / 100.0;   // forced by the config
    if (std::abs(scale - hudScale_) < 0.005 && std::abs(GlyphReader::hudScale() - scale) < 0.005) {
        return false;
    }
    hudScale_ = scale;
    GlyphReader::setHudScale(scale);
    // The lock patch was cut at the old scale
    hasLastLabel_ = false;
    lockPatch_.release();
    std::ostringstream line;
    line << "HUD scale " << std::fixed << std::setprecision(2) << scale << " (picture "
         << picture.width << "x" << picture.height << ", reference "
         << GlyphReader::REFERENCE_VIEWPORT_HEIGHT << " px tall)";
    std::cout << line.str() << std::endl;
    return true;
}


cv::Mat ScanSession::Impl::grabGamePicture() {
    const cv::Rect picture = pictureFor(signatureRegion_);
    CaptureRegion whole;
    whole.x = picture.x;
    whole.y = picture.y;
    whole.width = picture.width;
    whole.height = picture.height;
    const bool overlayShown = hooks_.overlayVisible && hooks_.overlayVisible();
    if (overlayShown && hooks_.showOverlay) {
        hooks_.showOverlay(false);
        Sleep(50);
    }
    cv::Mat frame = liveCapture().captureRegion(whole);
    if (overlayShown && hooks_.showOverlay) hooks_.showOverlay(true);
    return frame;
}


void ScanSession::Impl::noteCapture(const cv::Mat& image) {
    if (image.empty()) {
        captureStats_ = "empty - nothing came back from the capture";
        return;
    }
    cv::Mat gray;
    if (image.channels() == 1) gray = image;
    else cv::cvtColor(image, gray, image.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    const double black = 100.0 * cv::countNonZero(gray <= 5) / gray.total();
    const double white = 100.0 * cv::countNonZero(gray >= 250) / gray.total();
    std::ostringstream s;
    s << image.cols << "x" << image.rows << ", mean brightness " << std::fixed << std::setprecision(1)
      << cv::mean(gray)[0] << ", black " << black << " %, white " << white << " %";
    captureStats_ = s.str();
}


// Appended to ScanSession.cpp inside namespace SCMiner.

namespace {

ScreenCapture g_capture;
TextLineReader g_lines;
OCREngine g_ocr;
GlyphReader g_glyphs;
FrameRecorder g_recorder;

}  // namespace

ScreenCapture& liveCapture() { return g_capture; }
TextLineReader& sharedLines() { return g_lines; }
OCREngine& liveOcr() { return g_ocr; }
GlyphReader& liveGlyphs() { return g_glyphs; }
FrameRecorder& liveRecorder() { return g_recorder; }

cv::Mat ScanSession::Impl::grabRegion(const CaptureRegion& region) {
    cv::Mat frame = liveCapture().captureRegion(region);
    if (frame.empty() || covers_.empty()) return frame;
    std::vector<cv::Rect> extra;
    extra.reserve(covers_.size());
    for (const Rect& cover : covers_) extra.emplace_back(cover.x, cover.y, cover.w, cover.h);
    ScreenCapture::maskCovered(frame, cv::Rect(region.x, region.y, region.width, region.height), extra);
    return frame;
}

void ScanSession::Impl::breathe() {
    if (hooks_.breathe) hooks_.breathe();
}

void ScanSession::Impl::flagNote(const RockStats& raw, int rows, float labelScale) {
    if (events_.empty() || events_.back().kind != Event::Kind::UpdateRock) return;
    events_.back().noteScan = true;
    events_.back().rawLook = raw;
    events_.back().rows = rows;
    events_.back().labelScale = labelScale;
}

void ScanSession::Impl::pushClearRock() {
    Event ev;
    ev.kind = Event::Kind::ClearRock;
    events_.push_back(std::move(ev));
}

void ScanSession::Impl::pushClearContents() {
    Event ev;
    ev.kind = Event::Kind::ClearContents;
    events_.push_back(std::move(ev));
}

void ScanSession::Impl::readLoadoutOnHud(const cv::Mat& frame, const ScanPanel& panel) {
    if (!sharedLines().ready()) return;
    const char glyph = panel.labels.x >= 0 ? panel.labelGlyph : askInSeat_ ? 0 : panel.tryGlyph;
    const char* kindId = hudShipOf(glyph, askShip_, askInSeat_);
    if (kindId) {
        const std::string seat = g_ship + "|" + askHull_ + "|" + (askInSeat_ ? "in" : "out") + "|" +
                                 std::to_string(askBoardings_) + "|" + kindId;
        if (seat != loadoutSeat_) {
            loadoutSeat_ = seat;
            loadoutSeen_.clear();
            loadoutSaid_.clear();
            loadoutAgreeing_ = 0;
            loadoutUnsure_ = 0;
            loadoutDisagreeing_ = 0;
            loadoutNoHud_ = false;
            loadoutSettled_ = false;
            lastLoadoutRead_ = {};
        }
    }
    if (loadoutJob_.valid()) {
        if (loadoutJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        SeenLoadout seen;
        try {
            seen = loadoutJob_.get();
        } catch (const std::exception& e) {
            std::cerr << "[loadout] reading failed: " << e.what() << std::endl;
        }
        if (loadoutJobSeat_ == loadoutSeat_ && !loadoutJobKindId_.empty()) takeSeenLoadout(seen);
    }
    if (!kindId) {
        readTurretOnHud(frame);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // No HUD in the picture at all - the mining mode off - is looked for
    // again every two seconds: the table is not there, and finding that out
    // is a matter of some 80 ms, both tables' shapes looked for (40 with the
    // old one alone; on 22 snapshots of 4.10.3, 37-47 against 71-101 ms).
    if (loadoutRockCheck_ && (contentsFor_ > 0 || contentsTries_ >= 8 ||
                              now - loadoutRockCheckSince_ >= std::chrono::seconds(6))) {
        loadoutRockCheck_ = false;
        lastLoadoutRead_ = {};
    }
    const int every = loadoutSettled_ ? 180
                      : loadoutNoHud_ ? 2
                      : loadoutUnsure_ >= 6 || loadoutDisagreeing_ >= 4 ? 15
                                                                       : 4;
    if (now - lastLoadoutRead_ < std::chrono::seconds(every)) return;
    if (turretJob_.valid() && turretJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    lastLoadoutRead_ = now;
    loadoutJobSeat_ = loadoutSeat_;
    loadoutJobKindId_ = kindId;
    try {
        loadoutJob_ = std::async(std::launch::async, [picture = frame.clone()]() {
            SeenLoadout seen;
            readSeenLoadout(picture, sharedLines(), seen);
            return seen;
        });
    } catch (const std::system_error&) {
    }
}

void ScanSession::Impl::readTurretOnHud(const cv::Mat& frame) {
    if (turretJob_.valid()) {
        if (turretJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        std::string line;
        try {
            line = turretJob_.get();
        } catch (const std::exception& e) {
            std::cerr << "[turret] reading failed: " << e.what() << std::endl;
        }
        takeTurretLine(line);
    }
    if (askHeadCount_ < 2 || askInSeat_) {
        if (!turretSaid_.empty()) {
            Event ev;
            ev.kind = Event::Kind::ClearTurret;
            events_.push_back(std::move(ev));
        }
        turretSaid_.clear();
        turretSeen_.clear();
        turretAgreeing_ = 0;
        turretMissing_ = 0;
        return;
    }
    if (loadoutJob_.valid()) return;
    const auto now = std::chrono::steady_clock::now();
    const int every = turretSaid_.empty() && turretMissing_ < 3 ? 4 : 10;
    if (now - lastTurretRead_ < std::chrono::seconds(every)) return;
    lastTurretRead_ = now;
    try {
        turretJob_ = std::async(std::launch::async, [picture = frame.clone()]() {
            std::string line;
            if (!readTurretLaser(picture, sharedLines(), &line)) return std::string();
            return line;
        });
    } catch (const std::system_error&) {
    }
}

void ScanSession::Impl::takeTurretLine(const std::string& line) {
    const LoadoutResolve resolve = loadoutResolve();
    const std::string id = line.empty() || !resolve.turretKey ? std::string() : resolve.turretKey(line);
    if (id.empty()) {
        if (++turretMissing_ >= TURRET_GONE_READINGS && !turretSaid_.empty()) {
            Event ev;
            ev.kind = Event::Kind::ClearTurret;
            ev.turretMinute = true;
            events_.push_back(std::move(ev));
            turretSaid_.clear();
            turretSeen_.clear();
            turretAgreeing_ = 0;
        }
        return;
    }
    turretMissing_ = 0;
    turretAgreeing_ = id == turretSeen_ ? turretAgreeing_ + 1 : 1;
    turretSeen_ = id;
    if (turretAgreeing_ < 2) {
        lastTurretRead_ = {};
        return;
    }
    if (id == turretSaid_) return;
    turretSaid_ = id;
    Event ev;
    ev.kind = Event::Kind::Turret;
    ev.turretLine = line;
    events_.push_back(std::move(ev));
}

void ScanSession::Impl::takeSeenLoadout(const SeenLoadout& read) {
    if (!read.tableFound || !read.modulesRead) {
        loadoutNoHud_ = !read.tableFound;
        if (read.tableFound) loadoutUnsure_++;
        return;
    }
    const LoadoutResolve resolve = loadoutResolve();
    const std::string key = resolve.fitKey ? resolve.fitKey(read.laserText, read.moduleTexts, read.moduleBelow)
                                           : std::string();
    if (key.empty()) {
        loadoutUnsure_++;
        return;
    }
    loadoutNoHud_ = false;
    loadoutUnsure_ = 0;
    const bool disagrees = !loadoutSeen_.empty() && key != loadoutSeen_;
    loadoutDisagreeing_ = disagrees ? loadoutDisagreeing_ + 1 : 0;
    loadoutAgreeing_ = key == loadoutSeen_ ? loadoutAgreeing_ + 1 : 1;
    loadoutSeen_ = key;
    if (loadoutAgreeing_ < 2) {
        if (loadoutDisagreeing_ < 4) lastLoadoutRead_ = {};
        return;
    }
    if (key == loadoutSaid_) return;
    Event ev;
    ev.kind = Event::Kind::Loadout;
    ev.loadout.kindId = loadoutJobKindId_;
    ev.loadout.laserText = read.laserText;
    ev.loadout.moduleTexts = read.moduleTexts;
    ev.loadout.moduleBelow = read.moduleBelow;
    ev.loadout.tableX = read.table.x;
    ev.loadout.tableY = read.table.y;
    ev.loadout.own = loadoutAgreeing_ >= OWN_READINGS;
    events_.push_back(std::move(ev));
    pendingLoadout_ = true;
    pendingKey_ = key;
}

void ScanSession::Impl::noteLoadoutResult(const std::string& fit, bool changed) {
    (void)changed;
    if (!pendingLoadout_) return;
    pendingLoadout_ = false;
    if (fit.empty() && loadoutAgreeing_ < OWN_READINGS) {
        lastLoadoutRead_ = {};
        return;
    }
    loadoutSettled_ = true;
    loadoutSaid_ = pendingKey_;
}

ScanSession::ScanSession() : impl_(std::make_unique<Impl>()) {}
ScanSession::~ScanSession() = default;

void ScanSession::setHooks(Hooks hooks) {
    impl_->hooks_ = std::move(hooks);
    liveCapture().setYield(impl_->hooks_.breathe);
    liveOcr().setYield(impl_->hooks_.breathe);
}

void ScanSession::setThreads(int configured) {
    const int cores = (std::max)(1, (int)std::thread::hardware_concurrency());
    const int want = configured > 0 ? configured : std::clamp(cores / 4, 2, 6);
    cv::setNumThreads(want);
    std::cout << "[cpu] " << cores << " cores, scan uses " << want << std::endl;
}

int ScanSession::loadGlyphs(const char* path) { return liveGlyphs().loadTemplates(path); }

bool ScanSession::embeddedTemplates() const { return liveGlyphs().usesEmbeddedTemplates(); }

bool ScanSession::initOcr() {
    OCRConfig config;
    config.tessdataPath = "./tessdata";
    config.language = "eng";
    config.pageSegMode = 8;
    config.minConfidence = 20.0;
    impl_->ocrAvailable_ = liveOcr().initialize(config);
    return impl_->ocrAvailable_;
}

bool ScanSession::loadLines() { return sharedLines().loadEmbedded(); }

void ScanSession::setLive(bool on) { impl_->liveMode_ = on; }

void ScanSession::setAutoTrack(bool on) {
    impl_->autoTrack_ = on;
    impl_->hasLastLabel_ = false;
    impl_->lockPatch_.release();
}

bool ScanSession::autoTrack() const { return impl_->autoTrack_; }

void ScanSession::setRecord(bool on, int keeps, int keepGb) {
    impl_->recordScans_ = on;
    impl_->maxKeeps_ = keeps;
    impl_->maxKeepGb_ = keepGb;
    g_debugImages = on;
}

void ScanSession::setEase(bool on) { impl_->easeIdle_ = on; }

void ScanSession::setInterval(int milliseconds) { impl_->captureIntervalMs_ = milliseconds; }

void ScanSession::setHudPercent(int percent) { impl_->hudScalePercent_ = percent; }

void ScanSession::setDebugImages(bool on) { g_debugImages = on; }

void ScanSession::noteWait(int milliseconds) { g_intervalMs = (double)milliseconds; }

void ScanSession::setLogShip(const std::string& model, const std::string& label, bool drake) {
    g_ship = model;
    g_shipLabel = label;
    g_drake = drake;
}

void ScanSession::setLogPlace(const std::string& place) { g_place = place; }

void ScanSession::noteLogSeat(const std::string& model, const std::string& hull, bool inSeat, int boardings) {
    const std::string seat = model + "|" + hull + "|" + (inSeat ? "in" : "out") + "|" + std::to_string(boardings);
    if (seat == impl_->loadoutLogSeat_) return;
    impl_->loadoutLogSeat_ = seat;
    impl_->loadoutLookAgain();
}

void ScanSession::setCovers(std::vector<Rect> covers) { impl_->covers_ = std::move(covers); }

void ScanSession::loadAim(int x, int y, int w, int h, bool automatic, int margin) {
    impl_->signatureRegion_.x = x;
    impl_->signatureRegion_.y = y;
    impl_->signatureRegion_.width = w;
    impl_->signatureRegion_.height = h;
    impl_->aimAuto_ = automatic;
    impl_->trackMargin_ = margin;
}

void ScanSession::carryFixedAim(int viewX, int viewY, int viewW, int viewH) {
    if (impl_->aimAuto_ || viewW <= 0 || viewH <= 0) return;
    auto viewport = ScreenCapture::gameViewport();
    if (!viewport) return;
    if (viewport->x == viewX && viewport->y == viewY && viewport->width == viewW && viewport->height == viewH) return;
    const double sx = static_cast<double>(viewport->width) / viewW;
    const double sy = static_cast<double>(viewport->height) / viewH;
    impl_->signatureRegion_.x = viewport->x + static_cast<int>(std::lround((impl_->signatureRegion_.x - viewX) * sx));
    impl_->signatureRegion_.y = viewport->y + static_cast<int>(std::lround((impl_->signatureRegion_.y - viewY) * sy));
    impl_->signatureRegion_.width = (std::max)(20, static_cast<int>(std::lround(impl_->signatureRegion_.width * sx)));
    impl_->signatureRegion_.height = (std::max)(10, static_cast<int>(std::lround(impl_->signatureRegion_.height * sy)));
    std::cout << "Viewport changed (" << viewW << "x" << viewH << " -> " << viewport->width << "x" << viewport->height
              << ") - aim point carried over" << std::endl;
}

void ScanSession::moveAim(int x, int y, int w, int h) {
    impl_->signatureRegion_.x = x;
    impl_->signatureRegion_.y = y;
    impl_->signatureRegion_.width = w;
    impl_->signatureRegion_.height = h;
}

void ScanSession::setAim(int x, int y, int w, int h) {
    moveAim(x, y, w, h);
    impl_->aimAuto_ = false;
    impl_->aimViewport_ = cv::Rect();
    impl_->hasLastLabel_ = false;
    impl_->lockPatch_.release();
}

void ScanSession::armAutoAim() {
    impl_->aimAuto_ = true;
    impl_->aimViewport_ = cv::Rect();
}

ScanSession::Aim ScanSession::aim() const {
    Aim box;
    box.x = impl_->signatureRegion_.x;
    box.y = impl_->signatureRegion_.y;
    box.width = impl_->signatureRegion_.width;
    box.height = impl_->signatureRegion_.height;
    box.automatic = impl_->aimAuto_;
    box.margin = impl_->trackMargin_;
    return box;
}

ScanSession::View ScanSession::gameView() const {
    View view;
    if (auto viewport = ScreenCapture::gameViewport()) {
        view.found = true;
        view.x = viewport->x;
        view.y = viewport->y;
        view.width = viewport->width;
        view.height = viewport->height;
    }
    return view;
}

bool ScanSession::refreshAutoAim(bool announce) { return impl_->refreshAutoAim(announce); }

bool ScanSession::refreshHudScale() { return impl_->refreshHudScale(); }

int ScanSession::effectiveIntervalMs(int baseMs, bool ease) const {
    return impl_->effectiveIntervalMs(baseMs, ease);
}

ScanSession::RockReport ScanSession::lookForRock(const RockAsk& ask) {
    impl_->askInSeat_ = ask.inSeat;
    impl_->askCanMine_ = ask.shipCanMine;
    impl_->askHeadCount_ = ask.headCount;
    impl_->askShip_ = ask.ship;
    impl_->askHull_ = ask.hull;
    impl_->askBoardings_ = ask.boardings;
    impl_->askRocksHere_ = ask.rocksHere;
    impl_->askGameGeneration_ = ask.gameGeneration;
    impl_->askChannel_ = ask.channel;
    impl_->events_.clear();
    impl_->lookForRock();
    RockReport report;
    report.events = std::move(impl_->events_);
    return report;
}

void ScanSession::noteLoadoutResult(const std::string& fit, bool changed) { impl_->noteLoadoutResult(fit, changed); }

void ScanSession::loadoutLookAgain() { impl_->loadoutLookAgain(); }

ScanSession::SignatureReport ScanSession::readSignature(const ReadingPolicy& policy, int lastRecognized) {
    impl_->events_.clear();
    impl_->signatureOut_ = {};
    impl_->policy_ = policy;
    impl_->lastRecognizedSignature_ = lastRecognized;
    impl_->readSignature();
    SignatureReport out = impl_->signatureOut_;
    out.recognized = impl_->lastRecognizedSignature_;
    out.rockEvents = std::move(impl_->events_);
    impl_->policy_ = {};
    return out;
}

ScanSession::OcrHit ScanSession::readFixed(const ReadingPolicy& policy) {
    impl_->policy_ = policy;
    OcrHit hit;
    cv::Rect taken;
    cv::Mat image = impl_->captureSignatureImage(&taken);
    if (image.empty()) {
        impl_->policy_ = {};
        return hit;
    }
    hit.captured = true;
    hit.tracked = impl_->lastScanTracked_;
    hit.x = taken.x;
    hit.y = taken.y;
    auto candidates = liveCapture().preprocessCandidates(image);
    if (auto value = liveOcr().recognizeBest(candidates, policy)) {
        hit.recognized = true;
        hit.value = *value;
        hit.confidence = liveOcr().getLastConfidence();
    }
    impl_->policy_ = {};
    return hit;
}

ScanSession::OcrHit ScanSession::probeAim(const ReadingPolicy& policy) {
    impl_->policy_ = policy;
    OcrHit hit;
    if (impl_->hooks_.overlayVisible && impl_->hooks_.overlayVisible() && impl_->hooks_.showOverlay) {
        impl_->hooks_.showOverlay(false);
        Sleep(100);
    }
    impl_->hasLastLabel_ = false;
    cv::Rect taken;
    cv::Mat image = impl_->captureSignatureImage(&taken);
    if (!image.empty()) {
        saveDebugCapture("region_apply.png", image);
        hit.captured = true;
        hit.tracked = impl_->lastScanTracked_;
        hit.x = taken.x;
        hit.y = taken.y;
        auto candidates = liveCapture().preprocessCandidates(image);
        if (auto value = liveOcr().recognizeBest(candidates, policy)) {
            impl_->missStreak_ = 0;
            hit.recognized = true;
            hit.value = *value;
            hit.confidence = liveOcr().getLastConfidence();
        }
    }
    if (impl_->hooks_.haveOverlay && impl_->hooks_.haveOverlay() && impl_->hooks_.showOverlay)
        impl_->hooks_.showOverlay(true);
    impl_->policy_ = {};
    return hit;
}

void ScanSession::syncRecording(bool live) {
    if (live && impl_->recordScans_) {
        const std::uintmax_t free = freeBytesAt(dataPath("debug/recording"));
        if (free < RECORD_MIN_FREE_BYTES) {
            std::cout << "[recorder] only " << (free >> 20)
                      << " MB free on this drive - recording stays off" << std::endl;
        } else if (!liveRecorder().start(dataPath("debug/recording"),
                                         1000.0 / (std::max)(1, impl_->captureIntervalMs_), RECORD_SEGMENT_SECONDS,
                                         RECORD_SEGMENTS)) {
            std::cout << "[recorder] " << liveRecorder().lastError() << std::endl;
        }
    } else {
        liveRecorder().stop();
    }
}

void ScanSession::keepRecording() { impl_->keepRecording(); }

ScanSession::SnapshotWrite ScanSession::writeSnapshot(const std::string& pngPath) {
    SnapshotWrite wrote;
    const cv::Rect picture = pictureFor(impl_->signatureRegion_);
    wrote.x = picture.x;
    wrote.y = picture.y;
    // No picture without the game on screen: the screen is then other programs'.
    // A game whose window is not found is still read while its process runs.
    const bool onScreen = ScreenCapture::gameViewport().has_value() ||
                          (!ScreenCapture::gameMinimised() && GameFiles::gameRunning());
    if (!onScreen) {
        wrote.skipped = true;
        wrote.minimised = ScreenCapture::gameMinimised();
        impl_->noteCapture(cv::Mat());
        return wrote;
    }
    cv::Mat frame = impl_->grabGamePicture();
    impl_->noteCapture(frame);
    wrote.haveFrame = !frame.empty();
    if (wrote.haveFrame) wrote.wrote = cv::imwrite(pngPath, frame);
    return wrote;
}

ScanSession::Facts ScanSession::facts() const {
    Facts facts;
    facts.threads = cv::getNumThreads();
    facts.hudScale = impl_->hudScale_;
    facts.hudScalePercent = impl_->hudScalePercent_;
    facts.referenceHeight = GlyphReader::REFERENCE_VIEWPORT_HEIGHT;
    facts.source = sourceName(liveCapture().lastSource());
    facts.duplication = liveCapture().captureNote();
    facts.light = liveCapture().rawStats();
    facts.picture = impl_->captureStats_;
    facts.embeddedTemplates = liveGlyphs().usesEmbeddedTemplates();
    facts.rockView = true;
    facts.aimX = impl_->signatureRegion_.x;
    facts.aimY = impl_->signatureRegion_.y;
    facts.aimW = impl_->signatureRegion_.width;
    facts.aimH = impl_->signatureRegion_.height;
    facts.aimAuto = impl_->aimAuto_;
    facts.margin = impl_->trackMargin_;
    const cv::Rect area = impl_->allowedArea();
    if (area.width > 0 && area.height > 0) {
        facts.haveSearch = true;
        facts.searchX = area.x;
        facts.searchY = area.y;
        facts.searchW = area.width;
        facts.searchH = area.height;
    }
    if (auto viewport = ScreenCapture::gameViewport()) {
        facts.haveView = true;
        facts.viewX = viewport->x;
        facts.viewY = viewport->y;
        facts.viewW = viewport->width;
        facts.viewH = viewport->height;
    }
    return facts;
}

bool ScanSession::gameWindowFound() const { return ScreenCapture::gameWindow() != nullptr; }

void* ScanSession::gameWindowHandle() const { return ScreenCapture::gameWindow(); }

bool ScanSession::gameInView() const {
    const HWND game = ScreenCapture::gameWindow();
    return game && !gameCovered(game);
}

bool ScanSession::gameMinimised() const { return ScreenCapture::gameMinimised(); }

std::chrono::steady_clock::time_point ScanSession::rockPanelAt() const { return impl_->lastRockPanel_; }

Datarunner::Pixels ScanSession::grabGamePixels() { return Datarunner::packBgra(impl_->grabGamePicture()); }

Datarunner::Pixels ScanSession::grabViewPixels() {
    auto viewport = ScreenCapture::gameViewport();
    if (!viewport) return {};
    CaptureRegion whole;
    whole.x = viewport->x;
    whole.y = viewport->y;
    whole.width = viewport->width;
    whole.height = viewport->height;
    whole.autoDetect = false;
    return Datarunner::packBgra(liveCapture().captureRegion(whole));
}

}  // namespace SCMiner
