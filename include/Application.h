#pragma once

#include <set>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include "MiningAdvice.h"
#include "ScanSession.h"
#include "RefineryGlance.h"
#include "DatarunnerEyes.h"
#include "LoadoutNames.h"
#include <deque>
#include <map>
#include "SignatureAnalyzer.h"
#include "MinerGUI.h"
#include "BlueprintLibrary.h"
#include "GameTexts.h"
#include <future>
#include "GameState.h"
#include "Updater.h"
#include "RockContents.h"
#include "RockScans.h"
#include "Feedback.h"
#include "RefineryOrders.h"
#include "UexPrices.h"
#include "UexReports.h"
#include "WhereToBuy.h"
#include "Datarunner.h"
#include "RefineryShare.h"
#include "UexMarket.h"
#include "PluginHost.h"
#include <memory>

namespace SCMiner {

class Application {
public:
    Application();
    ~Application();

    // Initializes all components
    bool initialize();
    // The aim box for a game drawn in this viewport, in the proportions of
    // the setup it was measured on (see the definition). margin gets the
    // search margin that goes with it.
    // Re-derives the box when it is automatic and the viewport changed;
    // true when it did change
    bool refreshAutoAim(bool announce);

    // "Bedrock.exe --snapshot": the files Ctrl+Alt+S writes, from the
    // console and without the window - debug/snapshot_*.png|txt and
    // debug/session.txt with the capture path filled in
    void snapshotFromConsole();

    // Runs the console main loop
    void run();

    // Runs with the GUI
    void runWithGUI(HINSTANCE hInstance);

    // Stops the application
    void stop();

    // Callback for new analysis results
    using ResultCallback = std::function<void(const std::vector<AnalysisResult>&)>;
    void setResultCallback(ResultCallback callback);

    // Settings
    void setCaptureInterval(int milliseconds);
    void setHotkey(int vkCode);

    // Manual scan (for hotkey triggers)
    void triggerScan();

    // Toggle live mode on/off
    void toggleLiveMode();
    bool isLiveModeActive() const;

    // Auto-tracking of the signature label (for head tracking / Tobii)
    void setAutoTrack(bool enabled);
    bool isAutoTrackActive() const;

private:
    std::unique_ptr<ScanSession> scan_;
    RefineryGlance refineryGlance_;
    DatarunnerEyes datarunnerEyes_;
    void applyRock(const ScanSession::RockReport& report);

    // Recording is OFF unless region_config.txt says record=1: a hand-out
    // must neither film anyone's screen nor need ffmpeg. With it off there is
    // no ring, no Ctrl+Alt+K, and no debug pictures either.
    bool recordScans_ = false;
    // How many Ctrl+Alt+K copies of the recording ring may pile up, and how
    // many GB they may take together - region_config.txt: keeps=, keepgb=.
    // The hand-out defaults are small; a machine that collects footage on
    // purpose sets them higher.
    int maxKeeps_ = 3;
    int maxKeepGb_ = 2;
    // region_config.txt: live=0 starts idle. A fresh install scans right
    // away - the one thing the app is for, and one button less to miss
    bool startLive_ = true;
    bool inScan_ = false;          // a scan is running; messages pumped inside it must not start another
    // Scans in a row that produced nothing. After a streak the search widens
    // to the whole monitor once, to pick the marker up again wherever it went.
    mutable int missStreak_ = 0;
    SignatureAnalyzer analyzer_;
    std::unique_ptr<MinerGUI> gui_;
    // What the player owns, off the game's log - see BlueprintLibrary
    BlueprintLibrary library_;

