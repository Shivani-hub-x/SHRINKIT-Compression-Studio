#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "codec.h"

// Brotli-style compression: LZ77 back-references + Huffman entropy coding
// Works on: any file — best on text, HTML, CSS, JS
// Output: .bro
class BrotliCodec {
public:
    static CodecResult compress  (const std::string& inputPath, const std::string& outputDir = "");
    static CodecResult decompress(const std::string& inputPath, const std::string& outputDir = "");

private:
    struct Token {
        bool    isLiteral;
        uint8_t literal;
        int     offset;   // back-reference offset
        int     length;   // back-reference length
    };

    static const int WIN_SIZE  = 32768;  // LZ77 window
    static const int MIN_MATCH = 3;

    static std::vector<Token>   lz77Encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> lz77Decode(const std::vector<Token>& tokens,
                                             uint32_t origSize);
    static std::vector<uint8_t> packTokens  (const std::vector<Token>& tokens);
    static std::vector<Token>   unpackTokens(const std::vector<uint8_t>& packed,
                                              uint32_t origSize);
};
