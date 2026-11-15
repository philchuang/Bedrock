#include "LoadoutNames.h"

#include "DevSwitch.h"
#include "LoadoutResolve.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace SCMiner {
namespace {

// A name as the HUD writes it and as the game does, folded so that what the
// second reader may confuse meets: upper case, letters and numerals only, a
// 1 as I and a 0 as O.
std::string folded(const std::string& text, bool read = false) {
    std::string out;
    for (const unsigned char c : text) {
        if (read && c == 'l') {
            out += 'I';
            continue;
        }
        if (c >= 0x80) continue;
        const char u = (char)std::toupper(c);
        if (u >= 'A' && u <= 'Z') out += u;
        else if (u == '1') out += 'I';
        else if (u == '0') out += 'O';
        else if (u >= '2' && u <= '9') out += u;
    }
    return out;
}

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

template <typename Item, typename Keep>
const Item* byName(const std::string& read, const Item* items, int count, const std::string& lastWords, Keep keep) {
    if (read.size() < 3) return nullptr;
    const std::string tail = folded(lastWords);
    const Item* near = nullptr;
    int nearCount = 0;
    for (int i = 0; i < count; i++) {
        if (!keep(items[i])) continue;
        std::string name = folded(items[i].name);
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            name.resize(name.size() - tail.size());
        }
        if (name == read) return &items[i];
        if (read.size() >= 5 && editDistance(name, read) == 1) {
            near = &items[i];
            nearCount++;
        }
    }
    return nearCount == 1 ? near : nullptr;
}

// One line of what a module does, as the HUD writes it under its name:
// "Laser Power (-5%)", "Optimal Charge Window (+40%)". Its words in capitals
// and the signed figure in the brackets; a line the list's edge cut off
// ("(+4") has a figure that is not to be trusted.
struct Effect {
    std::string words;
    float value = 0;
    bool whole = false;
};

bool parseEffect(const std::string& text, Effect& e) {
    e = Effect();
    size_t open = std::string::npos;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '(') {
            open = i;
            break;
        }
        // the second reader writes the bracket full width now and then: EF BC 88
        if ((unsigned char)text[i] == 0xEF && i + 2 < text.size() && (unsigned char)text[i + 1] == 0xBC &&
            (unsigned char)text[i + 2] == 0x88) {
            open = i;
            break;
        }
    }
    if (open == std::string::npos) return false;
    for (size_t i = 0; i < open; i++) {
        const char u = (char)std::toupper((unsigned char)text[i]);
        if (u >= 'A' && u <= 'Z') e.words += u;
    }
    float sign = 1;
    size_t i = open;
    while (i < text.size() && !std::isdigit((unsigned char)text[i])) {
        if (text[i] == '-') sign = -1;
        i++;
    }
    if (i >= text.size()) return false;
    float v = 0;
    while (i < text.size() && std::isdigit((unsigned char)text[i])) v = v * 10 + (float)(text[i++] - '0');
    if (i < text.size() && text[i] == '.') {
        float place = 0.1f;
        for (i++; i < text.size() && std::isdigit((unsigned char)text[i]); i++, place /= 10) v += place * (text[i] - '0');
    }
    e.value = sign * v;
    e.whole = text.find('%', i) != std::string::npos;
    return !e.words.empty();
}

// What a module's effect line says it does, in its own terms: a figure in
// per cent, as the HUD prints it. NaN when the line is about something the
// module data does not hold. The lines are the game's own texts
// (hud_mining_modifier_*): Laser Power, Optimal Charge Window, Extraction
// Rate, Overcharge Rate, Optimal Charge Rate, Resistance, Instability,
// Shatter Damage, Inert Material Filtering. Extraction Rate is the
// drawing-in beam's power and no charge rate: taken for the optimal charge
// rate, it would count against every FLTR, XTR and Vaux and the Forel, and
// none of them would be recognised (a FLTR-XL shows
// "Extraction Rate (-5%)").
float effectOf(const MiningModule& m, const std::string& words) {
    if (words.find("POWER") != std::string::npos) return std::round((m.power - 1.0f) * 100.0f);
    if (words.find("WINDOW") != std::string::npos) return m.window;
    if (words.find("EXTRAC") != std::string::npos) return m.extraction;
    if (words.find("OVERCH") != std::string::npos) return m.danger;
    if (words.find("RATE") != std::string::npos) return m.rate;
    if (words.find("RESIST") != std::string::npos) return m.resistance;
    if (words.find("INSTAB") != std::string::npos) return m.instability;
    if (words.find("SHATTER") != std::string::npos) return m.shatter;
    if (words.find("INERT") != std::string::npos || words.find("FILTER") != std::string::npos) return m.filter;
    return std::nanf("");
}

