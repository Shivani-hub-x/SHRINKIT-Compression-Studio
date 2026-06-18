#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <string>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "codec.h"
#include "image_codec.h"
#include "rle.h"
#include "lzw.h"
#include "arith.h"
#include "brotli.h"
#include "png_codec.h"
#include "jpeg_codec.h"
#include "j2k_codec.h"
#include "webp_codec.h"
#include "flif_codec.h"
using namespace std;

// ── Screens ──────────────────────────────────────────────────────────────────
enum class Screen { Menu, Task, Compare };
enum class TaskKind {
    None,
    // Text lossless compress
    HuffCompress, RLECompress, LZWCompress, ArithCompress, BrotliCompress,
    // Text lossless decompress
    HuffDecompress, RLEDecompress, LZWDecompress, ArithDecompress, BrotliDecompress,
    // Image lossless compress
    QuadCompress, PNGCompress, FLIFCompress,
    // Image lossless decompress
    QuadDecompress, PNGDecompress, FLIFDecompress,
};

enum class CompareKind { TextLossless, ImageLossless };

// ── Complexity Info ──────────────────────────────────────────────────────────
struct ComplexityInfo {
    const char* algorithm;
    const char* timeBest;
    const char* timeAvg;
    const char* timeWorst;
    const char* spaceBest;
    const char* spaceAvg;
    const char* spaceWorst;
    float       refSpeed;   // relative speed (lower = faster, 1.0 baseline)
    float       refRatio;   // expected compression ratio % saved
};

static const ComplexityInfo g_textAlgos[] = {
    {"Huffman",    "O(n)",  "O(n log n)", "O(n log n)", "O(n)", "O(n)",   "O(n)",   1.0f, 45.0f},
    {"RLE",        "O(n)",  "O(n)",       "O(n)",       "O(1)", "O(n)",   "O(2n)",  0.3f, 20.0f},
    {"LZW",        "O(n)",  "O(n)",       "O(n)",       "O(n)", "O(n)",   "O(n*k)", 0.8f, 55.0f},
    {"Arithmetic", "O(n)",  "O(n)",       "O(n)",       "O(n)", "O(n)",   "O(n)",   1.2f, 50.0f},
    {"Brotli",     "O(n)",  "O(n)",       "O(n^2)",     "O(n)", "O(n)",   "O(n)",   2.0f, 60.0f},
};
static const ComplexityInfo g_imgLosslessAlgos[] = {
    {"QuadTree",    "O(n)",  "O(n log n)", "O(n^2)",     "O(n)", "O(n)", "O(n)",   1.5f, 40.0f},
    {"PNG/HUFFMAN", "O(n)",  "O(n)",       "O(n^2)",     "O(n)", "O(n)", "O(n)",   1.0f, 55.0f},
    {"FLIF",        "O(n)",  "O(n log n)", "O(n log n)", "O(n)", "O(n)", "O(n)",   2.5f, 65.0f},
};


static const ComplexityInfo* getTaskComplexity(TaskKind task) {
    switch (task) {
        case TaskKind::HuffCompress: case TaskKind::HuffDecompress:     return &g_textAlgos[0];
        case TaskKind::RLECompress:  case TaskKind::RLEDecompress:      return &g_textAlgos[1];
        case TaskKind::LZWCompress:  case TaskKind::LZWDecompress:      return &g_textAlgos[2];
        case TaskKind::ArithCompress: case TaskKind::ArithDecompress:   return &g_textAlgos[3];
        case TaskKind::BrotliCompress: case TaskKind::BrotliDecompress: return &g_textAlgos[4];
        case TaskKind::QuadCompress: case TaskKind::QuadDecompress:     return &g_imgLosslessAlgos[0];
        case TaskKind::PNGCompress:  case TaskKind::PNGDecompress:      return &g_imgLosslessAlgos[1];
        case TaskKind::FLIFCompress: case TaskKind::FLIFDecompress:     return &g_imgLosslessAlgos[2];
        default: return nullptr;
    }
}

// ── App State ─────────────────────────────────────────────────────────────────
struct AppState {
    Screen      screen  = Screen::Menu;
    TaskKind    task    = TaskKind::None;
    CompareKind compare = CompareKind::TextLossless;
    string      inputPath;
    string      result;
    string      formatErr;
    bool        working = false;
    bool        ok      = false;
    double      elapsedMs = 0.0;

    // Compare screen: drag-drop benchmark
    string      cmpFilePath;
    bool        cmpWorking = false;
    bool        cmpDone    = false;
    double      cmpTimes[5]  = {};   // measured ms per algo (max 5 algos)
    double      cmpRatios[5] = {};   // measured compression ratio %
    uint64_t    cmpOrigSize  = 0;
    uint64_t    cmpResultSizes[5] = {};
};

static AppState g_app;
static mutex    g_mutex;
static ImFont*  g_titleFont = nullptr;
static ImFont*  g_comicFont = nullptr;
static ImFont*  g_smallFont = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────
static string fmtSz(uint64_t b) {
    char buf[64];
    if      (b < 1024)      snprintf(buf,64,"%llu B",(unsigned long long)b);
    else if (b < 1024*1024) snprintf(buf,64,"%.2f KB",b/1024.0);
    else                    snprintf(buf,64,"%.2f MB",b/(1024.0*1024));
    return buf;
}

static string getExt(const string& path) {
    auto pos = path.rfind('.');
    if (pos == string::npos) return "";
    string ext = path.substr(pos);
    for (auto& c : ext) c = (char)tolower(c);
    return ext;
}

static ImVec4 lerpCol(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t,
                  a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t);
}

// ── Card definitions ──────────────────────────────────────────────────────────
struct CardInfo {
    const char* label;      // algorithm name
    const char* tag;        // e.g. "LOSSLESS TEXT"
    const char* accepts;    // accepted input formats
    const char* outputs;    // output format
    const char* desc;       // one-line description
    TaskKind    compKind;
    TaskKind    decompKind;
    ImVec4      baseCol;
    ImVec4      hoverCol;
    ImVec4      accentCol;  // tag + border colour
};

// ── Format validation ─────────────────────────────────────────────────────────
static bool isValidFormat(TaskKind task, const string& ext) {
    switch (task) {
        // Text compress — any file
        case TaskKind::HuffCompress:
        case TaskKind::RLECompress:
        case TaskKind::LZWCompress:
        case TaskKind::ArithCompress:
        case TaskKind::BrotliCompress:
            return true;
        // Text decompress
        case TaskKind::HuffDecompress:   return ext == ".huf";
        case TaskKind::RLEDecompress:    return ext == ".rle";
        case TaskKind::LZWDecompress:    return ext == ".lzw";
        case TaskKind::ArithDecompress:  return ext == ".arith";
        case TaskKind::BrotliDecompress: return ext == ".bro";
        // Image lossless compress
        case TaskKind::QuadCompress:
        case TaskKind::PNGCompress:
        case TaskKind::FLIFCompress:
            return ext == ".bmp" || ext == ".pgm";
        // Image lossless decompress
        case TaskKind::QuadDecompress:   return ext == ".qtc";
        case TaskKind::PNGDecompress:    return ext == ".png";
        case TaskKind::FLIFDecompress:   return ext == ".flif";
        default: return false;
    }
}

static string acceptedFormats(TaskKind task) {
    switch (task) {
        case TaskKind::HuffCompress:
        case TaskKind::RLECompress:
        case TaskKind::LZWCompress:
        case TaskKind::ArithCompress:
        case TaskKind::BrotliCompress:   return "Any file";
        case TaskKind::HuffDecompress:   return ".huf";
        case TaskKind::RLEDecompress:    return ".rle";
        case TaskKind::LZWDecompress:    return ".lzw";
        case TaskKind::ArithDecompress:  return ".arith";
        case TaskKind::BrotliDecompress: return ".bro";
        case TaskKind::QuadCompress:
        case TaskKind::PNGCompress:
        case TaskKind::FLIFCompress:     return ".bmp  .pgm";
        case TaskKind::QuadDecompress:   return ".qtc";
        case TaskKind::PNGDecompress:    return ".png";
        case TaskKind::FLIFDecompress:   return ".flif";
        default: return "";
    }
}

