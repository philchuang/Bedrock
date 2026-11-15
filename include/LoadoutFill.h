#pragma once
// A reading's lines named from the hardware table, after the reader has
// returned. The reader stores the lines; this fills the pointers the window
// is handed.

#include "LoadoutNames.h"
#include "LoadoutReader.h"

namespace SCMiner {

inline void fillSeenLoadout(SeenLoadout& seen) {
    seen.laser = laserByHudLine(seen.laserText);
    seen.modules.clear();
    for (size_t i = 0; i < seen.moduleTexts.size(); i++) {
        const std::vector<std::string> none;
        const std::vector<std::string>& below = i < seen.moduleBelow.size() ? seen.moduleBelow[i] : none;
        if (const MiningModule* module = moduleByHudBlock(seen.moduleTexts[i], below)) seen.modules.push_back(module);
    }
}

}  // namespace SCMiner