    // ---- plugins (PluginHost.h) ----
    std::unique_ptr<Plugins::Host> plugins_;
    std::vector<Plugins::Offer> pluginOffers_;     // what the server's list has
    std::string pluginServerNote_;                  // what the window says of that list; "" all well
    std::string pluginIndexUrl_;                    // where the list is; "" nowhere
    struct PluginFetch {
        std::string id;      // the plugin fetched; "" the list
        bool ok = false;
        std::string body, why;
    };
    std::future<PluginFetch> pluginJob_;            // the list or a plugin, off the window's thread
    std::string pluginBusy_;                        // the plugin being fetched; "" none
    unsigned long long pluginPushedLook_ = ~0ull;   // what the list showed last, in one figure
    std::chrono::steady_clock::time_point pluginPushedAt_{}, pluginDrawnAt_{};
    void setUpPlugins();
    void servicePlugins();
    void pushPlugins();
    void pluginAction(const std::string& id, const std::string& action);
    void fetchForPlugins(const std::string& id, const std::string& url);
    MinerGUI::PluginView pluginView(const std::string& id, Plugins::UiState& state);
    // Where the player is and what they fly, off the same Game.log - see
    // GameState. The log sets the system; a click by hand holds until the
    // log names another one.
    GameState gameState_;
    std::string logSystem_;      // the system the log last set
    std::string logShipLabel_;   // what was last announced, so changes are announced once
    std::string placeLine_;      // "Pyro - Monox", as shown; "" when unknown
    std::string placeOre_;       // the ore tables' location behind it, "" when none
    // A place the player set by hand; "" while the game log has it. It
    // outlives the session, because a player who had to set it once will be
    // in the same place when they come back, and because forgetting it
    // silently would put them back on a stale place without saying so. The
    // log takes it back the moment it proves the ship elsewhere - see
    // applyGameState - which is why the time it was set is kept with it, in
    // the log's own UTC spelling ("2026-09-27T09:27:53"): only a line the
    // game wrote AFTER the setting can overrule it.
    std::string placeOverride_;
    std::string placeOverrideStamp_;

    bool shipCanMine() const;   // panelsWanted: anywhere but in the seat of a ship that cannot mine
    void loadoutLookAgain();    // the next look reads the loadout at once
    void noteRockScan(const RockStats& shown, const RockStats& look, int rows, float labelScale);
    // The rocks done (Recorder::done) to rock_scans.csv and the share queue:
    // those whose wait is over, or with `all` every rock off view
    void keepRockScans(bool all = false);
    // Once a second: the rocks whose wait is over; the game put away, the
    // rock on view waits too; the game closed, every rock is done and goes
    // to the service at once
    void tendRockScans();
    std::chrono::steady_clock::time_point rockScansTended_{};
    bool rockGameUp_ = false;   // the game's window seen since the game last closed (tendRockScans)
    void setRockShare(bool on);
    void serviceRockShare();
    // A message from the Feedback page (Feedback.h): when Send is clicked the
    // message, and with the page's tick the debug folder zipped after it
    // (DebugZip.h), all on one job off the UI thread - no second try,
    // nothing kept. The page is told how far the zip's upload is, what came
    // of it all, and what goes along with a message whenever that changes:
    // the game's version as the rock scans send it, and whether the
    // installation number.
    void sendFeedback(const std::string& text, const std::string& name, bool debugFolder);
    void serviceFeedback();
    int feedbackGame();
    // ...how far the zip's upload is: its bytes out and its size, 0 until it
    // starts - the job's to write; serviceFeedback reads them for the page
    struct FeedbackUpload {
        std::atomic<std::int64_t> sent{0}, total{0};
    };
    std::shared_ptr<FeedbackUpload> feedbackUpload_;
    std::future<Feedback::Result> feedbackJob_;   // one going: what came of it
    int feedbackGameTold_ = -1;                   // what the page was last told
    bool feedbackInstallTold_ = false;
    RockScans::Recorder rockScans_;   // the rocks the view took, for rock_scans.csv
    // ...and sent to the service (RockScans) unless the player takes the tick
    // off in Settings (sharerocks=0), where the build carries the service's
    // address
    bool rockShare_ = true;
    std::string rockSession_;                       // drawn at each start
    std::deque<RockScans::Scan> rockShareQueue_;    // rocks waiting to go
    std::vector<RockScans::Scan> rockShareSending_;
    std::future<int> rockShareJob_;                 // a report going: the status it was answered with
    std::chrono::steady_clock::time_point rockShareNext_{}, rockShareRetry_{};
    int tradesSeen_ = -1;                    // the trade book's changes the window has been told of

