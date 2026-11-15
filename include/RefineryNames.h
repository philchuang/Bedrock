#pragma once
// A refinery field as read, named from the tables. The reader calls these;
// it does not include the tables.

#include <string>

namespace SCMiner {

// The method a read names, as REFINING_METHODS spells it; "" when none is near
std::string refiningMethodIn(const std::string& text);
// The material a read names, as REFINERY_MATERIALS spells it ("IRON (ORE)" and
// the refined "IRON" both give "Iron (Ore)"; the salvage's refined
// "CONSTRUCTION MATERIALS", cut short to "CONSTRUCTION MA" in its column or
// not, gives "Construction Salvage"); "" when none is near
std::string refineryMaterialIn(const std::string& text, bool* inert = nullptr);
// Whether a quality is one a material comes in: one of its ore's eight
// values (ORE_BANDS) - the terminal prints the raw ore's, refined or not.
// True for a material the bands do not know (salvage, rubble).
bool refineryQualityFits(const std::string& material, int quality);
// "light", "moderate", "heavy", "extreme"; "" for anything else
const char* refineryWorkloadName(int workload);

}  // namespace SCMiner
