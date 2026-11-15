#include "OreNames.h"

#include "MiningData.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace SCMiner {
namespace {

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) row[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        int diagonal = row[0];
        row[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            const int above = row[j];
            row[j] = (std::min)({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

}  // namespace

// What Tesseract made of the ore's line, put right against the minerals the
// game has: over the ice rock's bright face ICE (RAW) came back as JCE (RAW),
// (CE (RAW) and BCE (RAW) as often as not. The name is the nearest mineral
// within a third of its letters, written the way the rest of the app writes
// it; anything further off is not given out at all. The bracket after it
// is dropped.
std::string snapOreName(const std::string& read) {
    std::string name;
    const size_t open = read.rfind('(');
    for (size_t i = 0; i < (open == std::string::npos ? read.size() : open); i++) {
        const char c = (char)std::toupper((unsigned char)read[i]);
        if (c >= 'A' && c <= 'Z') name += c;
    }
    if (name.empty()) return "";
    const MineralSignature* best = nullptr;
    int bestDistance = 1000, ties = 0;
    for (const MineralSignature& m : MINERAL_SIGNATURES) {
        std::string upper;
        for (char c : m.name) upper += (char)std::toupper((unsigned char)c);
        const int d = editDistance(name, upper);
        if (d < bestDistance) {
            bestDistance = d;
            best = &m;
            ties = 0;
        } else if (d == bestDistance) {
            ties++;
        }
    }
    if (best && ties == 0 && bestDistance <= (std::max)(1, (int)best->name.size() / 3)) return best->name;
    // The second reader writes the panel's brackets its own way - "IRON ORE)"
    // with the opening one lost, "IRONCORE)" with it read as a C, and the
    // alphabet it was trained on has a full-width ) that is no bracket here
    // at all - so the word ORE runs into the name and nothing matches. No
    // mineral the game has ends in ORE or RAW, so that tail can go.
    std::string bare = name;
    for (const char* tail : {"ORE", "RAW"}) {
        const size_t at = bare.size() >= 6 ? bare.size() - 3 : std::string::npos;
        if (at != std::string::npos && bare.compare(at, 3, tail) == 0) {
            bare.resize(at);
            break;
        }
    }
    if (bare != name) {
        const std::string again = snapOreName(bare);
        if (!again.empty()) return again;
    }
    // ...and in the game's own English ice is RAW ICE, the word in front, on
    // the panel's heading and in its list alike (the MOLE's seat with the
    // long labels); the language pack that shortens the labels turns it
    // round to ICE (RAW). No mineral begins with RAW.
    if (name.size() >= 6 && name.compare(0, 3, "RAW") == 0) {
        const std::string again = snapOreName(name.substr(3));
        if (!again.empty()) return again;
    }
    // ...and the game itself shortens a long name on some HUDs - the Golem
    // writes HEPH (RAW) for Hephaestanite. Four letters name one mineral and
    // no other, so a name that begins one and only one is that one.
    if (bare.size() >= 4) {
        const MineralSignature* only = nullptr;
        for (const MineralSignature& m : MINERAL_SIGNATURES) {
            std::string upper;
            for (char c : m.name) {
                if (c >= 'a' && c <= 'z') upper += (char)(c - 'a' + 'A');
                else if (c >= 'A' && c <= 'Z') upper += c;
            }
            if (upper.size() < bare.size() || upper.compare(0, bare.size(), bare) != 0) continue;
            if (only && only->name != m.name) return "";   // begins two: no name
            if (!only) only = &m;
        }
        if (only) return only->name;
    }
    return "";
}

}  // namespace SCMiner
