#pragma once
// An ore's name as a reader gave it, snapped to a mineral the game has.
// The name list is the signature table; this header does not include it.

#include <string>

namespace SCMiner {

// "JCE (RAW)" -> "Ice", "HEPH (RAW)" -> "Hephaestanite". Empty when nothing
// is near enough.
std::string snapOreName(const std::string& read);

}  // namespace SCMiner
