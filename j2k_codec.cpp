#include "codec.h"
#include "j2k_codec.h"
#include <cmath>
#include <vector>
#include <stdexcept>
#include <algorithm>

// ── Haar Wavelet Transform ────────────────────────────────────────────────────
static void haarFwd(std::vector<float>& data, int n) {
    std::vector<float> tmp(n);
    while (n > 1) {
        int h = n / 2;
        for (int i = 0; i < h; i++) {
            tmp[i]   = (data[2*i] + data[2*i+1]) * 0.5f;
            tmp[h+i] = (data[2*i] - data[2*i+1]) * 0.5f;
        }
        for (int i = 0; i < n; i++) data[i] = tmp[i];
        n = h;
    }
}

static void haarInv(std::vector<float>& data, int n) {
    int step = 2;
    while (step <= n) {
        int h = step / 2;
        std::vector<float> tmp(step);
        for (int i = 0; i < h; i++) {
            tmp[2*i]   = data[i] + data[h+i];
            tmp[2*i+1] = data[i] - data[h+i];
        }
        for (int i = 0; i < step; i++) data[i] = tmp[i];
        step *= 2;
    }
}

// 2D Haar transform on a single channel
static void haar2DFwd(std::vector<float>& ch, int w, int h, int levels) {
    for (int lv = 0; lv < levels; lv++) {
        int cw = w >> lv, ch2 = h >> lv;
        if (cw < 2 || ch2 < 2) break;
        // Rows
        for (int y = 0; y < ch2; y++) {
            std::vector<float> row(cw);
            for (int x = 0; x < cw; x++) row[x] = ch[y*w+x];
            haarFwd(row, cw);
            for (int x = 0; x < cw; x++) ch[y*w+x] = row[x];
        }
        // Cols
        for (int x = 0; x < cw; x++) {
            std::vector<float> col(ch2);
            for (int y = 0; y < ch2; y++) col[y] = ch[y*w+x];
            haarFwd(col, ch2);
            for (int y = 0; y < ch2; y++) ch[y*w+x] = col[y];
        }
    }
}

static void haar2DInv(std::vector<float>& ch, int w, int h, int levels) {
    for (int lv = levels-1; lv >= 0; lv--) {
        int cw = w >> lv, ch2 = h >> lv;
        if (cw < 2 || ch2 < 2) continue;
        for (int x = 0; x < cw; x++) {
            std::vector<float> col(ch2);
            for (int y = 0; y < ch2; y++) col[y] = ch[y*w+x];
            haarInv(col, ch2);
            for (int y = 0; y < ch2; y++) ch[y*w+x] = col[y];
        }
        for (int y = 0; y < ch2; y++) {
            std::vector<float> row(cw);
            for (int x = 0; x < cw; x++) row[x] = ch[y*w+x];
            haarInv(row, cw);
            for (int x = 0; x < cw; x++) ch[y*w+x] = row[x];
        }
    }
}

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

ImageResult J2KCodec::compress(const std::string& inputPath, int levels,
                                 const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = extOf(inputPath);
    try {
        Image img;
        uint8_t fmt = 0;
        if (res.inputFormat == ".bmp")      { img = ImageIO::readBMP(inputPath); fmt=0; }
        else if (res.inputFormat == ".pgm") { img = ImageIO::readPGM(inputPath); fmt=1; }
        else throw std::runtime_error("J2K: only .bmp and .pgm supported");

        res.width  = img.width;
        res.height = img.height;
        res.originalSize = FileUtils::fileSize(inputPath);

        int n = img.width * img.height;
        std::vector<float> chR(n), chG(n), chB(n);
        for (int i = 0; i < n; i++) {
            chR[i] = img.pixels[i].r;
            chG[i] = img.pixels[i].g;
            chB[i] = img.pixels[i].b;
        }

        haar2DFwd(chR, img.width, img.height, levels);
        haar2DFwd(chG, img.width, img.height, levels);
        haar2DFwd(chB, img.width, img.height, levels);

        // Quantize: store as int16 (lossy step)
        float threshold = 2.0f; // small threshold for compression
        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        auto push16s = [&](int16_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
        };
        out.push_back('J'); out.push_back('2');
        out.push_back('K'); out.push_back('!');
        push32((uint32_t)img.width);
        push32((uint32_t)img.height);
        out.push_back(fmt);
        out.push_back((uint8_t)levels);

        auto storeChannel = [&](std::vector<float>& ch) {
            for (float v : ch) {
                if (std::abs(v) < threshold) v = 0;
                push16s((int16_t)std::max(-32768.0f, std::min(32767.0f, (float)round(v))));
            }
        };
        storeChannel(chR);
        storeChannel(chG);
        storeChannel(chB);

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.j2k";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".j2k";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

ImageResult J2KCodec::decompress(const std::string& inputPath,
                                   const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = ".j2k";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data[0]!='J'||data[1]!='2'||data[2]!='K'||data[3]!='!')
            throw std::runtime_error("Not a valid .j2k file");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v=data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        auto r16s = [&]() -> int16_t {
            int16_t v=(int16_t)(data[idx]|(data[idx+1]<<8)); idx+=2; return v;
        };

        uint32_t w      = r32();
        uint32_t h      = r32();
        uint8_t  fmt    = data[idx++];
        int      levels = data[idx++];

        res.width = w; res.height = h;
        res.originalSize = FileUtils::fileSize(inputPath);

        int n = w * h;
        std::vector<float> chR(n), chG(n), chB(n);
        for (int i = 0; i < n; i++) chR[i] = r16s();
        for (int i = 0; i < n; i++) chG[i] = r16s();
        for (int i = 0; i < n; i++) chB[i] = r16s();

        haar2DInv(chR, w, h, levels);
        haar2DInv(chG, w, h, levels);
        haar2DInv(chB, w, h, levels);

        Image out; out.width=w; out.height=h; out.pixels.resize(n);
        for (int i = 0; i < n; i++) {
            auto clamp = [](float v) -> uint8_t {
                return (uint8_t)std::max(0.0f, std::min(255.0f, (float)round(v)));
            };
            out.pixels[i] = {clamp(chR[i]), clamp(chG[i]), clamp(chB[i])};
        }

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
