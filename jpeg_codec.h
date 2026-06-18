#pragma once
#include "image_codec.h"
#include <string>

// JPEG/DCT — lossy image compression
// Accepts: .bmp .pgm
// Output : .jpg  (our custom DCT-based format, viewable after decode)
// Quality: 0 (worst) to 100 (best), default 75
class JPEGCodec {
public:
    static ImageResult compress  (const std::string& inputPath,
                                   int quality = 75,
                                   const std::string& outputDir = "");
    static ImageResult decompress(const std::string& inputPath,
                                   const std::string& outputDir = "");
};