    // The refinery's terminal (RefineryTerminal, RefineryOrders). Placing a
    // work order writes nothing to the game's log, so the REFINEMENT CENTER
    // is read off the screen - and only there: at a station the tables know
    // a refinery at, the picture is looked at once a second for a work
    // order's panel (some 10 ms), and only while one is up is it read, off
    // the window's thread. What two readings in a row agree on goes into
    // the book, which the Refinery window shows.
    void lookForRefinery();
    // Each work order read whole goes to the UEX report once
    void handRefineryOrdersToUex();
    // What the order book made of each quote and order, and what came of
    // each job at UEX, a line each with its time: debug\refinery_jobs.txt,
    // to see why a job did not go
    void keepRefineryNotes(const std::vector<std::string>& lines);
    std::chrono::steady_clock::time_point refineryHandCheck_{};
    void takeRefineryScreen(const RefineryReading& read, const std::string& station, long long taken);
    void pushRefineryOrders();
    RefineryOrderBook refineryOrders_;
    std::string refineryLastStation_;           // the station of the last reading with panels on it...
    std::chrono::steady_clock::time_point refineryLastPanels_{};   // ...and when
    // Refinery workloads (RefineryShare): what the terminals show of them,
    // read by the player - shown in the Refinery window, and sent unless the
    // player takes the tick off in it (sharerefinery=0) where the build
    // carries the service's address (BEDROCK_REFINERY_URL), LIVE's refineries
    // only - and what other players read, fetched while the Refinery window
    // is open
    bool refineryShare_ = true;
    struct WorkloadReading {
        int capacity = -1, workload = -1;
        long long at = 0;    // UTC seconds
    };
    std::map<std::string, WorkloadReading> workloadLast_;   // the last read per station, for the next to agree with
    std::map<std::string, WorkloadReading> workloadMine_;   // ...what two in a row agreed on
    std::map<std::string, WorkloadReading> workloadSent_;
    std::string quoteLast_;                                 // a quote read, for the next reading to agree with
    std::set<std::string> quotesSent_;
    std::deque<std::string> shareQueue_;                    // reports waiting to go
    std::string shareSending_;
    std::future<int> shareJob_;                             // one going: the status it was answered with
    std::chrono::steady_clock::time_point shareRetryAt_{};
    std::future<std::pair<std::string, std::map<std::string, RefineryShare::StationState>>> stationsJob_;
    std::chrono::steady_clock::time_point stationsAsked_{};
    std::map<std::string, RefineryShare::StationState> stationsShared_;
    void shareRefineryScreen(const RefineryReading& read, const std::string& station, long long taken);
    bool queueRefineryShare(const RefineryShare::Reading& reading);   // false where it does not go
    void serviceRefineryShare();
    void pushRefineryWorkloads();
    void setRefineryShare(bool on);
public:
    // The service's address in this build ("" for none) - a workshop build
    // can be pointed elsewhere with SCMINER_REFINERY_URL
    static std::string refineryServiceUrl();
private:
    static constexpr const char* REFINERY_ORDERS_FILE = "refinery_orders.txt";
    static constexpr const char* PLACE_OVERRIDE_FILE = "place.txt";
    void loadPlaceOverride();
    void setPlaceOverride(const std::string& placeName);
    void setGameTextsWanted(bool wanted);   // the box in the Blueprints window
    void setGameNamesWanted(bool wanted);   // ...and the one beside it, for the components' names
    void applyGameState();
    std::string gameLogOverride_;    // region_config.txt: gamelog=<path to Game.log>
    // region_config.txt: update=0 stops the app ever looking for a newer
    // version, updateurl= points it somewhere else. Looking sends nothing
    // but the request itself, and installs nothing without being asked.
    bool updateCheck_ = true;
    // stats=0: the update check and the refinery workloads go without the
    // install's own number (Updater::installId), and only the version and
    // platform are sent
    bool statsWanted_ = true;
    // ...the number itself, "" with stats=0: sent with the update check and
    // with the refinery workloads (X-Bedrock-Install)
    std::string installId_;
    std::vector<std::string> installHeader() const;
    std::string updateUrl_;
    Updater updater_;
    // Asked at start and once an hour after that: a companion window can stay
    // up for days, and the next start may be that far away.
    std::chrono::steady_clock::time_point lastUpdateCheck_{};
    static constexpr int UPDATE_RECHECK_MINUTES = 60;
    std::chrono::steady_clock::time_point lastLibraryPoll_;
    size_t lostHullsPassed_ = 0;     // GameState::lostHulls() already given to the window
    int libraryLooks_ = 0;           // polls without a log, for looking again
    bool libraryPushed_ = false;     // the window has seen the library once
    bool ranksPushed_ = false;       // ...and the ranks
    bool ranksKnown_ = false;        // the logs read to the end, so the ranks mean something
    // The contracts running in the game and the ones finished lately, as the
    // window was last told (see pushContracts)
    bool contractsPushed_ = false;
    int runningSeen_ = -1;
    std::set<std::string> ordersSaid_;   // gathering orders the log line was written for, by MissionId
    int gadgetChangesSeen_ = 0;          // GameState::gadgetChanges(), as last acted on
    bool sessionQuiet_ = false;          // the log has not been written to for a quarter of an hour
    // The box in the Blueprints window: Bedrock's marks in the game's own
    // contract texts, rewritten when the library changes - see GameTexts.
    // gametexts= in region_config.txt. Off until asked for: it writes into
    // the game's folder.
    bool gameTextsWanted_ = false;
    // ...and the box beside it: a component's class, size and grade before
    // its name, a missile's tracking signal and size, in the same file.
    // gamenames= in region_config.txt; off until asked for as well.
    bool gameNamesWanted_ = false;
    bool settingsHeld_ = false;      // a backup was put back: region_config.txt is its until the next start
    bool gameTextsDue_ = true;       // a write is owed - once per run, and after each change
    std::chrono::steady_clock::time_point gameTextsDueAt_{};
    std::future<GameTexts::Outcome> gameTextsJob_;   // the write in flight, off the UI thread
    void keepGameTextsFresh();
    std::chrono::steady_clock::time_point lastGameTextsCheck_{};
    std::map<std::string, long long> gameTextsAnswered_;   // per channel: the patch already written or cleared for
    std::string gameTextsNote_;      // what the last one said, for the window
    // The Ore Finder's prices from UEX (uexcorp.space, see UexPrices). Asked
    // for only while the Ore Finder is open: the price list once the one
    // kept is half an hour old, the terminal list once a week; both kept in
    // the data folder's uex\ between runs. What goes along is the version
    // and the platform in the User-Agent, and Bedrock's app token where the
    // build has one (CMakeLists.txt) - nothing of the player. uex=0 in
    // region_config.txt and it is never asked.
public:
    struct UexOutcome {
        std::shared_ptr<const UexPrices> prices;   // null where there is nothing to show
        long long fetchedAt = 0;                   // when the price list was fetched, Unix seconds
        std::string note;                          // why the price list is not fresh; "" when it is
        std::string placesNote;                    // why the offers have no places (the terminal list)
        bool tokenRefused = false;                 // UEX turned Bedrock's app token away, and was asked without it
    };
    // The two lists, kept in `folder` and fetched where they are too old -
    // waits for UEX, so never on the UI thread (--uexprobe calls it as well)
    static UexOutcome loadUex(const std::filesystem::path& folder);
    // Whether the build carries an app token, for the self-test (the token
    // itself goes no further)
    static std::string uexTokenForProbe();
    // ...and the marketplace's listings of one ore, told as lines - no
    // player's name in them
    static std::string uexMarketForProbe(const std::filesystem::path& folder, const std::string& ore);
private:
    bool uexWanted_ = true;
    bool uexPushed_ = false;                       // the window has been told whether it is on
    std::future<UexOutcome> uexJob_;               // the asking in flight, off the UI thread
    std::shared_ptr<const UexPrices> uexPrices_;
    long long uexFetchedAt_ = 0;
    std::string uexNote_;
    bool uexTried_ = false;
    std::chrono::steady_clock::time_point uexTriedAt_{};
    static constexpr int UEX_PRICES_MINUTES = 30;
    // ...and after a failure, again only after this long, and only while
    // the Ore Finder is still open
    static constexpr int UEX_RETRY_MINUTES = 10;
    // The shops that sell the mining gear (WhereToBuy.h): UEX's items_prices
    // of category 28 (gadgets), 29 (laser heads) and 30 (modules), kept in
    // uex\ and fetched again once half a day old. Asked for from the start,
    // so the Gear page has them when it opens: first the lists kept from
    // before, however old, without asking UEX - then whichever of them is
    // half a day old is fetched. One that cannot be fetched leaves the one
    // kept in place, however old.
public:
    struct UexShopsOutcome {
        std::shared_ptr<const UexShops> shops;   // null where there is nothing to show
        std::string note;                        // why not, or why not fresh; "" when all went well
        bool keptOnly = false;                   // only the lists kept, without asking UEX
    };
    // (static, and of the folder given: the self-test reads one it has kept).
    // keptOnly: the lists kept, whatever their age, and nothing asked of UEX
    static UexShopsOutcome loadUexShops(const std::filesystem::path& folder, bool keptOnly = false);
private:
    void serviceUexShops();
    void handGearOrder();
    std::string gearPlace_, gearSystem_;
    std::future<UexShopsOutcome> uexShopsJob_;
    std::shared_ptr<const UexShops> uexShops_;
    std::string uexShopsNote_;
    bool uexShopsKeptRead_ = false;   // the lists kept from before have been read in
    bool uexShopsTried_ = false;
    std::chrono::steady_clock::time_point uexShopsTriedAt_{};
    static constexpr int UEX_SHOPS_HOURS = 12;
    // Reporting to UEX (UexReports.h), opt-in: uexreport= in
    // region_config.txt is 0, or the Unix second the player agreed. What goes
    // is the player's refinery jobs, to their own account there. The key is
    // in uex_key.dat.
    struct UexPending {
        std::string key;                   // what makes it once-only
        UexRefineryJob job;
        // ...or a Datarunner picture's report (shot > 0): ready to go, and
        // the rows it carries
        int shot = 0;
        std::string body;
        std::vector<Datarunner::Sent> rows;
    };
    struct UexSent {
        std::string key, note, user;
        bool keyTrouble = false;           // UEX turned the key away
        std::string accountTrouble;        // ...or the account behind it (UexReports::accountTrouble)
        bool answered = false;             // UEX answered at all - not sent, it says nothing of the account
        // a Datarunner picture's report: what UEX said of it ("" taken), the
        // ids it gave, the entry a duplicate stopped at
        int shot = 0;
        std::string said, duplicate;
        std::vector<std::string> ids;
        std::vector<Datarunner::Sent> rows;
    };
    long long uexReportSince_ = 0;
    bool uexOffered_ = false;              // uexoffered= : the first start's offer was made
    int uexKeyChanges_ = 0;                // every key kept, for the window
    std::string uexKey_;                   // never logged, never in the config
    bool uexKeyTrouble_ = false;           // ...until the key changes, nothing more is sent
    std::string uexAccountTrouble_;        // what UEX said of the account at the last report, for the window
    std::deque<UexPending> uexQueue_;
    std::set<std::string> uexReported_;    // done, and kept in uex\reported.txt
    bool uexReportedRead_ = false;
    std::future<UexSent> uexSendJob_;
    std::future<std::shared_ptr<UexDirectory>> uexDirectoryJob_;
    std::shared_ptr<const UexDirectory> uexDirectory_;
    std::chrono::steady_clock::time_point uexDirectoryTried_{};
    bool uexDirectoryAsked_ = false;
    std::string uexReportNote_;            // what came of the last report or press, for the window
    long long uexReportNoteAt_ = 0;
    std::string uexUser_;                  // the name UEX answered with
    // The UEX marketplace (UexMarket.h): the listings of the ore the Ore
    // Finder's UEX tab shows, and of a rock's part of quality 900 or more
    // (the rock view says what a player pays for it); each ore's kept ten
    // minutes. Public and anonymous - no key, no opt-in, uex=0 stops it.
    struct UexMarketAnswer {
        std::string ore;
        std::shared_ptr<std::vector<UexListing>> listings;   // null where there was no answer
        std::shared_ptr<std::map<std::string, int>> items;   // the marketplace's items, where they were read now
        std::string note;
    };
    static UexMarketAnswer loadUexMarket(const std::filesystem::path& folder, const std::string& ore,
                                         std::shared_ptr<const std::map<std::string, int>> items);
    std::future<UexMarketAnswer> uexMarketJob_;
    std::shared_ptr<const std::map<std::string, int>> uexMarketItems_;
    std::map<std::string, std::chrono::steady_clock::time_point> uexMarketAsked_;
    std::vector<std::string> uexRockOres_;     // the rock on view's ores of quality 900 or more
    void serviceUexMarket();
    // ...and, with the player's key, their own conversations there - so a
    // listing they are talking about opens at their thread - every two
    // minutes while the UEX tab is up
    std::future<std::shared_ptr<UexThreads>> uexThreadsJob_;
    std::chrono::steady_clock::time_point uexThreadsAsked_{};
    bool uexThreadsEver_ = false;
    static std::shared_ptr<UexThreads> loadUexThreads(const std::string& key);
    static constexpr int UEX_MARKET_MINUTES = 10;
    static constexpr int UEX_MARKET_QUALITY = 900;
    static std::shared_ptr<UexDirectory> loadUexDirectory(const std::filesystem::path& folder);
    void askUexDirectory();          // UEX's lists, asked for off the UI thread where they are not in yet
    void serviceUexReports();
    void readUexReported();
    void rememberUexReported(const std::string& key);
    void pushUexAccount();
    void uexAccountAction(int action, const std::string& text);
    std::string currentSystem_;   // "" until Game.log names one - see MinerGUI

