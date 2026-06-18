#include "flif_codec.h"
#include "codec.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <cstdint>
using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// File helpers (same pattern as other codecs)
// ─────────────────────────────────────────────────────────────────────────────
static string extOf(const string& p) {
    auto pos = p.rfind('.'); if (pos == string::npos) return "";
    string e = p.substr(pos); for (auto& c : e) c = (char)tolower(c); return e;
}
static string baseOf(const string& p) {
    string s = p; for (auto& c : s) if (c == '\\') c = '/';
    auto sl = s.rfind('/'); auto dot = s.rfind('.');
    size_t start = (sl == string::npos) ? 0 : sl + 1;
    size_t len = (dot == string::npos || dot < start) ? string::npos : dot - start;
    return s.substr(start, len);
}
static string dirOf(const string& p) {
    string s = p; for (auto& c : s) if (c == '\\') c = '/';
    auto sl = s.rfind('/'); return (sl == string::npos) ? "." : s.substr(0, sl);
}

// ─────────────────────────────────────────────────────────────────────────────
// MANIAC-style Median Edge Detection (MED) Predictor
// This is the core prediction used by FLIF to decorrelate image data.
// For each pixel, predict based on Left (A), Above (B), and Above-Left (C):
//   if C >= max(A,B): predict min(A,B)     — vertical edge detected
//   if C <= min(A,B): predict max(A,B)     — horizontal edge detected
//   else:             predict A + B - C    — no strong edge, planar predict
//
// The residual (actual - predicted) has much lower entropy than raw pixels,
// making arithmetic coding far more effective.
// ─────────────────────────────────────────────────────────────────────────────

static inline uint8_t medPredict(uint8_t a, uint8_t b, uint8_t c) {
    // MED: Median Edge Detection
    if (c >= max(a, b)) return min(a, b);
    if (c <= min(a, b)) return max(a, b);
    return (uint8_t)((int)a + (int)b - (int)c);
}

// Apply MED prediction, convert image to residuals (signed, stored as uint8_t)
// Each pixel residual = actual - predicted, wrapped to [0..255]
static vector<uint8_t> applyMEDFilter(const Image& img) {
    vector<uint8_t> out;
    out.reserve(img.width * img.height * 3);

    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            auto& px = img.at(x, y);
            uint8_t channels[3] = { px.r, px.g, px.b };

            for (int ch = 0; ch < 3; ch++) {
                uint8_t a = (x > 0) ? ((ch == 0) ? img.at(x-1, y).r :
                                        (ch == 1) ? img.at(x-1, y).g : img.at(x-1, y).b) : 0;
                uint8_t b = (y > 0) ? ((ch == 0) ? img.at(x, y-1).r :
                                        (ch == 1) ? img.at(x, y-1).g : img.at(x, y-1).b) : 0;
                uint8_t c = (x > 0 && y > 0) ? ((ch == 0) ? img.at(x-1, y-1).r :
                                                  (ch == 1) ? img.at(x-1, y-1).g : img.at(x-1, y-1).b) : 0;
                uint8_t pred = medPredict(a, b, c);
                out.push_back((uint8_t)(channels[ch] - pred)); // wraps modulo 256
            }
        }
    }
    return out;
}

// Remove MED prediction to reconstruct image from residuals
static Image removeMEDFilter(const vector<uint8_t>& residuals, int w, int h) {
    Image img;
    img.width = w; img.height = h;
    img.pixels.resize(w * h);

    int idx = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t vals[3];
            for (int ch = 0; ch < 3; ch++) {
                uint8_t a = (x > 0) ? ((ch == 0) ? img.at(x-1, y).r :
                                        (ch == 1) ? img.at(x-1, y).g : img.at(x-1, y).b) : 0;
                uint8_t b = (y > 0) ? ((ch == 0) ? img.at(x, y-1).r :
                                        (ch == 1) ? img.at(x, y-1).g : img.at(x, y-1).b) : 0;
                uint8_t c = (x > 0 && y > 0) ? ((ch == 0) ? img.at(x-1, y-1).r :
                                                  (ch == 1) ? img.at(x-1, y-1).g : img.at(x-1, y-1).b) : 0;
                uint8_t pred = medPredict(a, b, c);
                vals[ch] = (uint8_t)(residuals[idx++] + pred); // wraps modulo 256
            }
            img.at(x, y) = { vals[0], vals[1], vals[2] };
        }
    }
    return img;
}

// ─────────────────────────────────────────────────────────────────────────────
// Arithmetic Encoder/Decoder (self-contained for FLIF)
// 32-bit integer arithmetic coding with static frequency model
// ─────────────────────────────────────────────────────────────────────────────

