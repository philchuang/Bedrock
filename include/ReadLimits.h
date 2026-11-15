#pragma once
// Bounds and capture settings the reader uses without the signature tables.
// The tables live in MiningData.h; this header is what the reader may include.

#include <string>

namespace SCMiner {

// Below this the OCR fallback is guessing rather than reading. One
// constant for the live scan AND the offline benchmark: a benchmark with a
// softer floor counts as recognised readings the player never sees, and a
// benchmark that flatters the app is worse than none.
//
// 65 because Tesseract only ever sees what the template reader could not
// segment, and must not be the softer of the two doors: at 45 it let
// through readings like 7901 off HUD text that had nothing to do with a
// marker.
inline constexpr double MIN_OCR_CONFIDENCE = 65.0;

// Readings above this are treated as misreads. Set well above the data's
// largest family value (ROC 30 x 4000): markers of 123,000 and 171,xxx (on
// a MOLE) appear in game, and a value the cap refuses is never shown - it
// reads as nothing, or as some shorter number.
inline constexpr int MAX_VALID_SIGNATURE = 250000;

// Screen capture configuration
struct CaptureRegion {
    int x = 0;
    int y = 0;
    int width = 400;
    int height = 300;
    bool autoDetect = true;  // Automatic detection of the scan area
};

// OCR configuration
struct OCRConfig {
    std::string tessdataPath = "./tessdata";
    std::string language = "eng";
    int pageSegMode = 7;     // PSM_SINGLE_LINE for single numbers
    double minConfidence = 60.0;
};

}  // namespace SCMiner
