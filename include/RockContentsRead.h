#pragma once
// Reading a rock's composition list off a picture. The window holds
// RockContents (RockContents.h) and does not include this.

#include "RockContents.h"
#include "ScanPanel.h"

#include <opencv2/core.hpp>

#include <string>

namespace SCMiner {

class TextLineReader;

// The composition list of a panel whose label column was found (ScanPanel::
// labels, labelScale). `rockOre` is the name the panel's own heading gave,
// "" when it was not read: with it the lines' names follow from the rock's
// kind and only shares and qualities are read; without it, or where that
// does not fit, the names are read too. `kgPerScu`, the rock's mass over its
// SCU, helps name the lines by the rock's kind (nameContentsByRock). Some
// 50 ms a word, 200 to 350 ms a list, so not on a window's thread.
bool readRockContents(const cv::Mat& frame, const ScanPanel& panel, const std::string& rockOre,
                      TextLineReader& lines, RockContents& out, double kgPerScu = 0);

// The part of the picture the list is looked for in - all the reader looks
// at, so a caller that reads on another thread need keep no more than this:
// the same reading comes of the cut-out, with the panel's `labels` moved by
// the cut-out's corner. Empty where no label column was found.
cv::Rect rockContentsRegion(const ScanPanel& panel, const cv::Size& frame);

}  // namespace SCMiner
