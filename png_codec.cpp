#include "png_codec.h"
#include "rle.h"      // reuse RLE for DEFLATE-like compression
#include "lzw.h"      // reuse LZW for entropy coding
#include <stdexcept>
#include <algorithm>

// PNG/DEFLATE: apply row filter (Sub filter) then LZW entropy coding
// Sub filter: each byte = byte - left_byte  (decorrelates pixels)

static std::string extOf(const std::string& p) {
    auto pos=p.rfind('.'); if(pos==std::string::npos) return "";
    std::string e=p.substr(pos); for(auto& c:e) c=(char)tolower(c); return e;
}
static std::string baseOf(const std::string& p) {
    std::string s=p; for(auto& c:s) if(c=='\\') c='/';
    auto sl=s.rfind('/'); auto dot=s.rfind('.');
    size_t start=(sl==std::string::npos)?0:sl+1;
    size_t len=(dot==std::string::npos||dot<start)?std::string::npos:dot-start;
    return s.substr(start,len);
}
static std::string dirOf(const std::string& p) {
    std::string s=p; for(auto& c:s) if(c=='\\') c='/';
    auto sl=s.rfind('/'); return (sl==std::string::npos)?".":s.substr(0,sl);
}

// Apply PNG Sub filter row by row (reduces entropy before compression)
static std::vector<uint8_t> applyFilter(const Image& img) {
    std::vector<uint8_t> out;
    int stride = img.width * 3;
    out.reserve(img.height * (stride + 1));
    for (int y = 0; y < img.height; y++) {
        out.push_back(1); // filter type = Sub
        for (int x = 0; x < img.width; x++) {
            auto& p = img.at(x, y);
            uint8_t r = p.r, g = p.g, b = p.b;
            uint8_t lr = (x > 0) ? img.at(x-1,y).r : 0;
            uint8_t lg = (x > 0) ? img.at(x-1,y).g : 0;
            uint8_t lb = (x > 0) ? img.at(x-1,y).b : 0;
            out.push_back((uint8_t)(r - lr));
            out.push_back((uint8_t)(g - lg));
            out.push_back((uint8_t)(b - lb));
        }
    }
    return out;
}

static Image removeFilter(const std::vector<uint8_t>& filtered, int w, int h) {
    Image img; img.width=w; img.height=h; img.pixels.resize(w*h);
    int stride = w * 3 + 1;
    for (int y = 0; y < h; y++) {
        // filter type byte ignored (we only use Sub)
        for (int x = 0; x < w; x++) {
            int base = y * stride + 1 + x * 3;
            uint8_t r = filtered[base],   g = filtered[base+1], b = filtered[base+2];
            uint8_t lr = (x>0)?img.at(x-1,y).r:0;
            uint8_t lg = (x>0)?img.at(x-1,y).g:0;
            uint8_t lb = (x>0)?img.at(x-1,y).b:0;
            img.at(x,y) = {(uint8_t)(r+lr),(uint8_t)(g+lg),(uint8_t)(b+lb)};
        }
    }
    return img;
}

ImageResult PNGCodec::compress(const std::string& inputPath,
                                 const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = extOf(inputPath);
    try {
        Image img;
        uint8_t fmt = 0;
        if (res.inputFormat == ".bmp")      { img = ImageIO::readBMP(inputPath); fmt=0; }
        else if (res.inputFormat == ".pgm") { img = ImageIO::readPGM(inputPath); fmt=1; }
        else throw std::runtime_error("PNG: only .bmp and .pgm supported");

        res.width  = img.width;
        res.height = img.height;
        res.originalSize = FileUtils::fileSize(inputPath);

        // Apply filter then LZW compress
        auto filtered = applyFilter(img);
        auto codes    = LZW::compress(filtered);
        auto packed   = LZW::packCodes(codes);

        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        out.push_back('P'); out.push_back('N');
        out.push_back('G'); out.push_back('!');
        push32((uint32_t)img.width);
        push32((uint32_t)img.height);
        out.push_back(fmt);
        push32((uint32_t)filtered.size());
        push32((uint32_t)codes.size());
        out.insert(out.end(), packed.begin(), packed.end());

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.png";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".png";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

ImageResult PNGCodec::decompress(const std::string& inputPath,
                                   const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = ".png";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data[0]!='P'||data[1]!='N'||data[2]!='G'||data[3]!='!')
            throw std::runtime_error("Not a valid .png file from this tool");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v=data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        uint32_t w         = r32();
        uint32_t h         = r32();
        uint8_t  fmt       = data[idx++];
        uint32_t filtSz    = r32();
        uint32_t numCodes  = r32();

        res.width = w; res.height = h;
        res.originalSize = FileUtils::fileSize(inputPath);

        std::vector<uint8_t> packed(data.begin()+idx, data.end());
        auto codes    = LZW::unpackCodes(packed);
        codes.resize(numCodes);
        auto filtered = LZW::decompress(codes);
        filtered.resize(filtSz);

        Image img = removeFilter(filtered, w, h);

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string ext = (fmt==1) ? ".pgm" : ".bmp";
        std::string outPath = dir + "/" + baseOf(inputPath) + "_decompressed" + ext;
        bool ok = (fmt==1) ? ImageIO::writePGM(outPath, img)
                           : ImageIO::writeBMP(outPath, img);
        if (!ok) throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = FileUtils::fileSize(outPath);
        res.outputFormat = ext;
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}
