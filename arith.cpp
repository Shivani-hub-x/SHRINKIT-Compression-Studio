#include "arith.h"
#include <stdexcept>
#include <cstring>

// ── Arithmetic coding with 32-bit integer range ───────────────────────────────
// Range: [low, high) scaled to [0, 2^32)
// Symbols: 0..255 = bytes, 256 = EOF

static const uint32_t TOP    = 0xFFFFFFFF;
static const uint32_t HALF   = 0x80000000;
static const uint32_t QTR    = 0x40000000;

void ArithCodec::Model::build(const std::vector<uint8_t>& data) {
    memset(freq, 0, sizeof(freq));
    for (uint8_t b : data) freq[b]++;
    freq[256] = 1;  // EOF symbol
    // Scale down if total would overflow
    uint64_t sum = 0;
    for (int i = 0; i <= 256; i++) sum += freq[i];
    if (sum > 1<<24) {
        for (int i = 0; i <= 256; i++)
            freq[i] = (freq[i] >> 4) + 1;
    }
    buildCum();
}

void ArithCodec::Model::buildCum() {
    cumFreq[0] = 0;
    for (int i = 0; i <= 256; i++)
        cumFreq[i+1] = cumFreq[i] + (freq[i] ? freq[i] : 1);
    total = cumFreq[257];
}

// ── Encode ────────────────────────────────────────────────────────────────────
std::vector<uint8_t> ArithCodec::encode(const std::vector<uint8_t>& data,
                                         const Model& m) {
    std::vector<uint8_t> outBits;
    uint64_t low  = 0;
    uint64_t high = TOP;
    int pending   = 0;

    auto emitBit = [&](int bit) {
        outBits.push_back((uint8_t)bit);
        for (int i = 0; i < pending; i++)
            outBits.push_back((uint8_t)(1 - bit));
        pending = 0;
    };

    auto encodeSymbol = [&](int sym) {
        uint64_t range = high - low + 1;
        high = low + (range * m.cumFreq[sym+1]) / m.total - 1;
        low  = low + (range * m.cumFreq[sym])   / m.total;
        for (;;) {
            if (high < HALF) {
                emitBit(0);
                low  = low  * 2;
                high = high * 2 + 1;
            } else if (low >= HALF) {
                emitBit(1);
                low  = (low  - HALF) * 2;
                high = (high - HALF) * 2 + 1;
            } else if (low >= QTR && high < 3*QTR) {
                pending++;
                low  = (low  - QTR) * 2;
                high = (high - QTR) * 2 + 1;
            } else break;
        }
    };

    for (uint8_t b : data) encodeSymbol(b);
    encodeSymbol(256); // EOF
    pending++;
    if (low < QTR) emitBit(0); else emitBit(1);

    // Pack bits into bytes
    std::vector<uint8_t> out;
    uint8_t cur = 0; int cnt = 0;
    for (uint8_t bit : outBits) {
        cur = (cur << 1) | bit;
        if (++cnt == 8) { out.push_back(cur); cur=0; cnt=0; }
    }
    if (cnt) out.push_back((uint8_t)(cur << (8-cnt)));
    return out;
}

// ── Decode ────────────────────────────────────────────────────────────────────
std::vector<uint8_t> ArithCodec::decode(const std::vector<uint8_t>& packed,
                                         const Model& m, uint32_t origSize) {
    // Unpack bits
    std::vector<uint8_t> bits;
    bits.reserve(packed.size()*8);
    for (uint8_t byte : packed)
        for (int j=7; j>=0; j--)
            bits.push_back((byte>>j)&1);

    size_t bitIdx = 0;
    auto readBit = [&]() -> uint64_t {
        return (bitIdx < bits.size()) ? bits[bitIdx++] : 0;
    };

    uint64_t low   = 0;
    uint64_t high  = TOP;
    uint64_t value = 0;
    for (int i = 0; i < 32; i++) value = (value << 1) | readBit();

    std::vector<uint8_t> out;
    out.reserve(origSize);

    for (;;) {
        uint64_t range = high - low + 1;
        uint64_t scaled = ((value - low + 1) * m.total - 1) / range;

        // Binary search for symbol
        int sym = 0;
        for (int i = 0; i <= 256; i++) {
            if (m.cumFreq[i+1] > scaled) { sym = i; break; }
        }
        if (sym == 256) break; // EOF
        out.push_back((uint8_t)sym);
        if (out.size() >= origSize) break;

        high = low + (range * m.cumFreq[sym+1]) / m.total - 1;
        low  = low + (range * m.cumFreq[sym])   / m.total;

        for (;;) {
            if (high < HALF) {
                low   = low  * 2;
                high  = high * 2 + 1;
                value = value * 2 + readBit();
            } else if (low >= HALF) {
                low   = (low   - HALF) * 2;
                high  = (high  - HALF) * 2 + 1;
                value = (value - HALF) * 2 + readBit();
            } else if (low >= QTR && high < 3*QTR) {
                low   = (low   - QTR) * 2;
                high  = (high  - QTR) * 2 + 1;
                value = (value - QTR) * 2 + readBit();
            } else break;
        }
    }
    return out;
}

// ── File helpers ──────────────────────────────────────────────────────────────
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

CodecResult ArithCodec::compress(const std::string& inputPath,
                                  const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = extOf(inputPath);
    if (res.inputFormat.empty()) res.inputFormat = "(none)";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.empty()) throw std::runtime_error("Input file is empty");
        res.originalSize = data.size();

        Model m; m.build(data);
        auto encoded = encode(data, m);

        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        out.push_back('A'); out.push_back('R');
        out.push_back('T'); out.push_back('H');
        push32((uint32_t)data.size());

        // Store frequency table (256 x uint32)
        for (int i = 0; i <= 256; i++) push32(m.freq[i]);

        std::string ext = res.inputFormat;
        out.push_back((uint8_t)ext.size());
        for (char c : ext) out.push_back((uint8_t)c);
        out.insert(out.end(), encoded.begin(), encoded.end());

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.arith";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".arith";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

CodecResult ArithCodec::decompress(const std::string& inputPath,
                                    const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = ".arith";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data[0]!='A'||data[1]!='R'||data[2]!='T'||data[3]!='H')
            throw std::runtime_error("Not a valid .arith file");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v=data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        uint32_t origSize = r32();

        Model m;
        for (int i = 0; i <= 256; i++) m.freq[i] = r32();
        m.buildCum();

        uint8_t extLen = data[idx++];
        std::string origExt(reinterpret_cast<char*>(&data[idx]), extLen);
        idx += extLen;
        res.outputFormat = origExt;

        std::vector<uint8_t> payload(data.begin()+idx, data.end());
        auto decoded = decode(payload, m, origSize);

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_decompressed" + origExt;
        if (!FileUtils::writeBinary(outPath, decoded))
            throw std::runtime_error("Cannot write: " + outPath);

        res.originalSize = data.size();
        res.resultSize   = decoded.size();
        res.outputPath   = outPath;
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}
