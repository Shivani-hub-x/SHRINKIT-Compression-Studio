#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "codec.h"

// LZW (Lempel-Ziv-Welch)
// Works on: any file — best for text, source code, structured data
// Output: .lzw
// Uses 12-bit codes (dictionary up to 4096 entries)
class LZW {
public:
    static std::vector<uint16_t> compress  (const std::vector<uint8_t>& data);
    static std::vector<uint8_t>  decompress(const std::vector<uint16_t>& codes);

    // Pack 12-bit codes into bytes and unpack
    static std::vector<uint8_t>  packCodes  (const std::vector<uint16_t>& codes);
    static std::vector<uint16_t> unpackCodes(const std::vector<uint8_t>& packed);
};

class LZWCodec {
public:
    static CodecResult compress  (const std::string& inputPath, const std::string& outputDir = "");
    static CodecResult decompress(const std::string& inputPath, const std::string& outputDir = "");
};
