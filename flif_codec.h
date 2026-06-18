#pragma once
#include "image_codec.h"
#include <string>

// FLIF — lossless image compression using MANIAC prediction + arithmetic coding
// Uses median-edge-detection prediction to decorrelate pixel data,
// then applies arithmetic coding for near-entropy-optimal compression.
// Accepts: .bmp .pgm
// Output : .flif
class FLIFCodec {
public:
    static ImageResult compress  (const std::string& inputPath,
                                   const std::string& outputDir = "");
    static ImageResult decompress(const std::string& inputPath,
                                   const std::string& outputDir = "");
};