static const uint32_t AR_TOP  = 0xFFFFFFFF;
static const uint32_t AR_HALF = 0x80000000;
static const uint32_t AR_QTR  = 0x40000000;

struct ArithModel {
    uint32_t freq[257]    = {};
    uint32_t cumFreq[258] = {};
    uint32_t total        = 0;

    void build(const vector<uint8_t>& data) {
        memset(freq, 0, sizeof(freq));
        for (uint8_t b : data) freq[b]++;
        freq[256] = 1; // EOF symbol
        // Scale down if total would overflow
        uint64_t sum = 0;
        for (int i = 0; i <= 256; i++) sum += freq[i];
        if (sum > (1 << 24)) {
            for (int i = 0; i <= 256; i++)
                freq[i] = (freq[i] >> 4) + 1;
        }
        buildCum();
    }

    void buildCum() {
        cumFreq[0] = 0;
        for (int i = 0; i <= 256; i++)
            cumFreq[i + 1] = cumFreq[i] + (freq[i] ? freq[i] : 1);
        total = cumFreq[257];
    }
};

static vector<uint8_t> arithEncode(const vector<uint8_t>& data, const ArithModel& m) {
    vector<uint8_t> outBits;
    uint64_t low = 0, high = AR_TOP;
    int pending = 0;

    auto emitBit = [&](int bit) {
        outBits.push_back((uint8_t)bit);
        for (int i = 0; i < pending; i++)
            outBits.push_back((uint8_t)(1 - bit));
        pending = 0;
    };

    auto encodeSymbol = [&](int sym) {
        uint64_t range = high - low + 1;
        high = low + (range * m.cumFreq[sym + 1]) / m.total - 1;
        low  = low + (range * m.cumFreq[sym])     / m.total;
        for (;;) {
            if (high < AR_HALF) {
                emitBit(0);
                low  = low  * 2;
                high = high * 2 + 1;
            } else if (low >= AR_HALF) {
                emitBit(1);
                low  = (low  - AR_HALF) * 2;
                high = (high - AR_HALF) * 2 + 1;
            } else if (low >= AR_QTR && high < 3 * AR_QTR) {
                pending++;
                low  = (low  - AR_QTR) * 2;
                high = (high - AR_QTR) * 2 + 1;
            } else break;
        }
    };

    for (uint8_t b : data) encodeSymbol(b);
    encodeSymbol(256); // EOF
    pending++;
    if (low < AR_QTR) emitBit(0); else emitBit(1);

    // Pack bits into bytes
    vector<uint8_t> out;
    uint8_t cur = 0; int cnt = 0;
    for (uint8_t bit : outBits) {
        cur = (cur << 1) | bit;
        if (++cnt == 8) { out.push_back(cur); cur = 0; cnt = 0; }
    }
    if (cnt) out.push_back((uint8_t)(cur << (8 - cnt)));
    return out;
}

