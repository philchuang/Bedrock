#pragma once
// Reading a commodity terminal off a picture. The window holds Datarunner::
// Shot (Datarunner.h) and does not include this.

#include "Datarunner.h"

#include <opencv2/core.hpp>

#include <filesystem>

namespace SCMiner {

class TextLineReader;

namespace Datarunner {

// A picture as UEX gets it with a report: at most 1080 pixels tall, a JPEG
// under UEX's 10 MB. Empty for an empty picture.
std::vector<unsigned char> pictureForUex(cv::Mat frame);
void addToMontage(cv::Mat& montage, const cv::Mat& frame, const std::vector<cv::Rect>& rows);
std::vector<unsigned char> montageForUex(const cv::Mat& montage, int frameHeight);

// A picture read. Where the game's picture is black, `own` is read instead.
Shot readPicture(const std::filesystem::path& file, TextLineReader& reader, const cv::Mat& own = cv::Mat());
Shot readPicture(const cv::Mat& bgr, TextLineReader& reader);
bool blackPicture(const cv::Mat& bgr);

// Tightly packed BGRA of a BGR or BGRA picture. Empty for an empty one.
Pixels packBgra(const cv::Mat& bgr);
// The BGR picture those bytes were packed from. Empty for an empty one.
cv::Mat unpackBgr(const Pixels& pixels);

}  // namespace Datarunner
}  // namespace SCMiner
