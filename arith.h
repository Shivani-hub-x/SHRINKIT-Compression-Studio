#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "codec.h"

// Arithmetic Coding — lossless, works on any file
// Better compression than Huffman especially for skewed distributions
// Output: .arith
class ArithCodec {
public:
    static CodecResult compress  (const std::string& inputPath, const std::string& outputDir = "");
    static CodecResult decompress(const std::string& inputPath, const std::string& outputDir = "");

private:
    struct Model {
        uint32_t freq[257] = {};  // freq[256] = EOF symbol
        uint32_t cumFreq[258] = {};
        uint32_t total = 0;
        void build(const std::vector<uint8_t>& data);
        void buildCum();
    };

    static std::vector<uint8_t> encode(const std::vector<uint8_t>& data, const Model& m);
    static std::vector<uint8_t> decode(const std::vector<uint8_t>& bits,
                                        const Model& m, uint32_t origSize);
};