    // UEX Datarunner (Datarunner.h), opt-in on its own: datarunner= in
    // region_config.txt is 0, or the Unix second the player turned it on.
    // While it is on and a key is in, the game's ScreenShots folder is
    // looked at every two seconds; a picture written since it was turned on
    // and since Bedrock started is read off the UI thread, and one that
    // shows a commodity terminal waits on the main window for the player.
    // Nothing goes but on their Send.
    long long datarunnerSince_ = 0;
    // "Don't show next time" ticked under its introduction: the rail opens
    // the page without it (region_config.txt datarunnerintro=0)
    bool datarunnerIntroOff_ = false;
    long long datarunnerFrom_ = 0;                          // pictures older than this are not read
    std::filesystem::path datarunnerFolder_;                // the ScreenShots folder looked at
    std::set<std::string> datarunnerKnown_;                 // its pictures dealt with, by name
    struct DatarunnerGrowing {
        std::uintmax_t size = 0;
        std::chrono::steady_clock::time_point since{};
        Datarunner::Pixels own;   // Bedrock's picture of the game, taken when this one came (for one the game saved black)
    };
    std::map<std::string, DatarunnerGrowing> datarunnerGrowing_;   // new ones, until the game has written them
    std::deque<std::pair<std::filesystem::path, Datarunner::Pixels>> datarunnerToRead_;   // with Bedrock's own of the moment
    // A picture read that showed no terminal, for the foot of the main window
    // for a minute: "" none
    std::string datarunnerNote_;
    std::chrono::steady_clock::time_point datarunnerNoteAt_{};
    std::chrono::steady_clock::time_point datarunnerLooked_{}, datarunnerPushed_{};
    std::future<std::shared_ptr<Datarunner::Shot>> datarunnerReadJob_;   // (after the reader: it goes first)
    std::vector<std::shared_ptr<Datarunner::Shot>> datarunnerShots_;     // waiting for the player, oldest first
    int datarunnerNextId_ = 1;
    // ...and at a commodity terminal without Print Screen (watchTerminal):
    // looks at the game while the log says one is in use, read with a reader
    // of their own
    std::future<std::shared_ptr<Datarunner::Shot>> terminalJob_;   // (after the reader: it goes first)
    std::chrono::steady_clock::time_point terminalLooked_{}, terminalSeen_{};
    long long terminalStartedFor_ = 0;      // the log's terminal line the looking began with, ms
    bool terminalLooking_ = false;
    std::shared_ptr<Datarunner::Shot> terminalLastLook_;   // the last look that saw a terminal, null none
    // ...the last one that did since the looking began, kept through looks
    // that missed it (terminalReading)
    std::shared_ptr<const Datarunner::Shot> terminalSeenLook_;
    int terminalReadingPushed_ = -1;        // terminalReadingShot as pushDatarunner last told the window
    // the rows the looks gave the player in the last half hour, whatever came
    // of them - sent, closed or thrown away, they are not offered again
    std::vector<std::shared_ptr<Datarunner::Shot>> terminalOffered_;
    // the lists the player sent or put away while at the terminal, shop and
    // side: read no more until he has left it (Datarunner::listDone)
    std::vector<std::pair<std::string, int>> terminalDone_;
    std::vector<Datarunner::Sent> datarunnerSent_;          // what went, out of uex\reported.txt
    // UEX's game versions, LIVE and PTU - a report says which it is of
    std::string uexVersions_[2];
    std::future<std::pair<std::string, std::string>> uexVersionsJob_;
    std::chrono::steady_clock::time_point uexVersionsAsked_{};
    bool uexVersionsEver_ = false;
    static constexpr int DATARUNNER_KEPT = 20;              // pictures waiting at most
    void serviceDatarunner();
    bool watchScreenshots();
    bool watchTerminal();
    bool terminalReading() const;
    // ...and the picture of the list being read: its id, 0 none yet; -1 not reading
    int terminalReadingShot() const;
    // The picture Bedrock's looks at a shop's list go into: its own of that
    // list not sent yet, taken from `since` on (Unix seconds); null none
    std::shared_ptr<Datarunner::Shot> terminalPicture(const std::string& shop, int side, long long since) const;
    void pushDatarunner();
    void datarunnerAction(int action, int shot, int row, int value);
    void datarunnerAnswered(const UexSent& sent);
    void keepSentPicture(const Datarunner::Shot& shot);

