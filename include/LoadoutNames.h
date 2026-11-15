#pragma once
// A HUD line resolved to the laser or module it names. Called after a read,
// beside the hardware table. The reader does not include this header.

#include "MiningHardware.h"

#include <string>
#include <vector>

namespace SCMiner {

// A line as the HUD writes it matched to the game's names: the laser's name
// before MINING LASER, a module's before MODULE. nullptr unless exactly one
// fits.
const MiningLaser* laserByHudLine(const std::string& line);
const MiningModule* moduleByHudLine(const std::string& line);
// ...the laser of a MOLE's turret: size 2, the one size the MOLE takes
const MiningLaser* turretLaserByHudLine(const std::string& line);
// A module's block in the HUD's list - its name, and the lines under it that
// say what it does.
const MiningModule* moduleByHudBlock(const std::string& nameLine, const std::vector<std::string>& below);
// Whether the table's Head column, as read (-1 where nothing was), is what
// this laser and these modules make of it.
bool headColumnAgrees(const MiningLaser& laser, const std::vector<const MiningModule*>& modules, const int head[6],
                      const double sure[6]);

// The reader's questions, answered by the functions above. Installed once,
// from the program that has the tables.
void installLoadoutResolve();

}  // namespace SCMiner
