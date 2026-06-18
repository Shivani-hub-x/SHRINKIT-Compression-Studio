#pragma once
#include "quadtree.h"
#include <string>
#include <cstdint>

// ── Result of image compress/decompress ──────────────────────────────────────
struct ImageResult {
    bool        success      = false;
    std::string outputPath;
    std::string inputFormat;
    std::string outputFormat;
    uint64_t    originalSize = 0;
    uint64_t    resultSize   = 0;
    int         width        = 0;
    int         height       = 0;
    std::string errorMessage;

    double ratio() const {
        if (originalSize == 0) return 0.0;
        return (1.0 - (double)resultSize / originalSize) * 100.0;
    }
};

// ── BMP / PGM I/O ────────────────────────────────────────────────────────────
namespace ImageIO {
    // Read BMP (24-bit) into Image
    Image readBMP(const std::string& path);
    // Write Image as BMP (24-bit)
    bool  writeBMP(const std::string& path, const Image& img);
    // Read PGM (P5 binary grayscale) into Image (grey stored as r=g=b)
    Image readPGM(const std::string& path);
    // Write Image as PGM
    bool  writePGM(const std::string& path, const Image& img);
}

// ── .qtc file operations ─────────────────────────────────────────────────────
class ImageCodec {
public:
    // Compress BMP or PGM → .qtc
    static ImageResult compress  (const std::string& inputPath,
                                   const std::string& outputDir = "");
    // Decompress .qtc → original format
    static ImageResult decompress(const std::string& inputPath,
                                   const std::string& outputDir = "");

    static bool isQtcFile(const std::string& path);
};
