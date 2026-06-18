#include "image_codec.h"
#include "quadtree.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
using namespace std;
// ─────────────────────────────────────────────────────────────────────────────
// File helpers
// ─────────────────────────────────────────────────────────────────────────────
static string extOf(const std::string& p) {
    auto pos = p.rfind('.');
    if (pos == string::npos) return "";
    std::string e = p.substr(pos);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}
static string baseOf(const std::string& p) {
    std::string s = p;
    for (auto& c : s) if (c=='\\') c='/';
    auto sl  = s.rfind('/');
    auto dot = s.rfind('.');
    size_t start = (sl==std::string::npos)?0:sl+1;
    size_t len   = (dot==std::string::npos||dot<start)?std::string::npos:dot-start;
    return s.substr(start, len);
}
static std::string dirOf(const std::string& p) {
    std::string s = p;
    for (auto& c : s) if (c=='\\') c='/';
    auto sl = s.rfind('/');
    return (sl==std::string::npos)?".":s.substr(0,sl);
}
static uint64_t fileSz(const std::string& p) {
    std::ifstream f(p, std::ios::binary|std::ios::ate);
    return f ? (uint64_t)f.tellg() : 0;
}
/*static std::string fmtSz(uint64_t b) {
    char buf[64];
    if      (b<1024)           snprintf(buf,64,"%llu B",(unsigned long long)b);
    else if (b<1024*1024)      snprintf(buf,64,"%.2f KB",b/1024.0);
    else                       snprintf(buf,64,"%.2f MB",b/(1024.0*1024));
    return buf;
}*/

// ─────────────────────────────────────────────────────────────────────────────
// BMP reader/writer  (24-bit only)
// ─────────────────────────────────────────────────────────────────────────────
Image ImageIO::readBMP(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open BMP: " + path);

    // File header
    char sig[2]; f.read(sig,2);
    if (sig[0]!='B'||sig[1]!='M') throw std::runtime_error("Not a BMP file");

    auto r32 = [&]() { uint32_t v=0; f.read((char*)&v,4); return v; };
    auto r16 = [&]() { uint16_t v=0; f.read((char*)&v,2); return v; };

    r32(); r32(); // file size, reserved
    uint32_t dataOffset = r32();

    // DIB header
    r32(); // header size
    int w = (int)r32();
    int h = (int)r32();
    bool flipped = h > 0; // positive = bottom-up
    if (h < 0) h = -h;
    r16(); // planes
    uint16_t bpp = r16();
    if (bpp != 24) throw std::runtime_error("Only 24-bit BMP supported");

    // Seek to pixel data
    f.seekg(dataOffset);

    Image img;
    img.width  = w;
    img.height = h;
    img.pixels.resize(w * h);

    int rowSize = (w * 3 + 3) & ~3;
    std::vector<uint8_t> row(rowSize);

    for (int y = 0; y < h; y++) {
        f.read((char*)row.data(), rowSize);
        int dstY = flipped ? (h-1-y) : y;
        for (int x = 0; x < w; x++) {
            img.at(x, dstY) = { row[x*3+2], row[x*3+1], row[x*3+0] }; // BGR→RGB
        }
    }
    return img;
}

