#include "RefineryGlance.h"

#include "GrabHub.h"
#include "RefineryTerminal.h"
#include "ScreenCapture.h"

#include <chrono>
#include <ctime>
#include <future>
#include <iostream>

namespace SCMiner {

namespace {

std::future<RefineryReading> job_;
std::string jobStation_;
long long jobTaken_ = 0;
std::chrono::steady_clock::time_point lastLook_{};
bool up_ = false;

}  // namespace

bool RefineryGlance::panelUp() const { return up_; }

bool RefineryGlance::takeFinished(RefineryReading& read, std::string& station, long long& taken) {
    if (!job_.valid()) return false;
    if (job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    try {
        read = job_.get();
    } catch (const std::exception& e) {
        std::cerr << "[refinery] reading failed: " << e.what() << std::endl;
        read = RefineryReading();
    }
    station = jobStation_;
    taken = jobTaken_;
    return true;
}

RefineryLook RefineryGlance::look(const RefineryAsk& ask) {
    RefineryLook out;
    if (job_.valid()) return out;
    if (!sharedLines().ready()) return out;
    // In a seat no terminal is in reach, unless the log has named a station.
    if (ask.station.empty() && ask.inSeat) return out;
    if (ask.mining) return out;
    const auto now = std::chrono::steady_clock::now();
    // Twice a second while the terminal is up: a quote stands for a second or
    // two before CONFIRM takes it away.
    if (now - lastLook_ < std::chrono::milliseconds(up_ ? 500 : 1000)) return out;
    lastLook_ = now;
    auto viewport = ScreenCapture::gameViewport();
    if (!viewport) return out;
    CaptureRegion whole;
    whole.x = viewport->x;
    whole.y = viewport->y;
    whole.width = viewport->width;
    whole.height = viewport->height;
    whole.autoDetect = false;
    const cv::Mat frame = liveCapture().captureRegion(whole);
    if (frame.empty()) return out;
    std::vector<RefineryBar> bars = findRefineryBars(frame);
    up_ = !bars.empty();
    const long long taken = (long long)std::time(nullptr);
    // "WORK ORDER DELIVERED" over the panels, or over an empty list for half
    // a minute after the last panels.
    if (ask.wantNotice && ask.atTerminal) up_ = true;
    if (bars.empty() && !(ask.wantNotice && ask.atTerminal)) {
        out.away = true;
        out.station = ask.station;
        out.taken = taken;
        return out;
    }
    jobStation_ = ask.station;
    jobTaken_ = taken;
    const bool notice = ask.wantNotice;
    try {
        TextLineReader& lines = sharedLines();
        job_ = std::async(std::launch::async, [picture = frame.clone(), bars, notice, &lines]() {
            return refineryReading(readRefineryScreen(picture, bars, lines, notice));
        });
    } catch (const std::system_error&) {
        // no thread to be had: the next look tries again
    }
    return out;
}

}  // namespace SCMiner
