#pragma once
// The reader's questions about a loadout line, answered beside the hardware
// table. No picture and no table in this header: the reader calls the
// pointers, and the program that has the tables installs them.

#include <string>
#include <vector>

namespace SCMiner {

struct LoadoutResolve {
    bool (*laserLine)(const std::string& line) = nullptr;
    bool (*turretLine)(const std::string& line) = nullptr;
    // Size 2 to the letter. One letter off is the size 1 a one-seat ship
    // shows, and a turret's table must not be taken for that ship's.
    bool (*exactTurretLine)(const std::string& line) = nullptr;
    bool (*moduleBlock)(const std::string& name, const std::vector<std::string>& below) = nullptr;
    bool (*headAgrees)(const std::string& laserText, const std::vector<std::string>& moduleNames,
                       const std::vector<std::vector<std::string>>& below, const int* head, const double* sure) = nullptr;
};

void setLoadoutResolve(LoadoutResolve resolve);
LoadoutResolve loadoutResolve();

}  // namespace SCMiner
