#pragma once
#include "image_codec.h"
#include <string>

// JPEG2000/Wavelet — lossy/lossless image compression using Haar wavelet
// Accepts: .bmp .pgm
// Output : .j2k
class J2KCodec {
public:
    static ImageResult compress  (const std::string& inputPath,
                                   int levels = 4,
                                   const std::string& outputDir = "");
    static ImageResult decompress(const std::string& inputPath,
                                   const std::string& outputDir = "");
};