static string taskTitle(TaskKind task) {
    switch (task) {
        case TaskKind::HuffCompress:     return "Huffman  —  Compress";
        case TaskKind::HuffDecompress:   return "Huffman  —  Decompress";
        case TaskKind::RLECompress:      return "RLE  —  Compress";
        case TaskKind::RLEDecompress:    return "RLE  —  Decompress";
        case TaskKind::LZWCompress:      return "LZW  —  Compress";
        case TaskKind::LZWDecompress:    return "LZW  —  Decompress";
        case TaskKind::ArithCompress:    return "Arithmetic Coding  —  Compress";
        case TaskKind::ArithDecompress:  return "Arithmetic Coding  —  Decompress";
        case TaskKind::BrotliCompress:   return "Brotli  —  Compress";
        case TaskKind::BrotliDecompress: return "Brotli  —  Decompress";
        case TaskKind::QuadCompress:     return "QuadTree  —  Compress";
        case TaskKind::QuadDecompress:   return "QuadTree  —  Decompress";
        case TaskKind::PNGCompress:      return "PNG / HUFFMAN  —  Compress";
        case TaskKind::PNGDecompress:    return "PNG / HUFFMAN  —  Decompress";
        case TaskKind::FLIFCompress:     return "FLIF  —  Compress";
        case TaskKind::FLIFDecompress:   return "FLIF  —  Decompress";
        default: return "";
    }
}

static string taskSubtitle(TaskKind task) {
    switch (task) {
        case TaskKind::HuffCompress:     return "Compress any file using Huffman coding  ->  .huf";
        case TaskKind::HuffDecompress:   return "Restore original file from .huf archive";
        case TaskKind::RLECompress:      return "Compress using Run-Length Encoding  ->  .rle";
        case TaskKind::RLEDecompress:    return "Restore original file from .rle archive";
        case TaskKind::LZWCompress:      return "Compress using Lempel-Ziv-Welch  ->  .lzw";
        case TaskKind::LZWDecompress:    return "Restore original file from .lzw archive";
        case TaskKind::ArithCompress:    return "Compress using Arithmetic Coding  ->  .arith";
        case TaskKind::ArithDecompress:  return "Restore original file from .arith archive";
        case TaskKind::BrotliCompress:   return "Compress using Brotli LZ77  ->  .bro";
        case TaskKind::BrotliDecompress: return "Restore original file from .bro archive";
        case TaskKind::QuadCompress:     return "Compress BMP/PGM using QuadTree  ->  .qtc";
        case TaskKind::QuadDecompress:   return "Restore image from .qtc archive";
        case TaskKind::PNGCompress:      return "Compress BMP/PGM using PNG/HUFFMAN  ->  .png";
        case TaskKind::PNGDecompress:    return "Restore image from .png archive";
        case TaskKind::FLIFCompress:     return "Compress BMP/PGM using FLIF  ->  .flif";
        case TaskKind::FLIFDecompress:   return "Restore image from .flif archive";
        default: return "";
    }
}

// ── Workers ──────────────────────────────────────────────────────────────────
static void doWork() {
    CodecResult  cr; cr.success = false;
    ImageResult  ir; ir.success = false;
    bool isImage = false;

    auto t0 = chrono::high_resolution_clock::now();

    switch (g_app.task) {
        // ── Text lossless ──────────────────────────────────────────────────
        case TaskKind::HuffCompress:     cr = Codec::compress(g_app.inputPath);        break;
        case TaskKind::HuffDecompress:   cr = Codec::decompress(g_app.inputPath);      break;
        case TaskKind::RLECompress:      cr = RLECodec::compress(g_app.inputPath);     break;
        case TaskKind::RLEDecompress:    cr = RLECodec::decompress(g_app.inputPath);   break;
        case TaskKind::LZWCompress:      cr = LZWCodec::compress(g_app.inputPath);     break;
        case TaskKind::LZWDecompress:    cr = LZWCodec::decompress(g_app.inputPath);   break;
        case TaskKind::ArithCompress:    cr = ArithCodec::compress(g_app.inputPath);   break;
        case TaskKind::ArithDecompress:  cr = ArithCodec::decompress(g_app.inputPath); break;
        case TaskKind::BrotliCompress:   cr = BrotliCodec::compress(g_app.inputPath);  break;
        case TaskKind::BrotliDecompress: cr = BrotliCodec::decompress(g_app.inputPath);break;
        // ── Image lossless ─────────────────────────────────────────────────
        case TaskKind::QuadCompress:     ir = ImageCodec::compress(g_app.inputPath);   isImage=true; break;
        case TaskKind::QuadDecompress:   ir = ImageCodec::decompress(g_app.inputPath); isImage=true; break;
        case TaskKind::PNGCompress:      ir = PNGCodec::compress(g_app.inputPath);     isImage=true; break;
        case TaskKind::PNGDecompress:    ir = PNGCodec::decompress(g_app.inputPath);   isImage=true; break;
        case TaskKind::FLIFCompress:     ir = FLIFCodec::compress(g_app.inputPath);   isImage=true; break;
        case TaskKind::FLIFDecompress:   ir = FLIFCodec::decompress(g_app.inputPath); isImage=true; break;
        default:
            cr.errorMessage = "Unknown task."; break;
    }

    auto t1 = chrono::high_resolution_clock::now();
    double ms = chrono::duration<double, milli>(t1 - t0).count();

    lock_guard<mutex> lk(g_mutex);
    g_app.elapsedMs = ms;
    char buf[1024];
    if (!isImage) {
        if (cr.success) {
            double r = cr.ratio();
            if (r >= 0.0) {
                snprintf(buf, sizeof(buf),
                    "Success!\n\nFormat     : %s  ->  %s\nBefore     : %s\nAfter      : %s\nSaved      : %.1f%%\nTime       : %.2f ms\nOutput     : %s",
                    cr.inputFormat.c_str(), cr.outputFormat.c_str(),
                    FileUtils::formatSize(cr.originalSize).c_str(),
                    FileUtils::formatSize(cr.resultSize).c_str(),
                    r, ms, cr.outputPath.c_str());
            } else {
                snprintf(buf, sizeof(buf),
                    "Success!\n\nFormat     : %s  ->  %s\nBefore     : %s\nAfter      : %s\nExpanded   : %.1f%%  (output is LARGER than input)\nTime       : %.2f ms\nOutput     : %s\n\n"
                    "NOTE: The compressed file is larger because this algorithm's\n"
                    "metadata/dictionary overhead exceeds the savings on this\n"
                    "particular input. This is normal for small files or data with\n"
                    "high entropy (e.g. already-compressed or random data).\n"
                    "Try a different algorithm or a larger / more redundant file.",
                    cr.inputFormat.c_str(), cr.outputFormat.c_str(),
                    FileUtils::formatSize(cr.originalSize).c_str(),
                    FileUtils::formatSize(cr.resultSize).c_str(),
                    -r, ms, cr.outputPath.c_str());
            }
            g_app.result = buf; g_app.ok = true;
        } else {
            g_app.result = "Error: " + cr.errorMessage; g_app.ok = false;
        }
    } else {
        if (ir.success) {
            double r = ir.ratio();
            if (r >= 0.0) {
                snprintf(buf, sizeof(buf),
                    "Success!\n\nFormat     : %s  ->  %s\nResolution : %d x %d px\nBefore     : %s\nAfter      : %s\nSaved      : %.1f%%\nTime       : %.2f ms\nOutput     : %s",
                    ir.inputFormat.c_str(), ir.outputFormat.c_str(),
                    ir.width, ir.height,
                    fmtSz(ir.originalSize).c_str(),
                    fmtSz(ir.resultSize).c_str(),
                    r, ms, ir.outputPath.c_str());
            } else {
                snprintf(buf, sizeof(buf),
                    "Success!\n\nFormat     : %s  ->  %s\nResolution : %d x %d px\nBefore     : %s\nAfter      : %s\nExpanded   : %.1f%%  (output is LARGER than input)\nTime       : %.2f ms\nOutput     : %s\n\n"
                    "NOTE: The compressed file is larger because the image data has\n"
                    "too much detail/entropy for this algorithm's overhead to be\n"
                    "offset by savings. Try a larger image or a different codec.",
                    ir.inputFormat.c_str(), ir.outputFormat.c_str(),
                    ir.width, ir.height,
                    fmtSz(ir.originalSize).c_str(),
                    fmtSz(ir.resultSize).c_str(),
                    -r, ms, ir.outputPath.c_str());
            }
            g_app.result = buf; g_app.ok = true;
        } else {
            g_app.result = "Error: " + ir.errorMessage; g_app.ok = false;
        }
    }
    g_app.working = false;
}

