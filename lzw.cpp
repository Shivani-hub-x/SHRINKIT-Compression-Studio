#include "lzw.h"
#include <unordered_map>
#include <stdexcept>

static const int MAX_DICT = 4096;  // 12-bit codes

// ── LZW Compress ─────────────────────────────────────────────────────────────
std::vector<uint16_t> LZW::compress(const std::vector<uint8_t>& data) {
    std::unordered_map<std::string, uint16_t> dict;
    // Initialise with single-byte entries
    for (int i = 0; i < 256; i++)
        dict[std::string(1, (char)i)] = (uint16_t)i;

    std::vector<uint16_t> codes;
    std::string w;
    int nextCode = 256;

    for (uint8_t byte : data) {
        std::string wc = w + (char)byte;
        if (dict.count(wc)) {
            w = wc;
        } else {
            codes.push_back(dict[w]);
            if (nextCode < MAX_DICT)
                dict[wc] = (uint16_t)nextCode++;
            w = std::string(1, (char)byte);
        }
    }
    if (!w.empty()) codes.push_back(dict[w]);
    return codes;
}

// ── LZW Decompress ───────────────────────────────────────────────────────────
std::vector<uint8_t> LZW::decompress(const std::vector<uint16_t>& codes) {
    std::vector<std::string> dict;
    for (int i = 0; i < 256; i++)
        dict.push_back(std::string(1, (char)i));

    std::vector<uint8_t> out;
    if (codes.empty()) return out;

    std::string w(1, (char)codes[0]);
    for (auto& b : w) out.push_back((uint8_t)b);

    for (size_t i = 1; i < codes.size(); i++) {
        uint16_t k = codes[i];
        std::string entry;
        if (k < (uint16_t)dict.size())
            entry = dict[k];
        else if (k == (uint16_t)dict.size())
            entry = w + w[0];
        else
            throw std::runtime_error("LZW: bad code " + std::to_string(k));

        for (auto& b : entry) out.push_back((uint8_t)b);
        if (dict.size() < MAX_DICT)
            dict.push_back(w + entry[0]);
        w = entry;
    }
    return out;
}

// ── Pack 12-bit codes into bytes (3 bytes per 2 codes) ───────────────────────
std::vector<uint8_t> LZW::packCodes(const std::vector<uint16_t>& codes) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < codes.size(); i += 2) {
        uint16_t a = codes[i];
        uint16_t b = (i+1 < codes.size()) ? codes[i+1] : 0;
        out.push_back((uint8_t)(a >> 4));
        out.push_back((uint8_t)(((a & 0xF) << 4) | (b >> 8)));
        out.push_back((uint8_t)(b & 0xFF));
    }
    return out;
}

std::vector<uint16_t> LZW::unpackCodes(const std::vector<uint8_t>& packed) {
    std::vector<uint16_t> codes;
    for (size_t i = 0; i + 2 < packed.size(); i += 3) {
        codes.push_back((uint16_t)((packed[i] << 4) | (packed[i+1] >> 4)));
        codes.push_back((uint16_t)(((packed[i+1] & 0xF) << 8) | packed[i+2]));
    }
    // Handle odd trailing byte-triple
    if (packed.size() % 3 == 3) { /* already handled */ }
    return codes;
}

// ── File helpers (same pattern as rle.cpp) ────────────────────────────────────
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

CodecResult LZWCodec::compress(const std::string& inputPath,
                                const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = extOf(inputPath);
    if (res.inputFormat.empty()) res.inputFormat = "(none)";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.empty()) throw std::runtime_error("Input file is empty");
        res.originalSize = data.size();

        auto codes  = LZW::compress(data);
        auto packed = LZW::packCodes(codes);

        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        out.push_back('L'); out.push_back('Z');
        out.push_back('W'); out.push_back('!');
        push32((uint32_t)data.size());
        push32((uint32_t)codes.size()); // number of codes (needed for unpack)
        std::string ext = res.inputFormat;
        out.push_back((uint8_t)ext.size());
        for (char c : ext) out.push_back((uint8_t)c);
        out.insert(out.end(), packed.begin(), packed.end());

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.lzw";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".lzw";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

CodecResult LZWCodec::decompress(const std::string& inputPath,
                                  const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = ".lzw";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.size() < 10) throw std::runtime_error("File too small");
        if (data[0]!='L'||data[1]!='Z'||data[2]!='W'||data[3]!='!')
            throw std::runtime_error("Not a valid .lzw file");

        size_t idx = 4;
        auto r32 = [&]() -> uint32_t {
            uint32_t v=data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        uint32_t origSize  = r32();
        (void)origSize; // not directly needed; used implicitly via numCodes
        uint32_t numCodes  = r32();
        uint8_t  extLen    = data[idx++];
        std::string origExt(reinterpret_cast<char*>(&data[idx]), extLen);
        idx += extLen;
        res.outputFormat = origExt;

        std::vector<uint8_t> packed(data.begin()+idx, data.end());
        auto codes   = LZW::unpackCodes(packed);
        codes.resize(numCodes); // trim to exact count
        auto decoded = LZW::decompress(codes);

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
