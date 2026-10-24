#include "PaddleOcrService.h"

#if defined(DESKTODO_HAS_PADDLE_OCR)
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::wstring Utf8(const std::string& value) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

struct Library {
    HMODULE handle = nullptr;
    ~Library() { if (handle) FreeLibrary(handle); }
    void Load(const std::filesystem::path& path) {
        handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle) throw std::runtime_error("Unable to load OCR runtime library");
    }
};

struct RecognizedLine {
    std::wstring text;
    float confidence = 0;
};

struct TextBox {
    std::array<cv::Point2f, 4> points;
    cv::Rect bounds;
};

class Engine {
public:
    Engine() {
        wchar_t path[32768]{};
        DWORD count = GetModuleFileNameW(nullptr, path, _countof(path));
        if (!count || count >= _countof(path)) throw std::runtime_error("Invalid executable path");
        auto root = std::filesystem::path(path).parent_path() / L"ocr";
        ortLibrary_.Load(root / L"onnxruntime.dll");
        using GetApiBase = const OrtApiBase* (ORT_API_CALL*)();
        auto getApi = reinterpret_cast<GetApiBase>(GetProcAddress(ortLibrary_.handle, "OrtGetApiBase"));
        const OrtApi* api = getApi ? getApi()->GetApi(ORT_API_VERSION) : nullptr;
        if (!api) throw std::runtime_error("Unsupported ONNX Runtime version");
        Ort::InitApi(api);
        cvLibrary_.Load(root / L"opencv_world4120.dll");
        cv::setNumThreads(1);
        environment_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "DeskTodoOCR");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(static_cast<int>((std::min)(4u, (std::max)(1u, std::thread::hardware_concurrency()))));
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.DisableCpuMemArena();
        detector_ = std::make_unique<Ort::Session>(*environment_, (root / L"PP-OCRv6_det_medium.onnx").c_str(), options);
        recognizer_ = std::make_unique<Ort::Session>(*environment_, (root / L"PP-OCRv6_rec_medium.onnx").c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        auto metadata = recognizer_->GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
        if (!metadata) throw std::runtime_error("OCR model dictionary is missing");
        std::istringstream dictionary(metadata.get());
        std::string line;
        characters_.push_back(L""); // CTC blank.
        while (std::getline(dictionary, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            characters_.push_back(Utf8(line));
        }
        characters_.push_back(L" ");
        if (characters_.size() < 1000) throw std::runtime_error("OCR model dictionary is invalid");
    }

    std::wstring Recognize(HBITMAP bitmap) {
        cv::Mat image = ReadBitmap(bitmap);
        if (image.empty()) throw std::runtime_error("Invalid OCR bitmap");
        if (image.total() > 64ull * 1024 * 1024) throw std::runtime_error("OCR image exceeds 64 megapixels");
        // Preserve antialiasing; hard binarization damages small Chinese strokes.
        cv::Mat gray;
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        double minimum, maximum;
        cv::minMaxLoc(gray, &minimum, &maximum);
        if (maximum - minimum < 4) return {};
        double border = (cv::mean(gray.row(0))[0] + cv::mean(gray.row(gray.rows - 1))[0] +
                         cv::mean(gray.col(0))[0] + cv::mean(gray.col(gray.cols - 1))[0]) / 4;
        if (border < 100) cv::bitwise_not(image, image);
        const double scale = (std::min)(4.0, (std::max)(1.0, 160.0 / (std::min)(image.rows, image.cols)));
        // Bound intermediate memory as well as the original capture size.
        if (scale > 1 && image.total() * scale * scale < 16 * 1024 * 1024)
            cv::resize(image, image, {}, scale, scale, cv::INTER_CUBIC);
        cv::copyMakeBorder(image, image, 16, 16, 16, 16, cv::BORDER_CONSTANT, cv::Scalar(255, 255, 255));
        auto boxes = Detect(image);
        std::stable_sort(boxes.begin(), boxes.end(), [](const TextBox& a, const TextBox& b) {
            return a.bounds.y < b.bounds.y;
        });
        // Group text on the same visual row, then sort that row left to right.
        for (size_t first = 0; first < boxes.size();) {
            size_t last = first + 1;
            while (last < boxes.size() && boxes[last].bounds.y - boxes[first].bounds.y <
                   (std::max)(8, (std::min)(boxes[first].bounds.height, boxes[last].bounds.height) / 2)) ++last;
            std::stable_sort(boxes.begin() + first, boxes.begin() + last, [](const TextBox& a, const TextBox& b) {
                return a.bounds.x < b.bounds.x;
            });
            first = last;
        }
        std::wstring text;
        cv::Rect previous;
        for (const auto& box : boxes) {
            cv::Mat crop = Crop(image, box);
            auto line = ReadLine(crop);
            if (line.confidence < 0.75f) {
                cv::Mat contrast;
                cv::cvtColor(crop, contrast, cv::COLOR_BGR2GRAY);
                cv::normalize(contrast, contrast, 0, 255, cv::NORM_MINMAX);
                cv::cvtColor(contrast, contrast, cv::COLOR_GRAY2BGR);
                auto retry = ReadLine(contrast);
                if (retry.confidence > line.confidence) line = std::move(retry);
            }
            if (line.text.empty() || line.confidence < 0.5f) continue;
            if (!text.empty()) {
                const bool sameRow = std::abs(box.bounds.y - previous.y) < (std::max)(8, (std::min)(box.bounds.height, previous.height) / 2);
                text += sameRow ? L" " : L"\r\n";
            }
            text += line.text;
            previous = box.bounds;
        }
        // A tiny selection may contain a single text line that the detector
        // misses. Read the whole crop, but do not flatten multiline captures.
        if (text.empty() && image.rows <= 400 && image.cols >= image.rows / 2) {
            auto line = ReadLine(image);
            if (line.confidence >= 0.7f) text = line.text;
        }
        return text;
    }

private:
    Library ortLibrary_, cvLibrary_;
    std::unique_ptr<Ort::Env> environment_;
    std::unique_ptr<Ort::Session> detector_, recognizer_;
    std::vector<std::wstring> characters_;

    static cv::Mat ReadBitmap(HBITMAP bitmap) {
        BITMAP details{};
        if (!GetObjectW(bitmap, sizeof(details), &details) || details.bmWidth <= 0 || details.bmHeight <= 0 ||
            static_cast<unsigned long long>(details.bmWidth) * details.bmHeight > 64ull * 1024 * 1024)
            return {};
        cv::Mat bgra(details.bmHeight, details.bmWidth, CV_8UC4);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = details.bmWidth;
        info.bmiHeader.biHeight = -details.bmHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        int rows = GetDIBits(dc, bitmap, 0, details.bmHeight, bgra.data, &info, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (rows != details.bmHeight) return {};
        cv::Mat bgr;
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    }

    static std::vector<Ort::Value> Infer(Ort::Session& session, const cv::Mat& image, int paddedWidth = 0) {
        const int width = (std::max)(image.cols, paddedWidth), height = image.rows;
        const size_t plane = static_cast<size_t>(width) * height;
        std::vector<float> input(plane * 3, 0);
        for (int y = 0; y < height; ++y) for (int x = 0; x < image.cols; ++x) {
            const auto pixel = image.at<cv::Vec3b>(y, x);
            for (int c = 0; c < 3; ++c)
                input[c * plane + static_cast<size_t>(y) * width + x] = pixel[c] / 127.5f - 1.0f;
        }
        const std::array<int64_t, 4> shape{1, 3, height, width};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = session.GetInputNameAllocated(0, allocator);
        auto outputName = session.GetOutputNameAllocated(0, allocator);
        const char* inputs[]{inputName.get()};
        const char* outputs[]{outputName.get()};
        return session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    }

    std::vector<TextBox> Detect(const cv::Mat& image) {
        double ratio = (std::min)(1.0, 1536.0 / (std::max)(image.cols, image.rows));
        const int width = (std::max)(32, static_cast<int>(std::round(image.cols * ratio / 32)) * 32);
        const int height = (std::max)(32, static_cast<int>(std::round(image.rows * ratio / 32)) * 32);
        cv::Mat resized;
        cv::resize(image, resized, {width, height});
        auto output = Infer(*detector_, resized);
        auto shape = output[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1 || shape[2] <= 0 || shape[3] <= 0)
            throw std::runtime_error("Unexpected OCR detector output");
        const int mapHeight = static_cast<int>(shape[2]), mapWidth = static_cast<int>(shape[3]);
        cv::Mat scores(mapHeight, mapWidth, CV_32F, output[0].GetTensorMutableData<float>());
        cv::Mat mask;
        cv::compare(scores, 0.3, mask, cv::CMP_GT);
        cv::dilate(mask, mask, cv::Mat::ones(2, 2, CV_8U));
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(mask, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        std::vector<TextBox> boxes;
        for (const auto& contour : contours) {
            if (boxes.size() >= 1000) break;
            if (contour.size() < 3) continue;
            auto rectangle = cv::minAreaRect(contour);
            if ((std::min)(rectangle.size.width, rectangle.size.height) < 3) continue;
            const cv::Rect bounds = cv::boundingRect(contour) & cv::Rect(0, 0, mapWidth, mapHeight);
            cv::Mat regionMask(bounds.size(), CV_8U, cv::Scalar(0));
            cv::drawContours(regionMask, std::vector<std::vector<cv::Point>>{contour}, 0, cv::Scalar(255),
                             cv::FILLED, cv::LINE_8, {}, INT_MAX, {-bounds.x, -bounds.y});
            if (cv::mean(scores(bounds), regionMask)[0] < 0.5) continue;
            const float expansion = rectangle.size.area() * 1.6f /
                (2 * (rectangle.size.width + rectangle.size.height));
            rectangle.size.width += 2 * expansion;
            rectangle.size.height += 2 * expansion;
            std::array<cv::Point2f, 4> points;
            rectangle.points(points.data());
            for (auto& point : points) {
                point.x = (std::clamp)(point.x * image.cols / mapWidth, 0.0f, static_cast<float>(image.cols - 1));
                point.y = (std::clamp)(point.y * image.rows / mapHeight, 0.0f, static_cast<float>(image.rows - 1));
            }
            std::sort(points.begin(), points.end(), [](const cv::Point2f& a, const cv::Point2f& b) { return a.x < b.x; });
            if (points[0].y > points[1].y) std::swap(points[0], points[1]);
            if (points[2].y > points[3].y) std::swap(points[2], points[3]);
            TextBox box{{points[0], points[2], points[3], points[1]}, {}};
            box.bounds = cv::boundingRect(std::vector<cv::Point2f>(box.points.begin(), box.points.end()));
            boxes.push_back(box);
        }
        return boxes;
    }

    static cv::Mat Crop(const cv::Mat& image, const TextBox& box) {
        const auto& p = box.points;
        int width = (std::max)(2, static_cast<int>((std::max)(cv::norm(p[1] - p[0]), cv::norm(p[2] - p[3]))));
        int height = (std::max)(2, static_cast<int>((std::max)(cv::norm(p[3] - p[0]), cv::norm(p[2] - p[1]))));
        const std::array<cv::Point2f, 4> target{{{0, 0}, {static_cast<float>(width - 1), 0},
            {static_cast<float>(width - 1), static_cast<float>(height - 1)}, {0, static_cast<float>(height - 1)}}};
        cv::Mat crop;
        cv::warpPerspective(image, crop, cv::getPerspectiveTransform(p.data(), target.data()),
                            {width, height}, cv::INTER_CUBIC, cv::BORDER_REPLICATE);
        if (height > width * 1.5) cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
        return crop;
    }

    RecognizedLine ReadLine(const cv::Mat& image) {
        constexpr int height = 48;
        const int width = (std::clamp)(static_cast<int>(std::ceil(image.cols * height / static_cast<double>(image.rows))), 8, 4096);
        cv::Mat resized;
        cv::resize(image, resized, {width, height});
        auto output = Infer(*recognizer_, resized, (std::max)(320, width));
        auto shape = output[0].GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != 1 || shape[1] <= 0 || shape[2] != static_cast<int64_t>(characters_.size()))
            throw std::runtime_error("OCR character dictionary does not match model");
        const auto* scores = output[0].GetTensorData<float>();
        const size_t classes = characters_.size();
        size_t previous = 0, count = 0;
        RecognizedLine result;
        for (int64_t step = 0; step < shape[1]; ++step) {
            const float* row = scores + step * classes;
            const size_t index = static_cast<size_t>(std::max_element(row, row + classes) - row);
            if (index && index != previous) {
                result.text += characters_[index];
                result.confidence += row[index];
                ++count;
            }
            previous = index;
        }
        if (count) result.confidence /= count;
        return result;
    }
};
}
#endif

bool PaddleOcrService::RecognizeBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error) {
    text.clear();
    error.clear();
#if defined(DESKTODO_HAS_PADDLE_OCR)
    try {
        static std::mutex mutex;
        static std::unique_ptr<Engine> engine;
        std::lock_guard<std::mutex> lock(mutex);
        if (!engine) engine = std::make_unique<Engine>();
        text = engine->Recognize(bitmap);
        if (text.empty()) error = L"截图中没有识别到文字。";
        return !text.empty();
    } catch (const std::exception& exception) {
        error = L"本地高精度 OCR 暂不可用：" + Utf8(exception.what());
    } catch (...) {
        error = L"本地高精度 OCR 初始化失败。";
    }
#else
    error = L"当前构建未包含本地高精度 OCR。";
#endif
    return false;
}
