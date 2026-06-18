#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "codec.h"  // reuse CodecResult and FileUtils

// Run-Length Encoding
// Works on: any file
// Best for: files with long runs of repeated bytes (BMP, simple images, sparse data)
// Output: .rle
class RLE {
public:
    // Compress raw bytes using RLE
    // Format: [0xFF escape][count][byte] for runs of 3+
    //         raw byte otherwise (if not 0xFF)
    //         [0xFF][0x00] to represent literal 0xFF
    static std::vector<uint8_t> compress  (const std::vector<uint8_t>& data);
    static std::vector<uint8_t> decompress(const std::vector<uint8_t>& data,
                                            uint32_t originalSize);
};

class RLECodec {
public:
    static CodecResult compress  (const std::string& inputPath, const std::string& outputDir = "");
    static CodecResult decompress(const std::string& inputPath, const std::string& outputDir = "");
};