static void launchWorker() {
    g_app.result = ""; g_app.working = true; g_app.ok = false; g_app.elapsedMs = 0.0;
    std::thread(doWork).detach();
}

// ── Compare benchmark worker ─────────────────────────────────────────────────
static void doCompareWork() {
    string path = g_app.cmpFilePath;
    CompareKind kind = g_app.compare;

    // Helper lambda to benchmark a text codec
    auto benchText = [&](int idx, CodecResult(*fn)(const string&, const string&)) {
        auto t0 = chrono::high_resolution_clock::now();
        CodecResult cr = fn(path, "");
        auto t1 = chrono::high_resolution_clock::now();
        double ms = chrono::duration<double, milli>(t1 - t0).count();
        lock_guard<mutex> lk(g_mutex);
        g_app.cmpTimes[idx] = ms;
        if (cr.success) {
            g_app.cmpOrigSize = cr.originalSize;
            g_app.cmpResultSizes[idx] = cr.resultSize;
            g_app.cmpRatios[idx] = cr.ratio();
            // Clean up output file
            remove(cr.outputPath.c_str());
        } else {
            g_app.cmpRatios[idx] = 0.0;
        }
    };

    auto benchImg = [&](int idx, ImageResult(*fn)(const string&, const string&)) {
        auto t0 = chrono::high_resolution_clock::now();
        ImageResult ir = fn(path, "");
        auto t1 = chrono::high_resolution_clock::now();
        double ms = chrono::duration<double, milli>(t1 - t0).count();
        lock_guard<mutex> lk(g_mutex);
        g_app.cmpTimes[idx] = ms;
        if (ir.success) {
            g_app.cmpOrigSize = ir.originalSize;
            g_app.cmpResultSizes[idx] = ir.resultSize;
            g_app.cmpRatios[idx] = ir.ratio();
            remove(ir.outputPath.c_str());
        } else {
            g_app.cmpRatios[idx] = 0.0;
        }
    };

    if (kind == CompareKind::TextLossless) {
        benchText(0, Codec::compress);
        benchText(1, RLECodec::compress);
        benchText(2, LZWCodec::compress);
        benchText(3, ArithCodec::compress);
        benchText(4, BrotliCodec::compress);
    } else if (kind == CompareKind::ImageLossless) {
        benchImg(0, ImageCodec::compress);
        benchImg(1, PNGCodec::compress);
        // FLIF not available
        { lock_guard<mutex> lk(g_mutex); g_app.cmpTimes[2] = 0; g_app.cmpRatios[2] = 0; }
    }

    lock_guard<mutex> lk(g_mutex);
    g_app.cmpWorking = false;
    g_app.cmpDone = true;
}

static void launchCompareWorker() {
    g_app.cmpWorking = true;
    g_app.cmpDone = false;
    for (int i = 0; i < 5; i++) { g_app.cmpTimes[i] = 0; g_app.cmpRatios[i] = 0; g_app.cmpResultSizes[i] = 0; }
    g_app.cmpOrigSize = 0;
    std::thread(doCompareWork).detach();
}

