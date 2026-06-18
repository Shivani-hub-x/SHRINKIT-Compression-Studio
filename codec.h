#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct CodecResult {
    bool        success      = false;
    std::string outputPath;
    std::string inputFormat;
    std::string outputFormat;
    uint64_t    originalSize = 0;
    uint64_t    resultSize   = 0;
    std::string errorMessage;

    double ratio() const {
        if (originalSize == 0) return 0.0;
        return (1.0 - (double)resultSize / originalSize) * 100.0;
    }
};

namespace FileUtils {
    std::vector<uint8_t> readBinary (const std::string& path);
    bool                 writeBinary(const std::string& path, const std::vector<uint8_t>& data);
    uint64_t             fileSize   (const std::string& path);
    std::string          extension  (const std::string& path);
    std::string          baseName   (const std::string& path);
    std::string          dirName    (const std::string& path);
    std::string          formatSize (uint64_t bytes);
}

class Codec {
public:
    static CodecResult compress  (const std::string& inputPath, const std::string& outputDir = "");
    static CodecResult decompress(const std::string& inputPath, const std::string& outputDir = "");
    static bool        isHufFile (const std::string& path);
};
