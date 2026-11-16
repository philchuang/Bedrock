#pragma once
// One look at a refinery terminal. The book stays with Application: this
// returns the reading, and says when the player has walked away so the book
// can drop what the next reading would have agreed with. This header names no picture type.

#include "RefineryOrders.h"

#include <string>

namespace SCMiner {

struct RefineryAsk {
    std::string station;
    bool inSeat = false;
    bool mining = false;      // a rock panel was on screen in the last half minute
    bool wantNotice = false;  // an order is done: look for WORK ORDER DELIVERED
    bool atTerminal = false;  // panels were up in the last half minute
};

struct RefineryLook {
    bool away = false;   // no panel, and not waiting on a delivered notice
    std::string station;
    long long taken = 0;
};

class RefineryGlance {
public:
    // A reading that finished. False when none has, or one is still running.
    // Call this before look(), so the book is updated before the next picture.
    bool takeFinished(RefineryReading& read, std::string& station, long long& taken);
    RefineryLook look(const RefineryAsk& ask);
    // A work-order panel was on the last picture. The host waits to hand an
    // order on until the player has left the terminal.
    bool panelUp() const;
};

}  // namespace SCMiner