// ── renderMenu ────────────────────────────────────────────────────────────────
static void renderMenu(float W, float H) {
    float dt = ImGui::GetIO().DeltaTime;

    // ── App title ─────────────────────────────────────────────
    ImGui::SetCursorPosY(22.0f);
    ImGui::PushFont(g_comicFont);
    const char* title = "SHIRNKIT";//const char* title = "Compressor Studio";
    float tw = ImGui::CalcTextSize(title).x;
    ImGui::SetCursorPosX((W - tw) * 0.5f);
    ImGui::TextColored(ImVec4(0.88f, 0.93f, 1.0f, 1.0f), "%s", title);
    ImGui::PopFont();

    ImGui::SetCursorPosY(52.0f);
    const char* sub = "Select an algorithm and operation below";
    float sw = ImGui::CalcTextSize(sub).x;
    ImGui::SetCursorPosX((W - sw) * 0.5f);
    ImGui::TextColored(ImVec4(0.45f, 0.50f, 0.60f, 1.0f), "%s", sub);

    // ── Section label helper ───────────────────────────────────
    auto sectionLabel = [&](const char* label, ImVec4 col, float y) {
        ImGui::SetCursorPosY(y);
        ImGui::SetCursorPosX(30.0f);
        ImGui::PushFont(g_comicFont);
        ImGui::TextColored(col, "%s", label);
        ImGui::PopFont();
        // Underline
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float lw = ImGui::CalcTextSize(label).x + 16.0f;
        dl->AddLine({wp.x+30, cp.y-2}, {wp.x+30+lw, cp.y-2},
                    ImGui::ColorConvertFloat4ToU32(ImVec4(col.x,col.y,col.z,0.4f)), 1.0f);
    };

    // ── All cards definition ───────────────────────────────────
    // { label, accepts, outputs, desc, compKind, decompKind, base, hover, accent }
    struct Card {
        const char* label;
        const char* accepts;
        const char* outputs;
        const char* desc;
        TaskKind    compKind;
        TaskKind    decompKind;
        ImVec4      base, hover, accent;
        float       hoverAnim = 0.0f;
        bool        showDecomp = false; // toggled by right-click or button
    };

    // We use static so hover animation state persists frame to frame
    static Card cards[] = {
        // ── TEXT LOSSLESS ──────────────────────────────────────────────────────────────────────────────────────────
        { "Huffman",          "Any file",   ".huf",   "Variable-length prefix codes, optimal for skewed freq",
          TaskKind::HuffCompress,   TaskKind::HuffDecompress,
          {0.07f,0.18f,0.12f,1}, {0.10f,0.30f,0.20f,1}, {0.25f,0.80f,0.50f,1} },

        { "RLE",              "Any file",   ".rle",   "Run-length encoding, best for repeated byte sequences",
          TaskKind::RLECompress,    TaskKind::RLEDecompress,
          {0.07f,0.18f,0.12f,1}, {0.10f,0.30f,0.20f,1}, {0.25f,0.80f,0.50f,1} },

        { "LZW",              "Any file",   ".lzw",   "Dictionary coding, great for text and source code",
          TaskKind::LZWCompress,    TaskKind::LZWDecompress,
          {0.07f,0.18f,0.12f,1}, {0.10f,0.30f,0.20f,1}, {0.25f,0.80f,0.50f,1} },

        { "Arithmetic",       "Any file",   ".arith", "Near-entropy optimal coding, beats Huffman on skewed data",
          TaskKind::ArithCompress,  TaskKind::ArithDecompress,
          {0.07f,0.18f,0.12f,1}, {0.10f,0.30f,0.20f,1}, {0.25f,0.80f,0.50f,1} },

        { "Brotli",           "Any file",   ".bro",   "LZ77 back-references + entropy coding, web-optimised",
          TaskKind::BrotliCompress, TaskKind::BrotliDecompress,
          {0.07f,0.18f,0.12f,1}, {0.10f,0.30f,0.20f,1}, {0.25f,0.80f,0.50f,1} },

        // ── IMAGE LOSSLESS ─────────────────────────────────────────────────────────────────────────────────────────
        { "QuadTree",         ".bmp .pgm",  ".qtc",   "Spatial subdivision, visible block artifacts",
          TaskKind::QuadCompress,   TaskKind::QuadDecompress,
          {0.06f,0.12f,0.22f,1}, {0.09f,0.20f,0.38f,1}, {0.30f,0.60f,1.00f,1} },

        { "PNG / HUFFMAN",    ".bmp .pgm",  ".png",   "Row filter + LZW entropy, lossless pixel-perfect",
          TaskKind::PNGCompress,    TaskKind::PNGDecompress,
          {0.06f,0.12f,0.22f,1}, {0.09f,0.20f,0.38f,1}, {0.30f,0.60f,1.00f,1} },

        { "FLIF",             ".bmp .pgm",  ".flif",  "MANIAC prediction + arithmetic, state-of-art lossless",
          TaskKind::FLIFCompress,   TaskKind::FLIFDecompress,
          {0.06f,0.12f,0.22f,1}, {0.09f,0.20f,0.38f,1}, {0.30f,0.60f,1.00f,1} },

    };
//    static const int N_CARDS = 11;

    // ── Section Y positions ────────────────────────────────────
    float cardW = 190.0f, cardH = 148.0f, gap = 12.0f;
    float rowY[2];  // Y start of each section's card row

    // Section headers
    float sec0Y = 76.0f;
    float sec1Y = sec0Y + 28.0f + cardH + 20.0f;
    rowY[0] = sec0Y + 26.0f;
    rowY[1] = sec1Y + 26.0f;

    // Section row widths
    int counts[2] = {5, 3};
    ImVec4 secCols[2] = {
        {0.25f,0.80f,0.50f,1},
        {0.30f,0.60f,1.00f,1},
    };
    const char* secLabels[2] = {
        "TEXT  —  LOSSLESS",
        "IMAGE  —  LOSSLESS",
    };

    for (int s = 0; s < 2; s++)
        sectionLabel(secLabels[s], secCols[s],
                     s==0?sec0Y : sec1Y);

    // ── Draw cards ────────────────────────────────────────────
    int cardBase[2] = {0, 5}; // which cards belong to each section

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();

    for (int s = 0; s < 2; s++) {
        int n   = counts[s];
        float totalW = n * cardW + (n-1) * gap;
        float startX = (W - totalW) * 0.5f;

        for (int ci = 0; ci < n; ci++) {
            int idx = cardBase[s] + ci;
            Card& c = cards[idx];

            float x = startX + ci * (cardW + gap);
            float y = rowY[s];

            ImVec2 cMin = {wp.x + x,       wp.y + y};
            ImVec2 cMax = {wp.x + x + cardW, wp.y + y + cardH};

            // Hover animation
            bool hovered = ImGui::IsMouseHoveringRect(cMin, cMax);
            float target = hovered ? 1.0f : 0.0f;
            c.hoverAnim += (target - c.hoverAnim) * dt * 10.0f;
            float t = c.hoverAnim;

            // Background + border
            ImVec4 bg = lerpCol(c.base, c.hover, t);
            dl->AddRectFilled(cMin, cMax,
                ImGui::ColorConvertFloat4ToU32(bg), 10.0f);
            ImVec4 border = lerpCol(
                ImVec4(c.accent.x,c.accent.y,c.accent.z,0.15f),
                ImVec4(c.accent.x,c.accent.y,c.accent.z,0.70f), t);
            dl->AddRect(cMin, cMax,
                ImGui::ColorConvertFloat4ToU32(border), 10.0f, 0, 1.5f);

            // Top accent bar
            ImVec4 barCol = ImVec4(c.accent.x,c.accent.y,c.accent.z, 0.25f+0.50f*t);
            dl->AddRectFilled(cMin, {cMax.x, cMin.y+3},
                ImGui::ColorConvertFloat4ToU32(barCol), 10.0f);

            // Algorithm name
            float textX = cMin.x + 12.0f;
            float textY = cMin.y + 12.0f;
            ImGui::PushFont(g_comicFont);
            dl->AddText(g_comicFont, 16.0f, {textX, textY},
                ImGui::ColorConvertFloat4ToU32(
                    lerpCol(ImVec4(0.80f,0.84f,0.90f,1),
                            ImVec4(1.0f, 1.0f, 1.0f, 1), t)),
                c.label);
            ImGui::PopFont();

            // Accepts / outputs
            char inOut[64];
            snprintf(inOut, sizeof(inOut), "In: %s  Out: %s", c.accepts, c.outputs);
            ImGui::PushFont(g_smallFont);
            dl->AddText(g_smallFont, 11.0f, {textX, cMin.y+34.0f},
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(c.accent.x*0.8f, c.accent.y*0.8f, c.accent.z*0.8f, 0.85f+0.15f*t)),
                inOut);
            ImGui::PopFont();

            // Description (word-wrapped manually via TextWrapped in invisible child)
            ImGui::SetCursorPos({x+12.0f, y+52.0f});
            ImGui::PushTextWrapPos(x + cardW - 12.0f);
            ImGui::TextColored(
                ImVec4(0.48f+0.12f*t, 0.52f+0.10f*t, 0.60f+0.08f*t, 1.0f),
                "%s", c.desc);
            ImGui::PopTextWrapPos();

            // Compress / Decompress buttons
            float btnW  = (cardW - 28.0f) * 0.5f;
            float btnY2 = y + cardH - 36.0f;

            // Compress button
            ImGui::SetCursorPos({x + 10.0f, btnY2});
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(c.accent.x*0.35f, c.accent.y*0.35f, c.accent.z*0.35f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(c.accent.x*0.55f, c.accent.y*0.55f, c.accent.z*0.55f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(c.accent.x*0.25f, c.accent.y*0.25f, c.accent.z*0.25f, 1));

            char cmpId[32]; snprintf(cmpId, sizeof(cmpId), "Compress##%d", idx);
            if (ImGui::Button(cmpId, {btnW, 26.0f})) {
                g_app.task      = c.compKind;
                g_app.screen    = Screen::Task;
                g_app.inputPath = "";
                g_app.result    = "";
                g_app.formatErr = "";
                g_app.working   = false;
                g_app.ok        = false;
            }
            ImGui::PopStyleColor(3);

            // Decompress button
            ImGui::SetCursorPos({x + 14.0f + btnW, btnY2});
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(c.accent.x*0.20f, c.accent.y*0.20f, c.accent.z*0.20f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(c.accent.x*0.38f, c.accent.y*0.38f, c.accent.z*0.38f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(c.accent.x*0.15f, c.accent.y*0.15f, c.accent.z*0.15f, 1));

            char dcpId[32]; snprintf(dcpId, sizeof(dcpId), "Decomp##%d", idx);
            if (ImGui::Button(dcpId, {btnW, 26.0f})) {
                g_app.task      = c.decompKind;
                g_app.screen    = Screen::Task;
                g_app.inputPath = "";
                g_app.result    = "";
                g_app.formatErr = "";
                g_app.working   = false;
                g_app.ok        = false;
            }
            ImGui::PopStyleColor(3);
        }
    }

    // ── Compare buttons ──────────────────────────────────────────────────────
    float cmpBtnY = rowY[1] + cardH + 14.0f;
    const char* cmpLabels[2] = { "Compare Text Algos", "Compare Img Lossless" };
    CompareKind cmpKinds[2] = { CompareKind::TextLossless, CompareKind::ImageLossless };
    float cmpBtnW = 180.0f, cmpGap = 16.0f;
    float cmpTotalW = 2*cmpBtnW + 1*cmpGap;
    float cmpStartX = (W - cmpTotalW) * 0.5f;

    for (int i = 0; i < 2; i++) {
        ImGui::SetCursorPos({cmpStartX + i*(cmpBtnW+cmpGap), cmpBtnY});
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(secCols[i].x*0.25f, secCols[i].y*0.25f, secCols[i].z*0.25f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(secCols[i].x*0.45f, secCols[i].y*0.45f, secCols[i].z*0.45f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(secCols[i].x*0.15f, secCols[i].y*0.15f, secCols[i].z*0.15f, 1));
        char cmpId[48]; snprintf(cmpId, sizeof(cmpId), "%s##cmp%d", cmpLabels[i], i);
        if (ImGui::Button(cmpId, {cmpBtnW, 32.0f})) {
            g_app.compare = cmpKinds[i];
            g_app.screen  = Screen::Compare;
        }
        ImGui::PopStyleColor(3);
    }

    // Footer
    float footerY = cmpBtnY + 46.0f;
    ImGui::SetCursorPos({0, footerY});
    const char* footer = "Click Compress or Decompress on any card  \xe2\x80\x94  then drop your file";
    float fw = ImGui::CalcTextSize(footer).x;
    ImGui::SetCursorPosX((W - fw) * 0.5f);
    ImGui::TextColored(ImVec4(0.30f, 0.33f, 0.40f, 1.0f), "%s", footer);
}

// ── renderTask (YOUR existing code — unchanged) ───────────────────────────────
static void renderTask(float W, float H) {
    float pad = 30.0f;

    ImGui::SetCursorPos({pad, 18.0f});
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f,0.18f,0.24f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f,0.27f,0.34f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f,0.14f,0.18f,1));
    if (ImGui::Button("<  Back to Menu", {140,32})) {
        g_app.screen = Screen::Menu;
        g_app.inputPath = ""; g_app.result = "";
        g_app.formatErr = ""; g_app.working = false;
    }
    ImGui::PopStyleColor(3);

    ImGui::SetCursorPos({pad, 66.0f});
    ImGui::PushFont(g_comicFont);
    ImGui::TextColored(ImVec4(0.90f,0.93f,1.0f,1), "%s", taskTitle(g_app.task).c_str());
    ImGui::PopFont();
    ImGui::SetCursorPos({pad, 96.0f});
    ImGui::TextColored(ImVec4(0.50f,0.55f,0.65f,1), "%s", taskSubtitle(g_app.task).c_str());

    float dzY = 140.0f, dzW = W-2*pad, dzH = 100.0f;
    ImVec2 dzMin = {ImGui::GetWindowPos().x+pad, ImGui::GetWindowPos().y+dzY};
    ImVec2 dzMax = {dzMin.x+dzW, dzMin.y+dzH};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool dzHover = ImGui::IsMouseHoveringRect(dzMin, dzMax);
    ImVec4 dzBg = dzHover ? ImVec4(0.12f,0.16f,0.22f,1) : ImVec4(0.08f,0.10f,0.15f,1);
    dl->AddRectFilled(dzMin, dzMax, ImGui::ColorConvertFloat4ToU32(dzBg), 10.0f);
    dl->AddRect(dzMin, dzMax, ImGui::ColorConvertFloat4ToU32(
        ImVec4(0.30f,0.35f,0.45f, dzHover?0.8f:0.4f)), 10.0f, 0, 1.5f);

    if (g_app.inputPath.empty()) {
        const char* hint = "Drag & drop your file here";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText({dzMin.x+(dzW-ts.x)*0.5f, dzMin.y+24.0f},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.50f,0.56f,0.66f,1)), hint);
        char fmtBuf[128];
        snprintf(fmtBuf, sizeof(fmtBuf), "Accepted: %s", acceptedFormats(g_app.task).c_str());
        ImVec2 fs = ImGui::CalcTextSize(fmtBuf);
        dl->AddText({dzMin.x+(dzW-fs.x)*0.5f, dzMin.y+56.0f},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.38f,0.42f,0.50f,1)), fmtBuf);
    } else {
        const char* label = "Selected file:";
        ImVec2 ls = ImGui::CalcTextSize(label);
        dl->AddText({dzMin.x+(dzW-ls.x)*0.5f, dzMin.y+18.0f},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f,0.60f,0.70f,1)), label);
        string displayPath = g_app.inputPath;
        if (ImGui::CalcTextSize(displayPath.c_str()).x > dzW-40.0f) {
            auto sl = displayPath.rfind('/');
            if (sl != string::npos) displayPath = "..." + displayPath.substr(sl);
        }
        ImVec2 ps = ImGui::CalcTextSize(displayPath.c_str());
        dl->AddText({dzMin.x+(dzW-ps.x)*0.5f, dzMin.y+46.0f},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f,0.90f,0.55f,1)), displayPath.c_str());
        const char* clr = "[click to clear]";
        ImVec2 cs = ImGui::CalcTextSize(clr);
        dl->AddText({dzMin.x+(dzW-cs.x)*0.5f, dzMin.y+72.0f},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.40f,0.43f,0.50f,1)), clr);
    }

    ImGui::SetCursorPos({pad, dzY});
    if (ImGui::InvisibleButton("##dz", {dzW, dzH})) {
        if (!g_app.inputPath.empty() && !g_app.working) {
            g_app.inputPath=""; g_app.result=""; g_app.formatErr="";
        }
    }

    if (!g_app.formatErr.empty()) {
        ImGui::SetCursorPos({pad, dzY+dzH+8.0f});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f,0.08f,0.08f,1));
        ImGui::BeginChild("##fe", {dzW,44.0f}, true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(ImVec4(1.0f,0.55f,0.50f,1), "  %s", g_app.formatErr.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    float btnY  = dzY + dzH + (g_app.formatErr.empty()?20.0f:64.0f);
    float btnW2 = 200.0f, btnH = 42.0f;
    ImGui::SetCursorPos({(W-btnW2)*0.5f, btnY});

    if (g_app.working) {
        ImGui::TextColored(ImVec4(1,0.85f,0.3f,1), "    Processing...");
    } else {
        bool disabled = g_app.inputPath.empty();
        if (disabled) ImGui::BeginDisabled();
        bool isCompress =
            g_app.task==TaskKind::HuffCompress   || g_app.task==TaskKind::RLECompress  ||
            g_app.task==TaskKind::LZWCompress     || g_app.task==TaskKind::ArithCompress||
            g_app.task==TaskKind::BrotliCompress  || g_app.task==TaskKind::QuadCompress ||
            g_app.task==TaskKind::PNGCompress     || g_app.task==TaskKind::FLIFCompress;
        ImVec4 btnCol = isCompress ? ImVec4(0.12f,0.48f,0.32f,1) : ImVec4(0.20f,0.32f,0.55f,1);
        ImVec4 btnHov = isCompress ? ImVec4(0.18f,0.62f,0.42f,1) : ImVec4(0.28f,0.44f,0.72f,1);
        ImVec4 btnAct = isCompress ? ImVec4(0.08f,0.36f,0.24f,1) : ImVec4(0.14f,0.24f,0.40f,1);
        ImGui::PushStyleColor(ImGuiCol_Button,        btnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btnAct);
        if (ImGui::Button(isCompress?"Compress":"Decompress", {btnW2,btnH}))
            launchWorker();
        ImGui::PopStyleColor(3);
        if (disabled) ImGui::EndDisabled();
    }

    float resY = btnY+btnH+20.0f;
    float complexH = 130.0f; // space reserved for complexity table
    float resH = H - resY - complexH - 24.0f;
    if (resH < 40.0f) resH = 40.0f;
    ImGui::SetCursorPos({pad, resY});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f,0.07f,0.11f,1));
    ImGui::BeginChild("##res", {dzW,resH}, true);
    if (!g_app.result.empty()) {
        ImGui::TextColored(g_app.ok?ImVec4(0.40f,0.92f,0.65f,1):ImVec4(1.0f,0.45f,0.40f,1),
                           "%s", g_app.result.c_str());
    } else if (g_app.working) {
        ImGui::TextColored(ImVec4(0.8f,0.8f,0.3f,1), "Working, please wait...");
    } else {
        ImGui::TextColored(ImVec4(0.35f,0.38f,0.45f,1), "Result will appear here after processing.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── Complexity Info ──────────────────────────────────────────────────────
    const ComplexityInfo* ci = getTaskComplexity(g_app.task);
    if (ci) {
        float tblY = resY + resH + 8.0f;
        ImGui::SetCursorPos({pad, tblY});
        ImGui::PushFont(g_comicFont);
        ImGui::TextColored(ImVec4(0.75f,0.80f,0.90f,1), "Algorithm Complexity  —  %s", ci->algorithm);
        ImGui::PopFont();

        float tblDataY = tblY + 24.0f;
        float rowH = 22.0f;
        float tblW = dzW;
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        ImVec2 wp2 = ImGui::GetWindowPos();

        // Header
        ImVec2 thMin = {wp2.x+pad, wp2.y+tblDataY};
        ImVec2 thMax = {wp2.x+pad+tblW, wp2.y+tblDataY+rowH};
        dl2->AddRectFilled(thMin, thMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f,0.12f,0.18f,1)), 6.0f);

        float cols[4];
        cols[0] = pad + 10;
        cols[1] = pad + tblW*0.22f;
        cols[2] = pad + tblW*0.48f;
        cols[3] = pad + tblW*0.74f;
        const char* hdrs[] = {"Case", "Time Complexity", "Space Complexity", ""};
        ImGui::PushFont(g_smallFont);
        for (int c2 = 0; c2 < 3; c2++)
            dl2->AddText(g_smallFont, 14.0f, {wp2.x+cols[c2], wp2.y+tblDataY+5},
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f,0.88f,0.95f,1)), hdrs[c2]);

        const char* cases[3] = {"Best", "Average", "Worst"};
        const char* times[3] = {ci->timeBest, ci->timeAvg, ci->timeWorst};
        const char* spaces[3] = {ci->spaceBest, ci->spaceAvg, ci->spaceWorst};
        for (int r = 0; r < 3; r++) {
            float ry = tblDataY + rowH*(r+1);
            ImVec4 rbg = (r%2==0) ? ImVec4(0.07f,0.08f,0.12f,1) : ImVec4(0.06f,0.07f,0.10f,1);
            dl2->AddRectFilled({wp2.x+pad, wp2.y+ry}, {wp2.x+pad+tblW, wp2.y+ry+rowH},
                ImGui::ColorConvertFloat4ToU32(rbg), (r==2)?6.0f:0.0f);
            ImVec4 tc = ImVec4(0.70f,0.74f,0.82f,1);
            dl2->AddText(g_smallFont, 14.0f, {wp2.x+cols[0], wp2.y+ry+5},
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f,0.88f,0.95f,1)), cases[r]);
            dl2->AddText(g_smallFont, 14.0f, {wp2.x+cols[1], wp2.y+ry+5},
                ImGui::ColorConvertFloat4ToU32(tc), times[r]);
            dl2->AddText(g_smallFont, 14.0f, {wp2.x+cols[2], wp2.y+ry+5},
                ImGui::ColorConvertFloat4ToU32(tc), spaces[r]);
        }
        ImGui::PopFont();
    }
}

