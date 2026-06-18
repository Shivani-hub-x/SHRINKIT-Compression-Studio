#include "codec.h"
#include "jpeg_codec.h"
#include <cmath>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <algorithm>

// ── DCT helpers ───────────────────────────────────────────────────────────────
static const double PI = 3.14159265358979323846;

// Standard JPEG quantization table (luminance), scaled by quality
static void makeQuantTable(int quality, int qtab[64]) {
    static const int base[64] = {
        16,11,10,16,24,40,51,61,
        12,12,14,19,26,58,60,55,
        14,13,16,24,40,57,69,56,
        14,17,22,29,51,87,80,62,
        18,22,37,56,68,109,103,77,
        24,35,55,64,81,104,113,92,
        49,64,78,87,103,121,120,101,
        72,92,95,98,112,100,103,99
    };
    double scale = quality < 50 ? 5000.0/quality : 200.0 - quality*2.0;
    for (int i = 0; i < 64; i++) {
        int q = (int)((base[i] * scale + 50) / 100);
        qtab[i] = std::max(1, std::min(255, q));
    }
}

// 8x8 DCT (unnormalized)
static void dct8x8(double block[8][8]) {
    double tmp[8][8];
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            double sum = 0;
            for (int x = 0; x < 8; x++)
                for (int y = 0; y < 8; y++)
                    sum += block[x][y]
                        * cos((2*x+1)*u*PI/16.0)
                        * cos((2*y+1)*v*PI/16.0);
            double cu = (u==0) ? 1.0/sqrt(2.0) : 1.0;
            double cv = (v==0) ? 1.0/sqrt(2.0) : 1.0;
            tmp[u][v] = 0.25 * cu * cv * sum;
        }
    }
    for (int i=0;i<8;i++) for(int j=0;j<8;j++) block[i][j]=tmp[i][j];
}

// 8x8 IDCT
static void idct8x8(double block[8][8]) {
    double tmp[8][8];
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            double sum = 0;
            for (int u = 0; u < 8; u++)
                for (int v = 0; v < 8; v++) {
                    double cu = (u==0) ? 1.0/sqrt(2.0) : 1.0;
                    double cv = (v==0) ? 1.0/sqrt(2.0) : 1.0;
                    sum += cu * cv * block[u][v]
                        * cos((2*x+1)*u*PI/16.0)
                        * cos((2*y+1)*v*PI/16.0);
                }
            tmp[x][y] = 0.25 * sum;
        }
    }
    for (int i=0;i<8;i++) for(int j=0;j<8;j++) block[i][j]=tmp[i][j];
}

// File helpers
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

ImageResult JPEGCodec::compress(const std::string& inputPath,
                                 int quality,
                                 const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = extOf(inputPath);
    try {
        Image img;
        uint8_t fmt = 0;
        if (res.inputFormat == ".bmp")      { img = ImageIO::readBMP(inputPath); fmt=0; }
        else if (res.inputFormat == ".pgm") { img = ImageIO::readPGM(inputPath); fmt=1; }
        else throw std::runtime_error("JPEG: only .bmp and .pgm supported");

        res.width  = img.width;
        res.height = img.height;
        res.originalSize = FileUtils::fileSize(inputPath);

        int qtab[64];
        makeQuantTable(quality, qtab);

        // Pad image to multiple of 8
        int pw = ((img.width  + 7) / 8) * 8;
        int ph = ((img.height + 7) / 8) * 8;

        // Compress each channel (R, G, B) using DCT
        // Store as: header + qtab(64 int16) + for each 8x8 block: 64 int16 coefficients
        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        auto push16 = [&](int16_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
        };

        out.push_back('J'); out.push_back('D');
        out.push_back('C'); out.push_back('T');
        push32((uint32_t)img.width);
        push32((uint32_t)img.height);
        out.push_back(fmt);
        out.push_back((uint8_t)quality);

        // Store quantization table
        for (int i = 0; i < 64; i++) push16((int16_t)qtab[i]);

        // Process 8x8 blocks, 3 channels
        for (int ch = 0; ch < 3; ch++) {
            for (int by = 0; by < ph; by += 8) {
                for (int bx = 0; bx < pw; bx += 8) {
                    double block[8][8];
                    for (int y = 0; y < 8; y++) {
                        for (int x = 0; x < 8; x++) {
                            int px = std::min(bx+x, img.width-1);
                            int py = std::min(by+y, img.height-1);
                            auto& p = img.at(px, py);
                            uint8_t val = (ch==0)?p.r:(ch==1)?p.g:p.b;
                            block[y][x] = (double)val - 128.0;
                        }
                    }
                    dct8x8(block);
                    // Quantize and store
                    for (int i = 0; i < 8; i++)
                        for (int j = 0; j < 8; j++) {
                            int q = qtab[i*8+j];
                            int16_t coef = (int16_t)round(block[i][j] / q);
                            push16(coef);
                        }
                }
            }
        }

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.jpg";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".jpg";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

ImageResult JPEGCodec::decompress(const std::string& inputPath,
                                   const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = ".jpg";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data[0]!='J'||data[1]!='D'||data[2]!='C'||data[3]!='T')
            throw std::runtime_error("Not a valid .jpg (DCT) file");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v=data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        auto r16 = [&]() -> int16_t {
            int16_t v=(int16_t)(data[idx]|(data[idx+1]<<8)); idx+=2; return v;
        };

        uint32_t w   = r32();
        uint32_t h   = r32();
        uint8_t  fmt = data[idx++];
        idx++; // skip quality byte

        res.width = w; res.height = h;
        res.originalSize = FileUtils::fileSize(inputPath);

        int qtab[64];
        for (int i = 0; i < 64; i++) qtab[i] = r16();

        Image out;
        out.width  = w; out.height = h;
        out.pixels.resize(w * h, {128,128,128});

        int pw = ((w+7)/8)*8;
        int ph = ((h+7)/8)*8;

        // Reconstruct each channel
        //uint8_t ch_data[3][1<<20]; // max ~1MB per channel temp — use vector in prod
        std::vector<std::vector<uint8_t>> channels(3, std::vector<uint8_t>(w*h, 128));

        for (int ch = 0; ch < 3; ch++) {
            for (int by = 0; by < (int)ph; by += 8) {
                for (int bx = 0; bx < (int)pw; bx += 8) {
                    double block[8][8];
                    for (int i = 0; i < 8; i++)
                        for (int j = 0; j < 8; j++) {
                            int16_t coef = r16();
                            block[i][j] = (double)coef * qtab[i*8+j];
                        }
                    idct8x8(block);
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++) {
                            int px = bx+x, py = by+y;
                            if (px < (int)w && py < (int)h) {
                                int v = (int)round(block[y][x] + 128.0);
                                v = std::max(0, std::min(255, v));
                                channels[ch][py*w+px] = (uint8_t)v;
                            }
                        }
                }
            }
        }

        for (int i = 0; i < (int)(w*h); i++)
            out.pixels[i] = {channels[0][i], channels[1][i], channels[2][i]};

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string ext = (fmt==1) ? ".pgm" : ".bmp";
        std::string outPath = dir + "/" + baseOf(inputPath) + "_decompressed" + ext;
        bool ok = (fmt==1) ? ImageIO::writePGM(outPath, out)
                           : ImageIO::writeBMP(outPath, out);
        if (!ok) throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = FileUtils::fileSize(outPath);
        res.outputFormat = ext;
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}
