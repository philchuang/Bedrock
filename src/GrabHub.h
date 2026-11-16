#pragma once
// The one capture and the readers the scan and the refinery glance share.
// Two DXGI captures would fight. Not a public header: it names cv::.

#include "FrameRecorder.h"
#include "GlyphReader.h"
#include "OCREngine.h"
#include "ScreenCapture.h"
#include "TextLineReader.h"

namespace SCMiner {

ScreenCapture& liveCapture();
TextLineReader& sharedLines();
OCREngine& liveOcr();
GlyphReader& liveGlyphs();
FrameRecorder& liveRecorder();

}  // namespace SCMiner
