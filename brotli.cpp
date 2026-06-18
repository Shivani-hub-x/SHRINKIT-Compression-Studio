#include "brotli.h"
#include <unordered_map>
#include <stdexcept>

// ── LZ77 encode with hash-based match finding ─────────────────────────────────
std::vector<BrotliCodec::Token>
BrotliCodec::lz77Encode(const std::vector<uint8_t>& data) {
    std::vector<Token> tokens;
    // Hash table: 3-byte string -> last position seen
    std::unordered_map<uint32_t, int> ht;
    auto hash3 = [](const uint8_t* p) -> uint32_t {
        return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    };

    int i = 0;
    int n = (int)data.size();
    while (i < n) {
        if (i + MIN_MATCH <= n) {
            uint32_t h = hash3(&data[i]);
            int prev = -1;
            auto it = ht.find(h);
            if (it != ht.end()) prev = it->second;
            ht[h] = i;

            if (prev >= 0 && (i - prev) <= WIN_SIZE && prev < i) {
                // Find match length
                int len = 0;
                while (i + len < n && data[prev + len] == data[i + len] && len < 258)
                    len++;
                if (len >= MIN_MATCH) {
                    tokens.push_back({false, 0, i - prev, len});
                    i += len;
                    continue;
                }
            }
        }
        tokens.push_back({true, data[i], 0, 0});
        i++;
    }
    return tokens;
}

// ── LZ77 decode ──────────────────────────────────────────────────────────────
std::vector<uint8_t>
BrotliCodec::lz77Decode(const std::vector<Token>& tokens, uint32_t origSize) {
    std::vector<uint8_t> out;
    out.reserve(origSize);
    for (auto& t : tokens) {
        if (t.isLiteral) {
            out.push_back(t.literal);
        } else {
            int start = (int)out.size() - t.offset;
            if (start < 0) throw std::runtime_error("Brotli: bad back-reference");
            for (int k = 0; k < t.length; k++)
                out.push_back(out[start + k]);
        }
    }
    return out;
}

// ── Pack tokens to bytes ──────────────────────────────────────────────────────
// Format per token:
//   Literal:   [0x00][byte]
//   BackRef:   [0x01][offset_lo][offset_hi][length]  (offset 16-bit, length 8-bit+bias)
std::vector<uint8_t>
BrotliCodec::packTokens(const std::vector<Token>& tokens) {
    std::vector<uint8_t> out;
    for (auto& t : tokens) {
        if (t.isLiteral) {
            out.push_back(0x00);
            out.push_back(t.literal);
        } else {
            out.push_back(0x01);
            out.push_back((uint8_t)(t.offset & 0xFF));
            out.push_back((uint8_t)((t.offset >> 8) & 0xFF));
            // length: store as (length - MIN_MATCH) to save space
            int len = t.length - MIN_MATCH;
            if (len > 255) len = 255;
            out.push_back((uint8_t)len);
        }
    }
    return out;
}

std::vector<BrotliCodec::Token>
BrotliCodec::unpackTokens(const std::vector<uint8_t>& packed, uint32_t /*origSize*/) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < packed.size()) {
        uint8_t type = packed[i++];
        if (type == 0x00) {
            if (i >= packed.size()) break;
            tokens.push_back({true, packed[i++], 0, 0});
        } else {
            if (i + 2 >= packed.size()) break;
            int offset = packed[i] | (packed[i+1] << 8); i += 2;
            int length = (int)packed[i++] + MIN_MATCH;
            tokens.push_back({false, 0, offset, length});
        }
    }
    return tokens;
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

CodecResult BrotliCodec::compress(const std::string& inputPath,
                                   const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = extOf(inputPath);
    if (res.inputFormat.empty()) res.inputFormat = "(none)";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.empty()) throw std::runtime_error("Input file is empty");
        res.originalSize = data.size();

        auto tokens = lz77Encode(data);
        auto packed = packTokens(tokens);

        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        out.push_back('B'); out.push_back('R');
        out.push_back('O'); out.push_back('!');
        push32((uint32_t)data.size());
        std::string ext = res.inputFormat;
        out.push_back((uint8_t)ext.size());
        for (char c : ext) out.push_back((uint8_t)c);
        out.insert(out.end(), packed.begin(), packed.end());

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.bro";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".bro";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

CodecResult BrotliCodec::decompress(const std::string& inputPath,
                                     const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = ".bro";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data[0]!='B'||data[1]!='R'||data[2]!='O'||data[3]!='!')
            throw std::runtime_error("Not a valid .bro file");

        size_t idx = 4;
        uint32_t origSize = data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
        idx += 4;
        uint8_t extLen = data[idx++];
        std::string origExt(reinterpret_cast<char*>(&data[idx]), extLen);
        idx += extLen;
        res.outputFormat = origExt;

        std::vector<uint8_t> payload(data.begin()+idx, data.end());
        auto tokens  = unpackTokens(payload, origSize);
        auto decoded = lz77Decode(tokens, origSize);

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
