#pragma once
// One mining scan: the grab and the readers that turn that picture into the
// structs the window already shows. It does not decide what they mean and it
// does not touch the window. This header names no picture type and no window type.

#include "Datarunner.h"
#include "ReadingPolicy.h"
#include "RockContents.h"
#include "RockStats.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SCMiner {

class ScanSession {
public:
    struct Rect {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };
    // The aim box, in the same coordinates the overlay uses.
    struct Aim {
        int x = 100;
        int y = 100;
        int width = 200;
        int height = 50;
        bool automatic = true;
        int margin = 250;
    };
    struct View {
        bool found = false;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };
    // The only edges back toward the window. None of them names the window.
    struct Hooks {
        std::function<void()> breathe;
        std::function<bool()> overlayVisible;
        std::function<bool()> haveOverlay;
        std::function<void(bool show)> showOverlay;
        std::function<void()> beforeCapture;   // a snapshot owed, if one is
        std::function<void()> aimMoved;        // the automatic box changed
        std::function<void()> firstCapture;    // the first picture of the run
    };
    struct LoadoutTell {
        std::string kindId;
        std::string laserText;
        std::vector<std::string> moduleTexts;
        std::vector<std::vector<std::string>> moduleBelow;
        int tableX = 0;
        int tableY = 0;
        bool own = false;
    };
    struct Event {
        enum class Kind {
            UpdateRock,
            ClearRock,
            SetContents,
            ClearContents,
            Loadout,
            Turret,
            ClearTurret,
            ListTried,      // a contents read started for rock.mass
            ListRecorded,   // two whole readings agreed: contents, for rock.mass
            ShipSeen        // the panels named a ship and the log has no seat
        };
        Kind kind = Kind::UpdateRock;
        RockStats rock;
        bool noteScan = false;
        RockStats rawLook;
        int rows = 0;            // the picture's height, with a noted scan
        float labelScale = 0;   // the panel's label column, 0 when it was not found
        RockContents contents;
        LoadoutTell loadout;
        std::string turretLine;
        bool turretMinute = false;   // a minute with no turret HUD, rather than leaving the seat
        std::string shipId;          // ShipSeen: the ship's id
    };
    struct RockReport {
        std::vector<Event> events;
    };
    // What the scan needs from the log and the window, and nothing else.
    struct RockAsk {
        bool inSeat = false;
        bool shipCanMine = false;
        int headCount = 0;
        std::string ship;   // the log's full name, for the HUD's ship
        std::string hull;
        int boardings = 0;
        // SignatureAnalyzer::Rocks, named here so this header stays free of it:
        // 0 the usual rocks, 1 a place's own (the 5.0 numbers), 2 either
        int rocksHere = 0;
        int gameGeneration = 0;   // the live log's major version; 0 not read yet
        std::string channel;      // LIVE, PTU, ... for a kept picture of the panel
    };
    struct SignatureReport {
        bool haveTrack = false;
        bool sawLabels = false;
        bool locked = false;
        int trackX = 0;
        int trackY = 0;
        bool updated = false;
        int value = 0;
        double confidence = 0;
        std::vector<Event> rockEvents;   // a rock the signature itself cleared
        int recognized = 0;              // lastRecognizedSignature_ after this half
    };
    struct OcrHit {
        bool captured = false;
        bool tracked = false;
        int x = 0;
        int y = 0;
        bool recognized = false;
        int value = 0;
        double confidence = 0;
    };
    struct SnapshotWrite {
        bool haveFrame = false;
        bool wrote = false;
        bool skipped = false;    // the game was not on screen, so nothing was captured
        bool minimised = false;  // ...because it was minimised, rather than not running
        int x = 0;
        int y = 0;
    };
    // What a note of the setup says about the picture. No reader type in it.
    struct Facts {
        int threads = 0;
        double hudScale = 1;
        int hudScalePercent = 100;
        int referenceHeight = 1440;
        std::string source;
        std::string duplication;
        std::string light;
        std::string picture;
        bool embeddedTemplates = true;
        bool rockView = true;
        int aimX = 0, aimY = 0, aimW = 0, aimH = 0;
        bool aimAuto = true;
        int margin = 250;
        bool haveSearch = false;
        int searchX = 0, searchY = 0, searchW = 0, searchH = 0;
        bool haveView = false;
        int viewX = 0, viewY = 0, viewW = 0, viewH = 0;
    };

    ScanSession();
    ~ScanSession();

    void setHooks(Hooks hooks);
    void setThreads(int configured);
    int loadGlyphs(const char* path);
    bool embeddedTemplates() const;
    bool initOcr();
    bool loadLines();

    void setLive(bool on);
    void setAutoTrack(bool on);
    bool autoTrack() const;
    void setRecord(bool on, int keeps, int keepGb);
    void setEase(bool on);
    void setInterval(int milliseconds);
    void setHudPercent(int percent);
    void setDebugImages(bool on);
    void noteWait(int milliseconds);
    void setLogShip(const std::string& model, const std::string& label, bool drake);
    void setLogPlace(const std::string& place);
    // A seat change the log reported. Reads the loadout again on the next look.
    void noteLogSeat(const std::string& model, const std::string& hull, bool inSeat, int boardings);
    void setCovers(std::vector<Rect> covers);

    void loadAim(int x, int y, int w, int h, bool automatic, int margin);
    // A fixed box saved against another viewport, carried into the one now.
    void carryFixedAim(int viewX, int viewY, int viewW, int viewH);
    void moveAim(int x, int y, int w, int h);
    void setAim(int x, int y, int w, int h);
    void armAutoAim();
    Aim aim() const;
    View gameView() const;
    bool refreshAutoAim(bool announce);
    bool refreshHudScale();
    int effectiveIntervalMs(int baseMs, bool ease) const;

    // The rock look. Apply the events before readSignature: the rock is on
    // the window hundreds of milliseconds before the signature half ends.
    RockReport lookForRock(const RockAsk& ask);
    // What setSeenLoadout returned. Settles the loadout the rock look agreed.
    void noteLoadoutResult(const std::string& fit, bool changed);
    void loadoutLookAgain();

    // policy is used for this call only. It must not outlive the analyzer.
    SignatureReport readSignature(const ReadingPolicy& policy, int lastRecognized);

    // The console's one-shot reads. recognized is false when the picture held no number.
    OcrHit readFixed(const ReadingPolicy& policy);
    // The region editor's one shot: writes debug/region_apply.png.
    OcrHit probeAim(const ReadingPolicy& policy);

    void syncRecording(bool live);
    void keepRecording();

    SnapshotWrite writeSnapshot(const std::string& pngPath);
    Facts facts() const;

    bool gameWindowFound() const;
    // The game window, as a handle the host can ask the process of. Null when none.
    void* gameWindowHandle() const;
    bool gameInView() const;
    // The game's window is up but minimised: the screen is other programs'.
    bool gameMinimised() const;
    std::chrono::steady_clock::time_point rockPanelAt() const;

    // The game's picture, overlay stepped aside. For a screenshot's own copy.
    Datarunner::Pixels grabGamePixels();
    // The game's viewport, overlay left where it is. Empty when no game is up.
    Datarunner::Pixels grabViewPixels();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace SCMiner
