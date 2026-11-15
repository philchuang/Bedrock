#include "RockContents.h"

#include "MiningHardware.h"
#include "MiningLocations.h"
#include "RockScans.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace SCMiner {

namespace {

bool sameName(const char* a, const std::string& b) {
    if (std::strlen(a) != b.size()) return false;
    for (size_t i = 0; i < b.size(); i++) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

const OreBands* bandsOf(const std::string& ore) {
    for (int b = 0; b < ORE_BANDS_COUNT; b++) {
        if (sameName(ORE_BANDS[b].ore, ore)) return &ORE_BANDS[b];
    }
    return nullptr;
}

// Which of an ore's eight values a quality is, -1 for none of them
int bandOf(const std::string& ore, int quality) {
    const OreBands* bands = bandsOf(ore);
    if (!bands) return -1;
    for (int k = 0; k < 8; k++) {
        if (bands->bands[k] == quality) return k;
    }
    return -1;
}

// The numerals of a line the reader gave, with a full stop where it wrote
// one: a reader of words puts O for 0, I or l for 1, S for 5, B for 8 - and
// has a full-width stop and comma in its alphabet besides
std::string numerals(std::string text) {
    for (const auto& [wide, plain] : {std::pair<const char*, char>{"\xEF\xBC\x8C", ','},
                                      std::pair<const char*, char>{"\xEF\xBC\x8E", '.'},
                                      std::pair<const char*, char>{"\xEF\xBC\x85", '%'}}) {
        for (size_t at; (at = text.find(wide)) != std::string::npos;) text.replace(at, std::strlen(wide), 1, plain);
    }
    std::string out;
    for (unsigned char c : text) {
        if (c >= '0' && c <= '9') out += (char)c;
        else if (c == 'O' || c == 'o' || c == 'D' || c == 'Q') out += '0';
        else if (c == 'I' || c == 'l' || c == '|' || c == 'i') out += '1';
        else if (c == 'Z' || c == 'z') out += '2';
        else if (c == 'S' || c == 's') out += '5';
        else if (c == 'B') out += '8';
        else if (c == '.' || c == ',') out += '.';
        else if (c == '%') out += '%';
    }
    return out;
}

// The shares are cut off after the second place, not rounded: measured
// lists add up to 99.97 to 99.99, never to 100.00 or over. So a list of n
// lines that is the whole rock adds up to between 100 less n hundredths
// and 100.
bool sumIsTheRock(double sum, size_t lines) {
    return sum > 100.0 - 0.01 * (double)lines - 0.006 && sum < 100.006;
}

}  // namespace

bool parseShare(const std::string& text, float& percent) {
    const std::string seen = numerals(text);
    std::string digits;
    int stop = -1;
    for (char c : seen) {
        if (c == '%') break;
        if (c == '.') {
            if (stop < 0 && !digits.empty()) stop = (int)digits.size();
            continue;
        }
        digits += c;
    }
    // Always to two places, "0.01%" to "100.00%": the last two numerals are
    // the hundredths, with the stop read or lost. One read elsewhere - a
    // numeral lost behind it - is no share.
    if (digits.size() < 3 || digits.size() > 5) return false;
    const int whole = (int)digits.size() - 2;
    if (stop >= 0 && stop != whole) return false;
    const double value = std::atoi(digits.substr(0, (size_t)whole).c_str()) + std::atoi(digits.substr((size_t)whole).c_str()) / 100.0;
    if (value <= 0.0 || value > 100.0) return false;
    percent = (float)value;
    return true;
}

bool parseQuality(const std::string& text, int& quality) {
    const std::string seen = numerals(text);
    if (seen.empty() || seen.size() > 4) return false;
    for (char c : seen) {
        if (c < '0' || c > '9') return false;
    }
    const int value = std::atoi(seen.c_str());
    if (value > 1000) return false;
    quality = value;
    return true;
}

bool qualityOfOre(const std::string& ore, int quality) {
    return bandOf(ore, quality) >= 0;
}

namespace {

// Lines the quality alone leaves to two ores, named by the parts of the rock:
// each part is one line at the most, at a quality its roll can come to -
// lowestQualityBand to topQualityBand. A Hephaestanite rock's bulk at 330
// could be Quartz by its value, but where Quartz has its own line at 522 it
// is not. Where that still leaves two ways, the rock's weight decides: its
// kg per SCU is 142.857 times its parts' densities by their shares, the
// inert rest at 1.0, within a few per cent for each rock read so far, and the
// bulk named the other ore is an eighth off.
bool nameByParts(const std::vector<const RockPart*>& parts, const RockContents& contents, double kgPerScu,
                 std::vector<std::string>& named) {
    const size_t lines = contents.parts.size();
    std::vector<std::vector<size_t>> fits(lines);
    for (size_t i = 0; i < lines; i++) {
        const RockPartRead& line = contents.parts[i];
        if (line.inert || line.quality == 0) continue;
        for (size_t p = 0; p < parts.size(); p++) {
            const int band = bandOf(parts[p]->ore, line.quality);
            if (band >= 0 && band >= lowestQualityBand(*parts[p]) && band <= topQualityBand(parts[p]->qualityScale)) {
                fits[i].push_back(p);
            }
        }
        if (fits[i].empty()) return false;
    }
    std::vector<std::vector<std::string>> ways;
    std::vector<bool> taken(parts.size(), false);
    std::vector<std::string> way(lines);
    std::function<void(size_t)> place = [&](size_t i) {
        if (ways.size() > 16) return;
        if (i == lines) {
            if (std::find(ways.begin(), ways.end(), way) == ways.end()) ways.push_back(way);
            return;
        }
        if (fits[i].empty()) {   // the inert rest
            way[i].clear();
            place(i + 1);
            return;
        }
        for (size_t p : fits[i]) {
            if (taken[p]) continue;
            taken[p] = true;
            way[i] = parts[p]->ore;
            place(i + 1);
            taken[p] = false;
        }
    };
    place(0);
    if (ways.size() == 1) {
        named = ways[0];
        return true;
    }
    if (ways.empty() || kgPerScu <= 0) return false;
    // Within 4 % - each ore's rocks shared so far weigh within 1 % of their
    // parts as a rule, Savrilium's 2 % - and every other way 8 % off at the
    // least. A small line of 3 % is not told apart that way: it moves the
    // weight by under 1 %.
    double best = 1e9, second = 1e9;
    size_t at = 0;
    for (size_t w = 0; w < ways.size(); w++) {
        double density = 0;
        for (size_t i = 0; i < lines; i++) {
            density += contents.parts[i].percent / 100.0 * (ways[w][i].empty() ? 1.0 : oreDensity(ways[w][i]));
        }
        if (density <= 0) continue;
        const double off = std::fabs(kgPerScu / (RockScans::KG_PER_SCU_AT_DENSITY_ONE * density) - 1.0);
        if (off < best) {
            second = best;
            best = off;
            at = w;
        } else if (off < second) {
            second = off;
        }
    }
    if (best > 0.04 || second < 0.08) return false;
    named = ways[at];
    return true;
}

}  // namespace

bool nameContentsByRock(const std::string& rockOre, RockContents& contents, double kgPerScu) {
    std::vector<const RockPart*> parts;
    for (int i = 0; i < ROCK_PART_COUNT; i++) {
        if (sameName(ROCK_PARTS[i].rock, rockOre)) parts.push_back(&ROCK_PARTS[i]);
    }
    if (parts.empty() || contents.parts.empty()) return false;
    // A line is the ore whose value its quality is - of the ores a rock of
    // this kind holds, and within what that part of it can come to. The
    // panel does not keep the files' order - a Hephaestanite rock can list
    // Silicon first, a Bexalite rock Gold before Borase - and a piece broken
    // off a rock holds whatever share of a part fell to it: all Iron at 325,
    // or a seventh Iron at 664 and the rest inert. So neither the order nor
    // the share says which line is what; the quality does, wherever the
    // rock's ores share no value - all but two kinds: Hephaestanite and
    // Quartz, each the other's side ore, both begin at 330. Their lines at
    // 330 are named by the parts the other lines took, or by the rock's
    // weight (nameByParts); failing both, they are read by name. The line
    // without a quality is the inert rest.
    std::vector<std::string> named(contents.parts.size());
    bool twoOres = false;
    int rests = 0;
    for (size_t i = 0; i < contents.parts.size(); i++) {
        const RockPartRead& line = contents.parts[i];
        if (line.inert || line.quality == 0) {
            rests++;
            continue;
        }
        for (const RockPart* part : parts) {
            const int band = bandOf(part->ore, line.quality);
            if (band < 0 || band > topQualityBand(part->qualityScale)) continue;
            if (!named[i].empty() && !sameName(part->ore, named[i])) twoOres = true;   // two ores have that value
            named[i] = part->ore;
        }
        if (named[i].empty()) return false;
    }
    if (rests > 1) return false;
    if (twoOres && !nameByParts(parts, contents, kgPerScu, named)) return false;
    for (size_t i = 0; i < contents.parts.size(); i++) {
        contents.parts[i].ore = named[i];
        contents.parts[i].inert = named[i].empty();
    }
    return true;
}

bool contentsFitRock(const std::string& rockOre, const RockContents& contents) {
    std::vector<const RockPart*> parts;
    for (int i = 0; i < ROCK_PART_COUNT; i++) {
        if (sameName(ROCK_PARTS[i].rock, rockOre)) parts.push_back(&ROCK_PARTS[i]);
    }
    // Ore by ore, of the ores the rock is built of: its lines' bands, the
    // highest first, against what its parts can come to, the furthest first.
    // A line too many, or one above its part's reach, and they are not this
    // rock's.
    for (const RockPart* part : parts) {
        std::vector<int> reach, bands;
        for (const RockPart* other : parts) {
            if (sameName(other->ore, part->ore)) reach.push_back(topQualityBand(other->qualityScale));
        }
        for (const RockPartRead& line : contents.parts) {
            if (!line.inert && sameName(part->ore, line.ore)) bands.push_back(bandOf(line.ore, line.quality));
        }
        if (bands.size() > reach.size()) return false;
        std::sort(reach.rbegin(), reach.rend());
        std::sort(bands.rbegin(), bands.rend());
        for (size_t i = 0; i < bands.size(); i++) {
            if (bands[i] > reach[i]) return false;
        }
    }
    return true;
}

bool contentsHold(RockContents& contents) {
    double sum = 0;
    bool named = !contents.parts.empty();
    for (const RockPartRead& part : contents.parts) {
        sum += part.percent;
        if (part.inert) {
            if (part.quality != 0) named = false;
        } else if (part.ore.empty() || bandOf(part.ore, part.quality) < 0) {
            named = false;
        }
    }
    contents.sum = (float)sum;
    contents.whole = named && sumIsTheRock(sum, contents.parts.size());
    return contents.whole;
}

// Every quality a rock of that kind can show: its ores' ORE_BANDS values up
// to the band each part reaches. Empty for a kind without parts. The reader
// asks this (PanelFive.h) and does not include the ore table itself.
std::vector<int> fiveQualities(const std::string& rockOre) {
    std::vector<int> out;
    for (int i = 0; i < ROCK_PART_COUNT; i++) {
        const RockPart& part = ROCK_PARTS[i];
        if (!sameName(part.rock, rockOre)) continue;
        for (int b = 0; b < ORE_BANDS_COUNT; b++) {
            if (!sameName(ORE_BANDS[b].ore, part.ore)) continue;
            for (int band = lowestQualityBand(part); band <= topQualityBand(part.qualityScale); band++) {
                out.push_back(ORE_BANDS[b].bands[band]);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace SCMiner