    std::atomic<bool> running_ = false;
    std::atomic<bool> scanRequested_ = false;
    std::atomic<bool> liveModeActive_ = false;
    // Live scan interval. Kept short because signatures can flash up for only
    // a moment (in-game bug), and a missed frame means a missed reading.
    int captureIntervalMs_ = 300;
    // What the scan is allowed to cost when nothing is coming back. A
    // scan is expensive - it takes most of a sixteen-core machine, and on
    // a smaller one it takes the lot. None of this ever STOPS the scan:
    // every judgement here about what the player is doing is a guess, and a
    // wrong guess has to cost time, never a reading. ease=0 in
    // region_config.txt turns it off.
    bool easeIdle_ = true;
    // How many Ctrl+Alt+S pictures to keep. Ten for a report; a few
    // hundred while a new cockpit's markers are being collected - seventy
    // of them at ten megabytes each is what a template set is built from,
    // and a cap of ten throws away all but the last handful.
    int snapshotKeep_ = 50;   // a tester's snapshots are the data that comes back
    int scanThreads_ = 0;           // 0 = worked out from the machine
    int effectiveIntervalMs() const;
    // Whether Star Citizen runs (its window, or else its process)...
    bool gameUp();
    // ...and is not minimised: the screen is read only then
    bool gameOnScreen();
    bool gameProcessUp();   // its process, looked for every two seconds at the most
    std::chrono::steady_clock::time_point gameProcessChecked_{};
    bool gameProcess_ = false;
    int hotkeyVkCode_ = VK_F9;      // default: F9

