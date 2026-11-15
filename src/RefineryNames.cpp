#include "RefineryNames.h"

#include "MiningHardware.h"
#include "RefineryData.h"
#include "RefiningData.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace SCMiner {
namespace {

std::string letters(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++) {
            cur[j] = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

}  // namespace

std::string refiningMethodIn(const std::string& text) {
    const std::string t = letters(text);
    if (t.size() < 4) return "";
    int best = 1 << 30;
    std::string name;
    for (const RefiningMethod& m : REFINING_METHODS) {
        const std::string want = letters(m.name);
        // A long name squeezed into the reader's line may come back cut short:
        // compare with as much of the name as was read
        const std::string head = want.substr(0, (std::min)(want.size(), t.size()));
        const int d = editDistance(t, t.size() < want.size() ? head : want);
        if (d < best) {
            best = d;
            name = m.name;
        }
    }
    return best <= (std::max)(1, static_cast<int>(t.size()) / 4) ? name : "";
}

std::string refineryMaterialIn(const std::string& text, bool* inert) {
    if (inert) *inert = false;
    const std::string t = letters(text);
    if (t.size() < 3) return "";
    if (editDistance(t, "INERTMATERIALS") <= 3 || editDistance(t, "NERTMATERIALS") <= 2 ||
        t.rfind("INERT", 0) == 0) {
        if (inert) *inert = true;
        return "";
    }
    // The salvage's refined name, which no material in the tables has: the
    // construction salvage comes out as CONSTRUCTION MATERIALS, too long for
    // its column, so cut short as much as was read
    {
        const std::string want = "CONSTRUCTIONMATERIALS";
        if (t.size() >= 13 && editDistance(t, want.substr(0, (std::min)(want.size(), t.size()))) <= 1) {
            return "Construction Salvage";
        }
    }
    // ...and where what comes out is named for itself, not for what went in:
    // PROCESSING lists raw ice as PRESSURIZED ICE (Levski, 29.10.2026) and,
    // by the game's commodities, raw silicon as SILICON
    {
        struct Refined {
            const char* name;
            const char* material;
        };
        static const Refined REFINED[] = {{"PRESSURIZEDICE", "Raw Ice"}, {"SILICON", "Raw Silicon"}};
        for (const Refined& r : REFINED) {
            const std::string want = r.name;
            if (editDistance(t, want) <= (std::max)(1, static_cast<int>(want.size()) / 5)) return r.material;
        }
    }
    int best = 1 << 30;
    std::string name;
    for (const char* material : REFINERY_MATERIALS) {
        const std::string full = letters(material);
        // "Iron (Ore)" as SETUP writes it, "IRON" as PROCESSING does
        std::string bare = material;
        const size_t paren = bare.find(" (");
        if (paren != std::string::npos) bare = bare.substr(0, paren);
        for (const std::string& want : {full, letters(bare)}) {
            const int d = editDistance(t, want);
            if (d < best) {
                best = d;
                name = material;
            }
        }
    }
    return best <= (std::max)(1, static_cast<int>(t.size()) / 5) ? name : "";
}

bool refineryQualityFits(const std::string& material, int quality) {
    std::string ore = material;
    const size_t paren = ore.find(" (");
    if (paren != std::string::npos) ore = ore.substr(0, paren);
    if (ore == "Raw Ice") ore = "Ice";
    else if (ore == "Raw Silicon") ore = "Silicon";
    else if (ore == "Aluminum") ore = "Aluminium";
    for (const OreBands& b : ORE_BANDS) {
        if (ore != b.ore) continue;
        for (uint16_t v : b.bands) {
            if (v == quality) return true;
        }
        return false;
    }
    return true;
}

const char* refineryWorkloadName(int workload) {
    static const char* const NAMES[] = {"light", "moderate", "heavy", "extreme"};
    return workload >= 0 && workload < 4 ? NAMES[workload] : "";
}

}  // namespace SCMiner