// A module's name, folded, without MODULE and without the Roman numeral at
// its end: Focus, Focus II and Focus III are one family to the second reader,
// which writes III as I - three like letters in a row run together in what
// it is trained to put out ("FOCUS I MODULE" as often as not).
std::string familyOf(std::string folded) {
    while (!folded.empty() && folded.back() == 'I') folded.pop_back();
    return folded;
}

std::string moduleBase(const MiningModule& m) {
    std::string name = folded(m.name);
    if (name.size() > 6 && name.compare(name.size() - 6, 6, "MODULE") == 0) name.resize(name.size() - 6);
    return name;
}

// The module a name line and the lines under it name. Every module whose name
// is the one read, one of its family, or within two letters of it is a
// candidate, and what the lines under the name say it does decides: a
// figure that agrees counts for it, one that disagrees against. Without a
// figure only the very name will do. XTR-L and XTR-XL, Torrent II and III
// are a letter apart; what they do is not.
const MiningModule* moduleFromBlock(const std::string& nameLine, const std::vector<std::string>& below) {
    const std::string f = folded(nameLine, true);
    const size_t at = f.find("MODULE");
    if (at == std::string::npos) return nullptr;
    const std::string read = f.substr(0, at);
    if (read.size() < 3) return nullptr;
    const std::string family = familyOf(read);
    std::vector<const MiningModule*> kin;
    const MiningModule* exact = nullptr;
    for (int i = 0; i < MINING_MODULE_COUNT; i++) {
        const std::string base = moduleBase(MINING_MODULES[i]);
        if (base == read) exact = &MINING_MODULES[i];
        if (base == read || (family.size() >= 3 && familyOf(base) == family) || editDistance(base, read) <= 2) {
            kin.push_back(&MINING_MODULES[i]);
        }
    }
    std::vector<Effect> effects;
    for (const std::string& line : below) {
        Effect e;
        if (parseEffect(line, e)) effects.push_back(e);
    }
    const MiningModule* best = nullptr;
    int bestScore = 0, ties = 0, usable = 0;
    for (const Effect& e : effects) {
        if (!kin.empty() && !std::isnan(effectOf(*kin.front(), e.words))) usable++;
    }
    // In a family of numbered ones the name read says nothing of the numeral
    // - FOCUS III came back FOCUS as well - so only what the module does can
    // say which one it is
    int family3 = 0;
    for (const MiningModule* m : kin) {
        if (family.size() >= 3 && familyOf(moduleBase(*m)) == family) family3++;
    }
    for (const MiningModule* m : kin) {
        int score = 0;
        for (const Effect& e : effects) {
            const float expected = effectOf(*m, e.words);
            if (std::isnan(expected)) continue;
            if (std::abs(expected - e.value) < 0.6f) score += 2;
            else if (e.whole) score -= 2;
        }
        // the very name breaks a tie between two that do the same - but not
        // within a family, where the name read may have lost its numeral
        if (m == exact && family3 <= 1) score += 1;
        if (score > bestScore) {
            best = m;
            bestScore = score;
            ties = 0;
        } else if (score == bestScore && score > 0) {
            ties++;
        }
    }
    // nothing said what it does: the name alone, and only the very name -
    // and not one of a family, which the reader cannot count the numerals of
    if (usable == 0) {
        best = exact && family3 <= 1 ? exact : nullptr;
        ties = 0;
    }
    // ...and in a family, a figure has to have agreed
    if (family3 > 1 && bestScore < 2) best = nullptr;
    // A name read whole that its lines say is another module: where the
    // Head column cannot tell the two apart either, it is neither. The FLTR
    // three change nothing in it, and on the Prospector's list, cut at 25
    // letters, an FLTR is told by its Extraction Rate alone - with the 1 of
    // "-15%" lost, an FLTR reads as an FLTR-XL.
    auto headBlind = [](const MiningModule& m) {
        return m.power == 1.0f && m.window == 0.0f && m.rate == 0.0f && m.resistance == 0.0f &&
               m.instability == 0.0f;
    };
    if (exact && best && best != exact && family3 <= 1 && headBlind(*best) && headBlind(*exact)) best = nullptr;
    if (DEV_ENV("SCMINER_LOADOUT_TRACE")) {
        std::cerr << "  [loadout] '" << nameLine << "' over";
        for (const std::string& line : below) std::cerr << " '" << line << "'";
        std::cerr << ": " << kin.size() << " candidates, " << effects.size() << " effects ->";
        for (const Effect& e : effects) std::cerr << " " << e.words << "=" << e.value << (e.whole ? "" : "?");
        std::cerr << " -> " << (best && ties == 0 ? best->name : "-") << " (score " << bestScore << ", ties " << ties
                  << ")" << std::endl;
    }
    return ties == 0 ? best : nullptr;
}

}  // namespace

