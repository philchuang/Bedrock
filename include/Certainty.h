#pragma once
// How sure a value read off a picture is. Plain, so a row the window holds
// does not pull in the reader.

namespace SCMiner {

// Sure where two readings of it agree, or it is word for word one the game
// writes; Check where it was made out once, or with a slip; Missing where
// it was not made out at all.
enum class Certainty { Missing = 0, Check = 1, Sure = 2 };

}  // namespace SCMiner
