// C ABI shared by the OCR worker and the PDF worker's layout detector.
// Executed only in the disposable OCR worker, never in the GUI process.
#include "OcrLite.h"
#include "DbNet.h"
#include "OcrUtils.h"
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cstring>
#include <exception>
#include <sstream>

// Geometry only: no recognition/angle model and no language dictionary loaded.
// Coordinates are in the unmodified input image, including all four corners.
extern "C" __declspec(dllexport) int YiyiDetect(const unsigned char *png, int size, char *output, int capacity)
{
    try {
        if (!png || size <= 0 || !output || capacity < 2) return -2;
        cv::setNumThreads(4);
        auto image = cv::imdecode(std::vector<unsigned char>(png, png + size), cv::IMREAD_COLOR);
        if (image.empty()) return -2;
        const int padding = 24;
        cv::Mat padded;
        cv::copyMakeBorder(image, padded, padding, padding, padding, padding, cv::BORDER_CONSTANT, cv::Scalar(255, 255, 255));
        DbNet net; net.setNumThread(4); net.initModel("det.onnx");
        auto scale = getScaleParam(padded, std::min(2448, std::max(padded.cols, padded.rows)));
        const auto boxes = net.getTextBoxes(padded, scale, .5f, .3f, 1.5f);
        std::ostringstream json; json << "[";
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (i) json << ',';
            json << '[';
            for (size_t p = 0; p < boxes[i].boxPoint.size(); ++p) {
                if (p) json << ',';
                json << boxes[i].boxPoint[p].x - padding << ',' << boxes[i].boxPoint[p].y - padding;
            }
            json << ']';
        }
        json << ']'; const auto bytes = json.str();
        if (bytes.size() >= static_cast<size_t>(capacity)) return -3;
        std::memcpy(output, bytes.c_str(), bytes.size() + 1);
        return static_cast<int>(bytes.size());
    } catch (...) { return -1; }
}

extern "C" __declspec(dllexport) int YiyiOcr(const unsigned char *png, int size, char *output, int capacity, float *confidence)
{
    try {
        cv::setNumThreads(4);
        auto image = cv::imdecode(std::vector<unsigned char>(png, png + size), cv::IMREAD_COLOR);
        if (image.empty()) return -2;
        OcrLite ocr;
        ocr.initLogger(false, false, false);
        ocr.setNumThread(4);
        ocr.initModels("det.onnx", "cls.onnx", "rec.onnx", "keys.txt");
        auto result = ocr.detect(image, 24, 2048, 0.5f, 0.3f, 1.6f, true, false);
        // Contour enumeration order is not reading order. Group rows, then sort
        // each row from left to right using the detected text rectangles.
        auto bounds = [](const TextBlock &block) { return cv::boundingRect(block.boxPoint); };
        std::stable_sort(result.textBlocks.begin(), result.textBlocks.end(), [&](const auto &a, const auto &b) { return bounds(a).y < bounds(b).y; });
        std::string text;
        for (size_t first = 0; first < result.textBlocks.size();) {
            const auto row = bounds(result.textBlocks[first]);
            size_t last = first + 1;
            while (last < result.textBlocks.size() && bounds(result.textBlocks[last]).y - row.y < std::max(3, row.height / 2)) ++last;
            std::stable_sort(result.textBlocks.begin() + first, result.textBlocks.begin() + last, [&](const auto &a, const auto &b) { return bounds(a).x < bounds(b).x; });
            for (size_t i = first; i < last; ++i) { if (i > first) text += ' '; text += result.textBlocks[i].text; }
            text += '\n'; first = last;
        }
        if (text.size() >= static_cast<size_t>(capacity)) return -3;
        std::memcpy(output, text.c_str(), text.size() + 1);
        double score = 0; int count = 0;
        for (const auto &block : result.textBlocks) for (float value : block.charScores) { score += value; ++count; }
        *confidence = count ? static_cast<float>(score / count) : 0;
        return static_cast<int>(text.size());
    } catch (const std::exception &error) {
        // Diagnostic only, never a successful recognition result.
        const auto length = std::min(std::strlen(error.what()), static_cast<size_t>(capacity - 1));
        std::memcpy(output, error.what(), length); output[length] = 0;
        return -1;
    } catch (...) { return -1; }
}
