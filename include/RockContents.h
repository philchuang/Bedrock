#pragma once
// What a rock is made of, read off the scan panel's composition list.
//
// Under the figures the panel lists the rock part by part:
//
//     COMP.                 21.30 SCU
//     10.55%  IRON (ORE)         664
//     36.10%  IRON (ORE)         325
//     53.33%  INERT MATERIALS      0
//
// a share of the rock, what it is, and its quality - the number the crafting
// bench reads off the refined material. A rock of an ore is built from parts
// (ROCK_PARTS, MiningHardware.h): a little of the ore at any quality, the
// bulk of it out of the ore's two lowest values, a side ore or two, and the
// rest inert. So the same ore stands in the list twice, and the line worth
// having is the small one.
//
// The list stands under the panel's label column, at a place and a lean of
// its seat's own, so its table is found by its lettering - the column the
// shares begin at, the column the qualities end at - and its words are read
// by the second reader, share, name and quality each on its own: a whole
// line is too wide for it. What it gives back is checked the way no single
// figure of the panel can be: the shares have to add up to the rock, and
// every quality has to be one of the eight values its ore has.

#include <cstddef>
#include <string>
#include <vector>

namespace SCMiner {

struct RockPartRead {
    std::string ore;         // as the signature table spells it; "" for the inert rest
    bool inert = false;
    float percent = 0;       // of the rock
    int quality = 0;         // as printed: one of the ore's eight values, 0 for the inert rest
    std::string shareText, nameText, qualityText;   // as the reader gave them, for the tools
};

struct RockContents {
    std::vector<RockPartRead> parts;   // in the panel's order
    bool whole = false;      // every line made sense, and the shares add up to the rock
    float sum = 0;           // of the shares read
    std::string note;        // for the tools: why not, or how it was read
    double ms = 0;
};

// "10.55%" -> 10.55; also "1055%" and "10.55", which the reader writes when
// it loses the full stop or the sign. False for anything that is no share.
bool parseShare(const std::string& text, float& percent);
// "664" -> 664, with the letters a reader of words puts for numerals (O, I,
// S, B) put right. False for anything that is no whole number up to 1000.
bool parseQuality(const std::string& text, int& quality);
// Whether a quality is one of the ore's eight values (ORE_BANDS)
bool qualityOfOre(const std::string& ore, int quality);

// The names the list's lines have by the rock's own kind, where the reader
// was not asked for them: a rock of `rockOre` holds the ores ROCK_PARTS
// names and an inert rest, in whatever order the panel lists them, and a
// line is the ore whose value its quality is - within what that part of the
// rock can come to. The line without a quality is the inert rest. Fills in
// `ore`/`inert` and says yes when every line's quality is one ore's and no
// other's - or, where two ores have the value (Hephaestanite and Quartz at
// 330), when the rock's other lines or its weight leave one: `kgPerScu` is
// the panel's mass over its SCU, 0 where not known.
bool nameContentsByRock(const std::string& rockOre, RockContents& contents, double kgPerScu = 0);

// Whether named lines can be a rock's of that kind, or a piece of one: a rock
// has one part of its ore that comes at any quality and one out of the two
// lowest values, so two lines of the ore above those are not its lines - an
// Ice rock's 322 misread as 922, which is a value Ice has, would make a
// second such line. Says yes for a kind ROCK_PARTS does not have, and of
// ores the kind does not hold.
bool contentsFitRock(const std::string& rockOre, const RockContents& contents);

// Whether a list hangs together: every part named, every quality one its ore
// has, and the shares adding up to the rock - which is a little under 100,
// since the panel cuts each share off after its second place. Sets `whole`
// and `sum`.
bool contentsHold(RockContents& contents);

}  // namespace SCMiner