    // Live mode: last recognized signature (to avoid duplicate updates)
    int lastRecognizedSignature_ = 0;
    std::chrono::steady_clock::time_point lastScanTime_;

    bool regionDetected_ = false;
    // region_config.txt: hudscale=100 leaves every picture as captured. On
    // 1440p footage shown at 1.5x (4K) and 0.75x (1080p) it read 164 / 94
    // frames right, against 153 / 78 with the locator brought to the
    // reference scale and 146 / 75 with locator and reader both. 0 follows
    // the picture (1.5 on 4K), another number forces that factor; both are
    // for measuring on a real 4K capture.
    int hudScalePercent_ = 100;
    // region_config.txt: uiscale=, the app's own zoom. 0 means the
    // window has never been told, so it keeps whatever it worked out
    // from the screen.
    int uiScalePercent_ = 0;
    bool snapshotPending_ = false;   // Ctrl+Alt+S arrived while a scan ran

    // Auto-tracking of the signature label (for head tracking / Tobii).
    std::atomic<bool> autoTrack_ = true;

    // Overlay window for visualizing the capture region
    HWND overlayHwnd_ = nullptr;
    // The cyan frame on screen, off by default: the box is worked out from
    // the game window, so there is nothing to drag into place, and a frame
    // that only shows while LIVE is off tells the player nothing.
    // overlay=1 in region_config.txt brings it back for a look.
    bool overlayEnabled_ = false;
    void createOverlayWindow(HINSTANCE hInstance);
    void updateOverlayPosition();
    void destroyOverlay();
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // Persisting the capture region
    void saveRegionConfig();
    void loadRegionConfig();
    // The "what's new" card, once per version - see the definition for why
    // a first-ever run is not an update
    void maybeShowWhatsNew();
public:
    // ...and what it puts up, from the version last seen ("" for none) and
    // this one: nothing, the card, the news window (2.5 and 2.0), 2.6.7's
    // thank-you, or 3.0's tour
    enum class WhatsNew { Nothing, Card, News, Thanks, Tour };
    static WhatsNew whatsNewAfter(const std::string& seen, const std::string& version, bool firstEverRun);
private:
    // Game.log: what the user pinned, else the running game's own folder,
    // else the folder the launcher last started from; empty when none
    std::filesystem::path locateGameLog() const;
    void pollLibrary();              // a step of log reading, now and then
    void pushLibrary();              // hands the window what is owned
    void pushRanks();                // ...and the player's rank with each outfit
    void pushContracts();            // ...and the contracts of the moment
    void applyGadget();              // a gadget stuck on a rock, or taken off it
    void serviceGameTexts();         // starts an owed write, collects a finished one
    void gameTextsChosen();          // either box changed: the file written anew, or taken away
    void serviceUex();               // asks UEX when the Ore Finder wants prices, collects the answer

public:
    // Callback for overlay movement (called from OverlayProc)
    void onOverlayMoved(int x, int y, int w, int h);
    // A refinery job read off the REFINEMENT CENTER - the terminal reader
    // hands each one over once it has its method, cost, time and every
    // ore's quantity and yield: reported to the player's UEX account when
    // they have opted in, once a job
    void reportRefineryJob(const UexRefineryJob& job);

private:
    ResultCallback resultCallback_;