static vector<uint8_t> arithDecode(const vector<uint8_t>& packed,
                                    const ArithModel& m, uint32_t origSize) {
    // Unpack bits
    vector<uint8_t> bits;
    bits.reserve(packed.size() * 8);
    for (uint8_t byte : packed)
        for (int j = 7; j >= 0; j--)
            bits.push_back((byte >> j) & 1);

    size_t bitIdx = 0;
    auto readBit = [&]() -> uint64_t {
        return (bitIdx < bits.size()) ? bits[bitIdx++] : 0;
    };

    uint64_t low = 0, high = AR_TOP, value = 0;
    for (int i = 0; i < 32; i++) value = (value << 1) | readBit();

    vector<uint8_t> out;
    out.reserve(origSize);

    for (;;) {
        uint64_t range  = high - low + 1;
        uint64_t scaled = ((value - low + 1) * m.total - 1) / range;

        int sym = 0;
        for (int i = 0; i <= 256; i++) {
            if (m.cumFreq[i + 1] > scaled) { sym = i; break; }
        }
        if (sym == 256) break;
        out.push_back((uint8_t)sym);
        if (out.size() >= origSize) break;

        high = low + (range * m.cumFreq[sym + 1]) / m.total - 1;
        low  = low + (range * m.cumFreq[sym])     / m.total;

        for (;;) {
            if (high < AR_HALF) {
                low   = low   * 2;
                high  = high  * 2 + 1;
                value = value * 2 + readBit();
            } else if (low >= AR_HALF) {
                low   = (low   - AR_HALF) * 2;
                high  = (high  - AR_HALF) * 2 + 1;
                value = (value - AR_HALF) * 2 + readBit();
            } else if (low >= AR_QTR && high < 3 * AR_QTR) {
                low   = (low   - AR_QTR) * 2;
                high  = (high  - AR_QTR) * 2 + 1;
                value = (value - AR_QTR) * 2 + readBit();
            } else break;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// .flif file format:
//   [4]  magic  "FLIF"
//   [4]  width  (uint32 LE)
//   [4]  height (uint32 LE)
//   [1]  format (0=BMP, 1=PGM)
//   [4]  residual size (uint32 LE)  — original residual byte count
//   [257*4] frequency table (257 × uint32 LE)
//   [N]  arithmetic-coded residual data
// ─────────────────────────────────────────────────────────────────────────────

ImageResult FLIFCodec::compress(const string& inputPath,
                                 const string& outputDir) {
    ImageResult res;
    res.inputFormat = extOf(inputPath);
    res.originalSize = FileUtils::fileSize(inputPath);

    try {
        // Load image
        Image img;
        uint8_t fmt = 0;
        if (res.inputFormat == ".bmp")      { img = ImageIO::readBMP(inputPath); fmt = 0; }
        else if (res.inputFormat == ".pgm") { img = ImageIO::readPGM(inputPath); fmt = 1; }
        else throw runtime_error("FLIF: only .bmp and .pgm supported");

        res.width  = img.width;
        res.height = img.height;

        // Step 1: Apply MANIAC-style MED prediction to get residuals
        auto residuals = applyMEDFilter(img);

        // Step 2: Build arithmetic coding model on residuals
        ArithModel model;
        model.build(residuals);

        // Step 3: Arithmetic-encode the residuals
        auto encoded = arithEncode(residuals, model);

        // Step 4: Build output file
        vector<uint8_t> out;
        auto push32 = [&](uint32_t v) {
            out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF);
            out.push_back((v >> 16) & 0xFF); out.push_back((v >> 24) & 0xFF);
        };

        // Magic
        out.push_back('F'); out.push_back('L');
        out.push_back('I'); out.push_back('F');
        // Dimensions
        push32((uint32_t)img.width);
        push32((uint32_t)img.height);
        // Original format
        out.push_back(fmt);
        // Residual size (needed for decoder)
        push32((uint32_t)residuals.size());
        // Frequency table (257 entries: 0-255 + EOF)
        for (int i = 0; i <= 256; i++) push32(model.freq[i]);
        // Encoded data
        out.insert(out.end(), encoded.begin(), encoded.end());

        // Write file
        string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        string outPath = dir + "/" + baseOf(inputPath) + "_compressed.flif";
        if (!FileUtils::writeBinary(outPath, out))
            throw runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".flif";
        res.success      = true;

    } catch (exception& e) { res.errorMessage = e.what(); }
    return res;
}

ImageResult FLIFCodec::decompress(const string& inputPath,
                                    const string& outputDir) {
    ImageResult res;
    res.inputFormat  = ".flif";
    res.originalSize = FileUtils::fileSize(inputPath);

    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.size() < 17)
            throw runtime_error("File too small for .flif");
        if (data[0] != 'F' || data[1] != 'L' || data[2] != 'I' || data[3] != 'F')
            throw runtime_error("Not a valid .flif file from this tool");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v = data[idx] | (data[idx+1] << 8) |
                         (data[idx+2] << 16) | (data[idx+3] << 24);
            idx += 4; return v;
        };

        uint32_t w   = r32();
        uint32_t h   = r32();
        uint8_t  fmt = data[idx++];
        uint32_t residualSize = r32();

        res.width  = w;
        res.height = h;

        // Read frequency table
        ArithModel model;
        for (int i = 0; i <= 256; i++) model.freq[i] = r32();
        model.buildCum();

        // Decode arithmetic-coded residuals
        vector<uint8_t> payload(data.begin() + idx, data.end());
        auto residuals = arithDecode(payload, model, residualSize);
        residuals.resize(residualSize);

        // Reconstruct image from residuals using inverse MED prediction
        Image img = removeMEDFilter(residuals, w, h);

        // Write output
        string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        string ext = (fmt == 1) ? ".pgm" : ".bmp";
        string outPath = dir + "/" + baseOf(inputPath) + "_decompressed" + ext;
        bool ok = (fmt == 1) ? ImageIO::writePGM(outPath, img)
                             : ImageIO::writeBMP(outPath, img);
        if (!ok) throw runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = FileUtils::fileSize(outPath);
        res.outputFormat = ext;
        res.success      = true;

    } catch (exception& e) { res.errorMessage = e.what(); }
    return res;
}
