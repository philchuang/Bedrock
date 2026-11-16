#include "DatarunnerEyes.h"

#include "DatarunnerRead.h"
#include "TextLineReader.h"

#include <iostream>
#include <memory>

namespace SCMiner {

struct DatarunnerEyes::Impl {
    TextLineReader files_;
    TextLineReader frames_;
    bool filesLoaded_ = false;
    bool framesLoaded_ = false;
};

DatarunnerEyes::DatarunnerEyes() : impl_(std::make_unique<Impl>()) {}
DatarunnerEyes::~DatarunnerEyes() = default;

std::shared_ptr<Datarunner::Shot> DatarunnerEyes::readFile(const std::filesystem::path& file,
                                                          const Datarunner::Pixels& own) {
    if (!impl_->filesLoaded_) {
        impl_->filesLoaded_ = true;
        if (!impl_->files_.loadEmbedded()) std::cout << "\nUEX Datarunner: the line reader did not load" << std::endl;
    }
    auto shot = std::make_shared<Datarunner::Shot>(
        Datarunner::readPicture(file, impl_->files_, Datarunner::unpackBgr(own)));
    return shot;
}

std::shared_ptr<Datarunner::Shot> DatarunnerEyes::readFrame(const Datarunner::Pixels& frame) {
    if (!impl_->framesLoaded_) {
        impl_->framesLoaded_ = true;
        if (!impl_->frames_.loadEmbedded()) std::cout << "\nUEX Datarunner: the line reader did not load" << std::endl;
    }
    auto shot = std::make_shared<Datarunner::Shot>(
        Datarunner::readPicture(Datarunner::unpackBgr(frame), impl_->frames_));
    shot->montage = frame;
    shot->jpeg.clear();
    return shot;
}

}  // namespace SCMiner