// Of the size the HUD is read on: the Prospector's and the Golem's lasers are
// size 1, and each size 1 is one letter from its size 2 - HELIX I and HELIX
// II, IMPACT I and II, KLEIN-S1 and -S2 - which multiply the table's Head
// column alike: a numeral read twice over would have put a MOLE's laser on
// a Prospector
const MiningLaser* laserByHudLine(const std::string& line) {
    const std::string f = folded(line, true);
    const size_t at = f.find("MININGLASER");
    if (at == std::string::npos) return nullptr;
    return byName(f.substr(0, at), MINING_LASERS, MINING_LASER_COUNT, "Mining Laser",
                  [](const MiningLaser& l) { return l.size == 1; });
}

const MiningModule* moduleByHudLine(const std::string& line) {
    const std::string f = folded(line, true);
    // "MINING MODULES" heads the list; it names none
    if (f.find("MODULES") != std::string::npos) return nullptr;
    const size_t at = f.find("MODULE");
    if (at == std::string::npos) return nullptr;
    return byName(f.substr(0, at), MINING_MODULES, MINING_MODULE_COUNT, "Module",
                  [](const MiningModule&) { return true; });
}

const MiningLaser* turretLaserByHudLine(const std::string& line) {
    const std::string f = folded(line, true);
    const size_t at = f.find("MININGLASER");
    if (at == std::string::npos) return nullptr;
    const auto sizeTwo = [](const MiningLaser& l) { return l.size == 2; };
    const std::string name = f.substr(0, at);
    if (const MiningLaser* laser = byName(name, MINING_LASERS, MINING_LASER_COUNT, "Mining Laser", sizeTwo)) return laser;
    // The HUD writes II as one glyph, which the second reader may take for a
    // U (HELIX U MINING LASER) - and no laser of size 2 has a U in its name
    if (name.find('U') == std::string::npos) return nullptr;
    std::string two;
    for (const char c : name) two += c == 'U' ? std::string("II") : std::string(1, c);
    return byName(two, MINING_LASERS, MINING_LASER_COUNT, "Mining Laser", sizeTwo);
}

const MiningModule* moduleByHudBlock(const std::string& nameLine, const std::vector<std::string>& below) {
    return moduleFromBlock(nameLine, below);
}

