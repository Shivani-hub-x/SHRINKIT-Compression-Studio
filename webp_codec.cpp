#include "codec.h"
#include "webp_codec.h"
#include <cmath>
#include <vector>
#include <stdexcept>
#include <algorithm>

// WebP-style: 4x4 block intra prediction + DCT quantization
// Prediction modes: DC, Horizontal, Vertical, TrueMotion

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

// Simple 4x4 DCT
static void dct4(double b[4][4]) {
    const double PI = 3.14159265358979;
    double t[4][4];
    for (int u=0;u<4;u++) for (int v=0;v<4;v++) {
        double s=0;
        for (int x=0;x<4;x++) for (int y=0;y<4;y++)
            s += b[x][y]*cos((2*x+1)*u*PI/8.0)*cos((2*y+1)*v*PI/8.0);
        double cu=(u==0)?1.0/sqrt(2.0):1.0;
        double cv=(v==0)?1.0/sqrt(2.0):1.0;
        t[u][v]=0.25*cu*cv*s;
    }
    for (int i=0;i<4;i++) for(int j=0;j<4;j++) b[i][j]=t[i][j];
}

static void idct4(double b[4][4]) {
    const double PI = 3.14159265358979;
    double t[4][4];
    for (int x=0;x<4;x++) for (int y=0;y<4;y++) {
        double s=0;
        for (int u=0;u<4;u++) for (int v=0;v<4;v++) {
            double cu=(u==0)?1.0/sqrt(2.0):1.0;
            double cv=(v==0)?1.0/sqrt(2.0):1.0;
            s+=cu*cv*b[u][v]*cos((2*x+1)*u*PI/8.0)*cos((2*y+1)*v*PI/8.0);
        }
        t[x][y]=0.25*s;
    }
    for (int i=0;i<4;i++) for(int j=0;j<4;j++) b[i][j]=t[i][j];
}

ImageResult WebPCodec::compress(const std::string& inputPath, int quality,
                                  const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = extOf(inputPath);
    try {
        Image img;
        uint8_t fmt = 0;
        if (res.inputFormat == ".bmp")      { img = ImageIO::readBMP(inputPath); fmt=0; }
        else if (res.inputFormat == ".pgm") { img = ImageIO::readPGM(inputPath); fmt=1; }
        else throw std::runtime_error("WebP: only .bmp and .pgm supported");

        res.width  = img.width;
        res.height = img.height;
        res.originalSize = FileUtils::fileSize(inputPath);

        // Quantization step size based on quality
        double qstep = std::max(1.0, (100 - quality) * 0.5);

        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        auto push16s = [&](int16_t v){
            out.push_back((uint8_t)(v&0xFF)); out.push_back((uint8_t)((v>>8)&0xFF));
        };

        out.push_back('W'); out.push_back('E');
        out.push_back('B'); out.push_back('P');
        push32((uint32_t)img.width);
        push32((uint32_t)img.height);
        out.push_back(fmt);
        out.push_back((uint8_t)quality);

        // Process 4x4 blocks per channel
        for (int ch = 0; ch < 3; ch++) {
            for (int by = 0; by < img.height; by += 4) {
                for (int bx = 0; bx < img.width; bx += 4) {
                    // DC prediction: use average of left/top border
                    double pred = 128.0;
                    int cnt = 0;
                    if (bx > 0) { for (int y=0;y<4;y++) { int py=std::min(by+y,img.height-1); auto& p=img.at(bx-1,py); pred+=(ch==0?p.r:ch==1?p.g:p.b); cnt++; } }
                    if (by > 0) { for (int x=0;x<4;x++) { int px=std::min(bx+x,img.width-1);  auto& p=img.at(px,by-1); pred+=(ch==0?p.r:ch==1?p.g:p.b); cnt++; } }
                    if (cnt) pred /= cnt;

                    double block[4][4];
                    for (int y=0;y<4;y++) for (int x=0;x<4;x++) {
                        int px=std::min(bx+x,img.width-1);
                        int py=std::min(by+y,img.height-1);
                        auto& p=img.at(px,py);
                        uint8_t v=(ch==0)?p.r:(ch==1)?p.g:p.b;
                        block[y][x]=(double)v - pred;
                    }
                    dct4(block);
                    for (int i=0;i<4;i++) for(int j=0;j<4;j++) {
                        int16_t coef=(int16_t)round(block[i][j]/qstep);
                        push16s(coef);
                    }
                    // Store predictor
                    push16s((int16_t)round(pred));
                }
            }
        }

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.webp";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".webp";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

ImageResult WebPCodec::decompress(const std::string& inputPath,
                                   const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = ".webp";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data[0]!='W'||data[1]!='E'||data[2]!='B'||data[3]!='P')
            throw std::runtime_error("Not a valid .webp file");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v=data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        auto r16s = [&]() -> int16_t {
            int16_t v=(int16_t)(data[idx]|(data[idx+1]<<8)); idx+=2; return v;
        };

        uint32_t w   = r32();
        uint32_t h   = r32();
        uint8_t  fmt = data[idx++];
        int      q   = data[idx++];
        double qstep = std::max(1.0, (100-q)*0.5);

        res.width = w; res.height = h;
        res.originalSize = FileUtils::fileSize(inputPath);

        Image out; out.width=w; out.height=h;
        out.pixels.resize(w*h, {128,128,128});

        std::vector<std::vector<uint8_t>> channels(3, std::vector<uint8_t>(w*h, 128));

        for (int ch = 0; ch < 3; ch++) {
            for (int by = 0; by < (int)h; by += 4) {
                for (int bx = 0; bx < (int)w; bx += 4) {
                    double block[4][4];
                    for (int i=0;i<4;i++) for(int j=0;j<4;j++)
                        block[i][j] = r16s() * qstep;
                    double pred = r16s();

                    idct4(block);
                    for (int y=0;y<4;y++) for (int x=0;x<4;x++) {
                        int px=bx+x, py=by+y;
                        if (px<(int)w && py<(int)h) {
                            int v=(int)round(block[y][x]+pred);
                            v=std::max(0,std::min(255,v));
                            channels[ch][py*w+px]=(uint8_t)v;
                        }
                    }
                }
            }
        }

        for (int i=0;i<(int)(w*h);i++)
            out.pixels[i]={channels[0][i],channels[1][i],channels[2][i]};

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
