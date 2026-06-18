#pragma once
#include "image_codec.h"
#include <string>

// WebP — lossy image compression using block prediction + DCT
// Accepts: .bmp .pgm
// Output : .webp
class WebPCodec {
public:
    static ImageResult compress  (const std::string& inputPath,
                                   int quality = 75,
                                   const std::string& outputDir = "");
    static ImageResult decompress(const std::string& inputPath,
                                   const std::string& outputDir = "");
};