// ── renderCompare ─────────────────────────────────────────────────────────────
static void renderCompare(float W, float H) {
    float pad = 30.0f;
    ImGui::SetCursorPos({0, 0});
    ImGui::BeginChild("##cmpScroll", {W, H}, false, ImGuiWindowFlags_NoBackground);

    ImGui::SetCursorPos({pad, 18.0f});
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.16f,0.18f,0.24f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f,0.27f,0.34f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f,0.14f,0.18f,1));
    if (ImGui::Button("<  Back to Menu##cmp", {160,34})) {
        g_app.screen = Screen::Menu;
        // Reset benchmark state so graph is fresh next time
        g_app.cmpFilePath = "";
        g_app.cmpWorking = false;
        g_app.cmpDone = false;
        g_app.cmpOrigSize = 0;
        for (int i = 0; i < 5; i++) { g_app.cmpTimes[i]=0; g_app.cmpRatios[i]=0; g_app.cmpResultSizes[i]=0; }
    }
    ImGui::PopStyleColor(3);

    const char* titles[] = {
        "Text Lossless  \xe2\x80\x94  Algorithm Comparison",
        "Image Lossless  \xe2\x80\x94  Algorithm Comparison",
    };
    const char* descs[] = {
        "Compare all text compression algorithms side by side. The chart below shows theoretical relative speed and expected compression ratio.",
        "Compare image lossless compression algorithms. The chart shows speed and space savings on average.",
    };
    ImVec4 accentCols[] = {
        {0.25f,0.80f,0.50f,1}, {0.30f,0.60f,1.00f,1},
    };
    int ci = (int)g_app.compare;
    ImVec4 accent = accentCols[ci];

    ImGui::SetCursorPos({pad, 68.0f});
    ImGui::PushFont(g_comicFont);
    ImGui::TextColored(accent, "%s", titles[ci]);
    ImGui::PopFont();

    ImGui::SetCursorPos({pad, 102.0f});
    ImGui::PushTextWrapPos(W - pad);
    ImGui::TextColored(ImVec4(0.50f,0.55f,0.65f,1), "%s", descs[ci]);
    ImGui::PopTextWrapPos();

    const ComplexityInfo* algos = nullptr;
    int nAlgos = 0;
    switch (g_app.compare) {
        case CompareKind::TextLossless:  algos = g_textAlgos;        nAlgos = 5; break;
        case CompareKind::ImageLossless: algos = g_imgLosslessAlgos; nAlgos = 3; break;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    wp.y -= ImGui::GetScrollY();   // offset all drawing by scroll position
    float cW = W - 2*pad;

    // Helper: draw a styled block background
    auto drawBlock = [&](float y, float h) {
        ImVec2 mn = {wp.x+pad, wp.y+y}, mx = {wp.x+pad+cW, wp.y+y+h};
        dl->AddRectFilled(mn, mx, ImGui::ColorConvertFloat4ToU32(ImVec4(0.055f,0.065f,0.10f,1)), 12.0f);
        dl->AddRect(mn, mx, ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x,accent.y,accent.z,0.25f)), 12.0f, 0, 1.5f);
        dl->AddRectFilled(mn, {mx.x, mn.y+4}, ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x,accent.y,accent.z,0.5f)), 12.0f);
    };

    // ═══ BLOCK 1: THEORETICAL CHART ═══
    float b1Y = 132.0f, chartH = 330.0f;
    drawBlock(b1Y, chartH);

    ImGui::PushFont(g_comicFont);
    dl->AddText(g_comicFont, 20.0f, {wp.x+pad+16, wp.y+b1Y+14},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f,0.88f,0.95f,1)), "Theoretical Performance Overview");
    ImGui::PopFont();

    dl->AddText({wp.x+pad+16, wp.y+b1Y+44},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.50f,0.55f,0.65f,1)),
        "Left bar = Relative Speed (lower is faster)    Right bar = Expected Compression Ratio %");

    float bTop = b1Y + 68.0f, bBot = b1Y + chartH - 56.0f, bH = bBot - bTop;
    float gW = cW / (float)(nAlgos + 1);

    for (int g = 0; g <= 4; g++) {
        float gy = bBot - (bH * g / 4.0f);
        dl->AddLine({wp.x+pad+10, wp.y+gy}, {wp.x+pad+cW-10, wp.y+gy},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f,0.17f,0.22f,0.5f)), 1.0f);
    }

    float maxSpd = 0;
    for (int i = 0; i < nAlgos; i++) if (algos[i].refSpeed > maxSpd) maxSpd = algos[i].refSpeed;
    maxSpd *= 1.25f;

    for (int i = 0; i < nAlgos; i++) {
        float cx = pad + gW * (i + 1);
        float bw = gW * 0.3f;

        float sH = (algos[i].refSpeed / maxSpd) * bH;
        dl->AddRectFilled({wp.x+cx-bw-3, wp.y+bBot-sH}, {wp.x+cx-3, wp.y+bBot},
            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x*0.5f,accent.y*0.5f,accent.z*0.5f,0.85f)), 4.0f);

        float rH = (algos[i].refRatio / 100.0f) * bH;
        dl->AddRectFilled({wp.x+cx+3, wp.y+bBot-rH}, {wp.x+cx+bw+3, wp.y+bBot},
            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x,accent.y,accent.z,0.85f)), 4.0f);

        char sL[16]; snprintf(sL, sizeof(sL), "%.1fx", algos[i].refSpeed);
        ImVec2 ss = ImGui::CalcTextSize(sL);
        dl->AddText({wp.x+cx-bw/2-ss.x/2-3, wp.y+bBot-sH-18},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f,0.80f,0.90f,1)), sL);

        char rL[16]; snprintf(rL, sizeof(rL), "%.0f%%", algos[i].refRatio);
        ImVec2 rs = ImGui::CalcTextSize(rL);
        dl->AddText({wp.x+cx+bw/2-rs.x/2+3, wp.y+bBot-rH-18},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f,0.80f,0.90f,1)), rL);

        ImVec2 ns = ImGui::CalcTextSize(algos[i].algorithm);
        dl->AddText({wp.x+cx-ns.x/2, wp.y+bBot+12},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.82f,0.86f,0.94f,1)), algos[i].algorithm);
    }

    // Legend
    float lY = b1Y + chartH - 30.0f, lX = pad + cW - 340.0f;
    dl->AddRectFilled({wp.x+lX, wp.y+lY}, {wp.x+lX+14, wp.y+lY+12},
        ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x*0.5f,accent.y*0.5f,accent.z*0.5f,0.85f)), 2.0f);
    dl->AddText({wp.x+lX+20, wp.y+lY-1},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.65f,0.70f,0.80f,1)), "Relative Speed");
    dl->AddRectFilled({wp.x+lX+150, wp.y+lY}, {wp.x+lX+164, wp.y+lY+12},
        ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x,accent.y,accent.z,0.85f)), 2.0f);
    dl->AddText({wp.x+lX+170, wp.y+lY-1},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.65f,0.70f,0.80f,1)), "Compression Ratio %");

    // ═══ BLOCK 2: DRAG & DROP BENCHMARK ═══
    float b2Y = b1Y + chartH + 20.0f;
    float b2H = g_app.cmpDone ? 370.0f : 130.0f;
    drawBlock(b2Y, b2H);

    ImGui::PushFont(g_comicFont);
    dl->AddText(g_comicFont, 20.0f, {wp.x+pad+16, wp.y+b2Y+14},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f,0.88f,0.95f,1)), "Real File Benchmark");
    ImGui::PopFont();

    if (g_app.cmpFilePath.empty() && !g_app.cmpWorking && !g_app.cmpDone) {
        float dzY = b2Y + 50.0f, dzH = 56.0f;
        ImVec2 dzMn = {wp.x+pad+16, wp.y+dzY}, dzMx = {wp.x+pad+cW-16, wp.y+dzY+dzH};
        bool dzH2 = ImGui::IsMouseHoveringRect(dzMn, dzMx);
        dl->AddRectFilled(dzMn, dzMx, ImGui::ColorConvertFloat4ToU32(
            dzH2 ? ImVec4(0.10f,0.14f,0.20f,1) : ImVec4(0.07f,0.09f,0.14f,1)), 8.0f);
        dl->AddRect(dzMn, dzMx, ImGui::ColorConvertFloat4ToU32(
            ImVec4(accent.x,accent.y,accent.z, dzH2?0.6f:0.3f)), 8.0f, 0, 1.5f);
        const char* hint = "Drag & drop a file here to benchmark all algorithms with YOUR data";
        ImVec2 hs = ImGui::CalcTextSize(hint);
        dl->AddText({dzMn.x+(cW-32-hs.x)*0.5f, dzMn.y+18},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.55f,0.60f,0.72f,1)), hint);
    } else if (g_app.cmpWorking) {
        dl->AddText({wp.x+pad+16, wp.y+b2Y+52},
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f,0.85f,0.3f,1)), "Benchmarking all algorithms... please wait.");
        string fn = g_app.cmpFilePath; auto sl=fn.rfind('/'); if(sl!=string::npos) fn=fn.substr(sl+1);
        char fL[256]; snprintf(fL,sizeof(fL),"File: %s",fn.c_str());
        dl->AddText({wp.x+pad+16, wp.y+b2Y+78}, ImGui::ColorConvertFloat4ToU32(ImVec4(0.65f,0.70f,0.80f,1)), fL);
    } else if (g_app.cmpDone) {
        string fn = g_app.cmpFilePath; auto sl=fn.rfind('/'); if(sl!=string::npos) fn=fn.substr(sl+1);
        char fI[256]; snprintf(fI,sizeof(fI),"File: %s  (%s)", fn.c_str(), fmtSz(g_app.cmpOrigSize).c_str());
        dl->AddText({wp.x+pad+16, wp.y+b2Y+46}, ImGui::ColorConvertFloat4ToU32(ImVec4(0.70f,0.75f,0.85f,1)), fI);

        float rbT = b2Y + 74.0f, rbB = b2Y + b2H - 60.0f, rbH2 = rbB - rbT;
        float rgW = cW / (float)(nAlgos + 1);
        double maxT = 1.0;
        for (int i = 0; i < nAlgos; i++) if (g_app.cmpTimes[i] > maxT) maxT = g_app.cmpTimes[i];
        maxT *= 1.25;

        for (int g = 0; g <= 4; g++) {
            float gy = rbB - (rbH2 * g / 4.0f);
            dl->AddLine({wp.x+pad+10, wp.y+gy}, {wp.x+pad+cW-10, wp.y+gy},
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.15f,0.17f,0.22f,0.4f)), 1.0f);
        }
        for (int i = 0; i < nAlgos; i++) {
            float cx = pad + rgW*(i+1), bw = rgW*0.25f;

            // Time bar — clamp to chart area
            float tH = (float)(g_app.cmpTimes[i]/maxT)*rbH2;
            if (tH < 2) tH = 2;
            if (tH > rbH2) tH = rbH2;
            dl->AddRectFilled({wp.x+cx-bw-4, wp.y+rbB-tH}, {wp.x+cx-4, wp.y+rbB},
                ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x*0.5f,accent.y*0.5f,accent.z*0.5f,0.85f)), 4.0f);

            // Ratio bar — clamp negative values to 0, cap at chart height
            float ratioVal = (float)g_app.cmpRatios[i];
            if (ratioVal < 0.0f) ratioVal = 0.0f;
            if (ratioVal > 100.0f) ratioVal = 100.0f;
            float rH2 = (ratioVal / 100.0f) * rbH2;
            if (rH2 < 2 && g_app.cmpRatios[i] > 0) rH2 = 2;
            if (rH2 > rbH2) rH2 = rbH2;
            dl->AddRectFilled({wp.x+cx+4, wp.y+rbB-rH2}, {wp.x+cx+bw+4, wp.y+rbB},
                ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x,accent.y,accent.z,0.85f)), 4.0f);

            // Time label — keep inside chart
            char tL[32]; snprintf(tL,sizeof(tL),"%.1fms",g_app.cmpTimes[i]);
            ImVec2 ts=ImGui::CalcTextSize(tL);
            float tLabelY = wp.y+rbB-tH-18;
            if (tLabelY < wp.y+rbT) tLabelY = wp.y+rbT;
            dl->AddText({wp.x+cx-bw/2-ts.x/2-4, tLabelY},
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f,0.80f,0.90f,1)), tL);

            // Ratio label — show actual value (may be negative), keep inside chart
            char rL[32]; snprintf(rL,sizeof(rL),"%.1f%%",g_app.cmpRatios[i]);
            ImVec2 rs=ImGui::CalcTextSize(rL);
            float rLabelY = wp.y+rbB-rH2-18;
            if (rLabelY < wp.y+rbT) rLabelY = wp.y+rbT;
            dl->AddText({wp.x+cx+bw/2-rs.x/2+4, rLabelY},
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f,0.80f,0.90f,1)), rL);

            // Algorithm name label — below chart with proper spacing
            ImVec2 ns=ImGui::CalcTextSize(algos[i].algorithm);
            dl->AddText({wp.x+cx-ns.x/2, wp.y+rbB+10},
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.82f,0.86f,0.94f,1)), algos[i].algorithm);
        }
        float rLY = b2Y+b2H-40.0f, rLX = pad+cW-360.0f;
        dl->AddRectFilled({wp.x+rLX,wp.y+rLY},{wp.x+rLX+14,wp.y+rLY+12},
            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x*0.5f,accent.y*0.5f,accent.z*0.5f,0.85f)),2.0f);
        dl->AddText({wp.x+rLX+20,wp.y+rLY-1}, ImGui::ColorConvertFloat4ToU32(ImVec4(0.65f,0.70f,0.80f,1)),"Time Taken (ms)");
        dl->AddRectFilled({wp.x+rLX+175,wp.y+rLY},{wp.x+rLX+189,wp.y+rLY+12},
            ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x,accent.y,accent.z,0.85f)),2.0f);
        dl->AddText({wp.x+rLX+195,wp.y+rLY-1}, ImGui::ColorConvertFloat4ToU32(ImVec4(0.65f,0.70f,0.80f,1)),"Space Saved %");

        ImGui::SetCursorPos({pad+16, b2Y+b2H-40});
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x*0.2f,accent.y*0.2f,accent.z*0.2f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x*0.35f,accent.y*0.35f,accent.z*0.35f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x*0.12f,accent.y*0.12f,accent.z*0.12f,1));
        if (ImGui::Button("Drop another file##rst", {170, 26})) {
            g_app.cmpFilePath = ""; g_app.cmpDone = false;
        }
        ImGui::PopStyleColor(3);
    }

    // ═══ BLOCK 3: COMPLEXITY TABLE ═══
    float b3Y = b2Y + b2H + 20.0f;
    float tblRH = 30.0f;
    float b3H = 54.0f + tblRH * (nAlgos + 1) + 10.0f;
    drawBlock(b3Y, b3H);

    ImGui::PushFont(g_comicFont);
    dl->AddText(g_comicFont, 20.0f, {wp.x+pad+16, wp.y+b3Y+14},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.85f,0.88f,0.95f,1)),
        "Algorithm Complexity  (Best / Average / Worst)");
    ImGui::PopFont();

    float tY = b3Y + 52.0f;
    dl->AddRectFilled({wp.x+pad+8, wp.y+tY}, {wp.x+pad+cW-8, wp.y+tY+tblRH},
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f,0.12f,0.18f,1)), 6.0f);

    float tI = cW - 16.0f;
    float co[7] = { pad+18, pad+tI*0.14f, pad+tI*0.27f, pad+tI*0.42f, pad+tI*0.57f, pad+tI*0.72f, pad+tI*0.86f };
    const char* hd[] = {"Algorithm","Time Best","Time Avg","Time Worst","Space Best","Space Avg","Space Worst"};

    ImGui::PushFont(g_smallFont);
    for (int c=0;c<7;c++)
        dl->AddText(g_smallFont, 14.0f, {wp.x+co[c], wp.y+tY+8},
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.88f,0.90f,0.97f,1)), hd[c]);

    for (int i=0;i<nAlgos;i++) {
        float ry = tY + tblRH*(i+1);
        ImVec4 bg = (i%2==0)?ImVec4(0.07f,0.08f,0.12f,1):ImVec4(0.06f,0.07f,0.10f,1);
        dl->AddRectFilled({wp.x+pad+8, wp.y+ry}, {wp.x+pad+cW-8, wp.y+ry+tblRH},
            ImGui::ColorConvertFloat4ToU32(bg), (i==nAlgos-1)?6.0f:0.0f);
        ImVec4 tc = ImVec4(0.72f,0.76f,0.84f,1);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[0],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(ImVec4(0.88f,0.90f,0.97f,1)),algos[i].algorithm);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[1],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(tc),algos[i].timeBest);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[2],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(tc),algos[i].timeAvg);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[3],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(tc),algos[i].timeWorst);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[4],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(tc),algos[i].spaceBest);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[5],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(tc),algos[i].spaceAvg);
        dl->AddText(g_smallFont,14.0f,{wp.x+co[6],wp.y+ry+8},ImGui::ColorConvertFloat4ToU32(tc),algos[i].spaceWorst);
    }
    ImGui::PopFont();

    ImGui::SetCursorPos({0, b3Y + b3H + 20.0f});
    ImGui::Dummy({1, 1});
    ImGui::EndChild();
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window* window = SDL_CreateWindow(
        "Compressor Studio",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1160, 850, SDL_WINDOW_RESIZABLE);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Load fonts
    struct FC { const char* path; bool isTTC; };
    FC candidates[] = {
        {"/System/Library/Fonts/Supplemental/Arial.ttf", false},
        {"/System/Library/Fonts/HelveticaNeue.ttc",      true },
        {"/System/Library/Fonts/Avenir Next.ttc",        true },
    };
    ImFont* fontMain = nullptr;
    for (auto& c : candidates) {
        ImFontConfig fc; fc.FontNo = 0;
        fontMain    = io.Fonts->AddFontFromFileTTF(c.path, 17.0f, c.isTTC?&fc:nullptr);
        ImFontConfig fc2; fc2.FontNo = 0;
        g_titleFont = io.Fonts->AddFontFromFileTTF(c.path, 21.0f, c.isTTC?&fc2:nullptr);
        ImFontConfig fc3; fc3.FontNo = 0;
        g_smallFont = io.Fonts->AddFontFromFileTTF(c.path, 14.0f, c.isTTC?&fc3:nullptr);
        if (fontMain) break;
    }
    if (!fontMain)    io.Fonts->AddFontDefault();
    if (!g_titleFont) g_titleFont = io.Fonts->Fonts[0];
    if (!g_smallFont) g_smallFont = io.Fonts->Fonts[0];

    // Load Comic Sans for titles (italic-style used for headings only)
    g_comicFont = io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/Supplemental/Comic Sans MS Bold.ttf", 24.0f);
    if (!g_comicFont) {
        g_comicFont = io.Fonts->AddFontFromFileTTF(
            "/System/Library/Fonts/Supplemental/Comic Sans MS.ttf", 24.0f);
    }
    if (!g_comicFont) g_comicFont = g_titleFont;  // fallback

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding    = {0,0};
    style.FramePadding     = {10,5};
    style.ItemSpacing      = {8,6};
    style.ChildRounding    = 8.0f;
    style.FrameRounding    = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]      = {0.060f,0.060f,0.090f,1.0f};//main window
    colors[ImGuiCol_ChildBg]       = {0.070f,0.075f,0.110f,1.0f};
    colors[ImGuiCol_Border]        = {0.15f, 0.17f, 0.22f, 1.0f};
    colors[ImGuiCol_ScrollbarBg]   = {0.05f, 0.05f, 0.08f, 1.0f};
    colors[ImGuiCol_ScrollbarGrab] = {0.20f, 0.22f, 0.28f, 1.0f};

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;

            if (ev.type == SDL_DROPFILE) {
                string path = ev.drop.file;
                SDL_free(ev.drop.file);
                lock_guard<mutex> lk(g_mutex);

                if (g_app.screen == Screen::Task && !g_app.working) {
                    string ext = getExt(path);
                    if (isValidFormat(g_app.task, ext)) {
                        g_app.inputPath = path;
                        g_app.result    = "";
                        g_app.formatErr = "";
                    } else {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                            "Format '%s' not accepted here.  Expected: %s",
                            ext.empty()?"(none)":ext.c_str(),
                            acceptedFormats(g_app.task).c_str());
                        g_app.formatErr = msg;
                        g_app.inputPath = "";
                        g_app.result    = "";
                    }
                }

                // Compare screen: accept any file for benchmarking
                if (g_app.screen == Screen::Compare && !g_app.cmpWorking) {
                    g_app.cmpFilePath = path;
                    launchCompareWorker();
                }
            }
        }

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);
        float W=(float)winW, H=(float)winH;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({W,H});
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoScrollWithMouse|
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        if (g_app.screen == Screen::Menu)
            renderMenu(W, H);
        else if (g_app.screen == Screen::Compare)
            renderCompare(W, H);
        else
            renderTask(W, H);

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 14,15,23,255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
