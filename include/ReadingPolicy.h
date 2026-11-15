#pragma once
// What the reader asks the tables, without including them. The scan supplies
// one of these; the glyph matcher does not compile the ore tables in.

#include <functional>
#include <string>
#include <vector>

namespace SCMiner {

struct ReadingPolicy {
    // Repairs a misread to its unique table value (identity if unknown).
    // Applied BEFORE voting so renderings that fail on different digits
    // still converge on the same candidate.
    std::function<int(int)> canonicalize;
    // Rates how well a value fits the signature table.
    std::function<double(int)> bonus;
    // The scan's questions while it is still looking, answered from the
    // tables by whoever supplied this policy.
    std::function<bool(int)> explainsExactly;
    std::function<bool(int)> isSingleSignature;
    std::function<bool(int)> isKnownSignature;
    std::function<bool(int, std::string&, int&)> isExactMatch;
    std::function<std::vector<int>()> knownSignatures;
};

}  // namespace SCMiner