    // Domain knowledge for the OCR consensus vote: repair misread digits
    // against the signature table and reward exact hits
    ReadingPolicy readingPolicy();

    // Switches live scanning (and with it the rolling recording) on or off.
    // Shared by the LIVE button and by the restart that resumes the previous
    // state, so the two can never drift apart.
    void applyLiveMode(bool active);

    // Copies the rolling recording aside so the ring cannot overwrite it.
    // Bound to a global hotkey: a missed reading is only ever noticed while
    // playing, and by the time it has been reported the clips holding it are
    // long gone.
    void keepRecording();

    // Ctrl+Alt+S: the game picture as it is captured, with a note of
    // everything the scan works from, to debug/snapshot_*.png|txt - the
    // material for a report of "it reads nothing here". snapshotKeep_ at most.
    void requestSnapshot();
    void saveSnapshot();

    // debug/session.txt: what this run works with (screen, viewport, aim
    // box, scale, capture path). One file, rewritten, never growing.
    void writeSessionInfo();
    void describeSetup(std::ostream& out) const;
    // A copy of region_config.txt in debug/, so that folder alone is the
    // whole report: settings, session, log and snapshots in one go
    void mirrorConfigToDebug() const;
    // Processes a single scan cycle (console mode, auto-detects the region)
    void processScan();

    // Live mode scan (quiet, only reports changes)
    void processScanLive();

    // Scan with GUI update
    void processScanWithGUI();

    // Prints the results to the console
    void displayResults(const std::vector<AnalysisResult>& results, int scannedValue);

    // Compact one-line output for live mode
    void displayResultsCompact(const std::vector<AnalysisResult>& results, int scannedValue);

    // Hotkey handling
    void setupHotkey();
#ifdef _WIN32
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif
};

} // namespace SCMiner