// Whether the table's Head column is what the laser and the modules read
// make of it (readHeadCell). They multiply, as everywhere in the game. An
// active module counts only while it fires, which the HUD may show it
// doing: any of them may be. Every figure that is not 100 has to have been
// read - a module the list lost changes at least one of them, unless all
// it does is filter what is drawn in. One that is 100 the HUD writes in a
// dim grey, which the reader may lose or take for another figure, unsure
// of it (misread as 200 at 0.56, as 160 at 0.83): only a figure read for
// sure says it is not 100, as one in colour is.
constexpr double HEAD_SURE = 0.9;

bool headColumnAgrees(const MiningLaser& laser, const std::vector<const MiningModule*>& modules, const int head[6],
                      const double sure[6]) {
    const size_t n = (std::min)(modules.size(), (size_t)8);
    for (unsigned firing = 0; firing < (1u << n); firing++) {
        bool skip = false;
        double power = 1, resistance = 1 + laser.resistance / 100.0, instability = 1 + laser.instability / 100.0;
        double rate = 1 + laser.rate / 100.0, window = 1 + laser.window / 100.0;
        for (size_t i = 0; i < n; i++) {
            const MiningModule& m = *modules[i];
            const bool on = (firing >> i) & 1u;
            if (on && !m.active) skip = true;
            if (m.active && !on) continue;
            power *= m.power;
            resistance *= 1 + m.resistance / 100.0;
            instability *= 1 + m.instability / 100.0;
            rate *= 1 + m.rate / 100.0;
            window *= 1 + m.window / 100.0;
        }
        if (skip) continue;
        const double expected[6] = {power * 100, resistance * 100, instability * 100, rate * 100, -1, window * 100};
        bool agrees = true;
        for (int r = 0; r < 6 && agrees; r++) {
            if (expected[r] < 0) continue;
            const long want = std::lround(expected[r]);
            if (want == 100) agrees = head[r] < 0 || std::abs(head[r] - 100) <= 1 || sure[r] < HEAD_SURE;
            else agrees = head[r] >= 0 && std::abs(head[r] - want) <= 1;
        }
        if (agrees) return true;
    }
    return false;
}


namespace {

bool resolveLaserLine(const std::string& line) { return laserByHudLine(line) != nullptr; }
bool resolveTurretLine(const std::string& line) { return turretLaserByHudLine(line) != nullptr; }
// To the letter, not one letter off: turretLaserByHudLine also takes a name a
// single letter from a size-2 laser, and that is how HELIX I is HELIX II.
bool resolveExactTurretLine(const std::string& line) {
    const std::string f = folded(line, true);
    const size_t at = f.find("MININGLASER");
    if (at == std::string::npos) return false;
    const std::string read = f.substr(0, at);
    const std::string tail = folded("Mining Laser");
    for (int i = 0; i < MINING_LASER_COUNT; i++) {
        if (MINING_LASERS[i].size != 2) continue;
        std::string name = folded(MINING_LASERS[i].name);
        if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0) {
            name.resize(name.size() - tail.size());
        }
        if (name == read) return true;
    }
    return false;
}
bool resolveModuleBlock(const std::string& name, const std::vector<std::string>& below) {
    return moduleByHudBlock(name, below) != nullptr;
}
bool resolveHeadAgrees(const std::string& laserText, const std::vector<std::string>& moduleNames,
                       const std::vector<std::vector<std::string>>& below, const int* head, const double* sure) {
    const MiningLaser* laser = laserByHudLine(laserText);
    if (!laser || moduleNames.size() != below.size()) return false;
    std::vector<const MiningModule*> modules;
    for (size_t i = 0; i < moduleNames.size(); i++) {
        const MiningModule* module = moduleByHudBlock(moduleNames[i], below[i]);
        if (!module) return false;
        modules.push_back(module);
    }
    return headColumnAgrees(*laser, modules, head, sure);
}

}  // namespace

void installLoadoutResolve() {
    LoadoutResolve resolve;
    resolve.laserLine = resolveLaserLine;
    resolve.turretLine = resolveTurretLine;
    resolve.exactTurretLine = resolveExactTurretLine;
    resolve.moduleBlock = resolveModuleBlock;
    resolve.headAgrees = resolveHeadAgrees;
    setLoadoutResolve(resolve);
}

}  // namespace SCMiner