bool ImageIO::writeBMP(const std::string& path, const Image& img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    int w = img.width, h = img.height;
    int rowSize = (w * 3 + 3) & ~3;
    uint32_t dataSize = rowSize * h;
    uint32_t fileSize = 54 + dataSize;

    auto w32 = [&](uint32_t v){ f.write((char*)&v,4); };
    auto w16 = [&](uint16_t v){ f.write((char*)&v,2); };

    f.write("BM",2);
    w32(fileSize); w32(0); w32(54);     // file header
    w32(40); w32(w); w32((uint32_t)h);  // DIB header (positive h = bottom-up)
    w16(1); w16(24);                     // planes, bpp
    w32(0); w32(dataSize);
    w32(2835); w32(2835);
    w32(0); w32(0);

    std::vector<uint8_t> row(rowSize, 0);
    for (int y = h-1; y >= 0; y--) {   // bottom-up
        for (int x = 0; x < w; x++) {
            const auto& p = img.at(x, y);
            row[x*3+0] = p.b;
            row[x*3+1] = p.g;
            row[x*3+2] = p.r;
        }
        f.write((char*)row.data(), rowSize);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// PGM reader/writer  (P5 binary)
// ─────────────────────────────────────────────────────────────────────────────
Image ImageIO::readPGM(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open PGM: " + path);
    std::string magic; f >> magic;
    if (magic != "P5") throw std::runtime_error("Only P5 (binary) PGM supported");
    int w, h, maxval; f >> w >> h >> maxval;
    f.ignore(1); // skip single whitespace after header
    Image img; img.width=w; img.height=h; img.pixels.resize(w*h);
    for (int i = 0; i < w*h; i++) {
        uint8_t v = (uint8_t)f.get();
        img.pixels[i] = {v,v,v};
    }
    return img;
}

bool ImageIO::writePGM(const std::string& path, const Image& img) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P5\n" << img.width << " " << img.height << "\n255\n";
    for (auto& p : img.pixels) f.put((char)p.r); // store as grey using R channel
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// .qtc file format:
//   [4]  magic  "QTRC"
//   [4]  width  (uint32 LE)
//   [4]  height (uint32 LE)
//   [1]  format (0=BMP, 1=PGM)
//   [N]  serialised quadtree
// ─────────────────────────────────────────────────────────────────────────────
bool ImageCodec::isQtcFile(const std::string& path) {
    if (extOf(path) != ".qtc") return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char sig[4]; f.read(sig,4);
    return memcmp(sig,"QTRC",4)==0;
}

ImageResult ImageCodec::compress(const std::string& inputPath,
                                  const std::string& outputDir) {
    ImageResult res;
    res.inputFormat = extOf(inputPath);
    res.originalSize = fileSz(inputPath);

    try {
        // Load image
        Image img;
        uint8_t fmt = 0;
        if (res.inputFormat == ".bmp") { img = ImageIO::readBMP(inputPath); fmt = 0; }
        else if (res.inputFormat == ".pgm") { img = ImageIO::readPGM(inputPath); fmt = 1; }
        else throw std::runtime_error("Unsupported format. Use .bmp or .pgm");

        res.width  = img.width;
        res.height = img.height;

        // Build quadtree
        QTNode* root = QuadTree::build(img, 0, 0, img.width, img.height);

        // Serialise tree
        auto treeData = QuadTree::serialise(root);
        QuadTree::freeTree(root);

        // Build output file
        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        out.push_back('Q'); out.push_back('T');
        out.push_back('R'); out.push_back('C');
        push32((uint32_t)img.width);
        push32((uint32_t)img.height);
        out.push_back(fmt);
        out.insert(out.end(), treeData.begin(), treeData.end());

        // Write file
        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.qtc";
        std::ofstream f(outPath, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot write: " + outPath);
        f.write((char*)out.data(), out.size());

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".qtc";
        res.success      = true;

    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

ImageResult ImageCodec::decompress(const std::string& inputPath,
                                    const std::string& outputDir) {
    ImageResult res;
    res.inputFormat  = ".qtc";
    res.originalSize = fileSz(inputPath);

    try {
        std::ifstream f(inputPath, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open: " + inputPath);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});

        if (data.size() < 13) throw std::runtime_error("File too small");
        if (memcmp(data.data(),"QTRC",4)!=0)
            throw std::runtime_error("Not a valid .qtc file");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v = data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        uint32_t w   = r32();
        uint32_t h   = r32();
        uint8_t  fmt = data[idx++];

        res.width  = w;
        res.height = h;

        // Deserialise tree
        std::vector<uint8_t> treeData(data.begin()+idx, data.end());
        size_t tIdx = 0;
        QTNode* root = QuadTree::deserialise(treeData, tIdx, 0, 0, w, h);

        // Render to image
        Image out;
        out.width  = w;
        out.height = h;
        out.pixels.resize(w * h, {0,0,0});
        QuadTree::render({}, root, out);
        QuadTree::freeTree(root);

        // Write output
        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string ext = (fmt==1) ? ".pgm" : ".bmp";
        std::string outPath = dir + "/" + baseOf(inputPath) + "_decompressed" + ext;

        bool ok = (fmt==1) ? ImageIO::writePGM(outPath, out)
                           : ImageIO::writeBMP(outPath, out);
        if (!ok) throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = fileSz(outPath);
        res.outputFormat = ext;
        res.success      = true;

    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}
