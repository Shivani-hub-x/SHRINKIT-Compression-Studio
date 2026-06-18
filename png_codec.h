#pragma once
#include "image_codec.h"
#include <string>

// PNG/DEFLATE — lossless image compression
// Accepts: .bmp .pgm
// Output : .png (our custom deflate-based format)
class PNGCodec {
public:
    static ImageResult compress  (const std::string& inputPath,
                                   const std::string& outputDir = "");
    static ImageResult decompress(const std::string& inputPath,
                                   const std::string& outputDir = "");
};
