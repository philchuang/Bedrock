#pragma once
// The grab is ScanSession's. This is the read: a screenshot file, or a frame
// already grabbed, turned into the shot the window holds. This header names no picture type.

#include "Datarunner.h"

#include <filesystem>
#include <memory>

namespace SCMiner {

class DatarunnerEyes {
public:
    DatarunnerEyes();
    ~DatarunnerEyes();

    // Where the game's picture is black, `own` is read instead.
    std::shared_ptr<Datarunner::Shot> readFile(const std::filesystem::path& file, const Datarunner::Pixels& own);
    // A frame of the game. The shot's montage is that frame.
    std::shared_ptr<Datarunner::Shot> readFrame(const Datarunner::Pixels& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace SCMiner
