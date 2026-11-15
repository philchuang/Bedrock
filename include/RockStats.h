#pragma once
// What a rock says about itself, off the scan panel. No hardware table: the
// reader fills this, and the advice that uses the tables lives beside them.

#include <string>

namespace SCMiner {

struct RockStats {
    float mass = 0;
    float resistance = 0;      // per cent, as printed
    float instability = 0;
    float scu = 0;
    std::string ore;
    float distance = 0;        // metres, off the marker under the rock
    // Printed in colour - green where the laser on the rock took some off, red
    // where it put some on. Either way the laser has already had its say.
    // See rockOwnResistance.
    bool resistanceAdjusted = false;
    // Made of the 5.0 numbers: a Nyx I rock (RockModel)
    bool v5 = false;
    // 5.0 prints the laser's modifier in front of the resistance and the
    // instability ("+25%  65%"), and the figures above already carry it: the
    // rock's own is figure / (1 + modifier). There the colour says how severe
    // a figure is, so resistanceAdjusted is set from the modifier instead.
    float resistanceDelta = 0;     // per cent as printed; 0 where none was
    float instabilityDelta = 0;
    bool deltasRead = false;       // a 5.0 panel: the two above are what it printed
};

}  // namespace SCMiner
