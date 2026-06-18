#include "rle.h"
#include <fstream>
#include <stdexcept>

// ── RLE compress ─────────────────────────────────────────────────────────────
// Escape byte = 0xFE (avoids conflicts)
// [0xFE][count][byte] = run of 'count' bytes (count 2..255)
// [0xFE][0x00]        = literal 0xFE
// anything else       = literal byte
static const uint8_t ESC = 0xFE;

std::vector<uint8_t> RLE::compress(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size());
    size_t i = 0;
    while (i < data.size()) {
        uint8_t cur = data[i];
        // Count run length
        size_t run = 1;
        while (i + run < data.size() && data[i + run] == cur && run < 255)
            run++;
        if (cur == ESC) {
            // Emit literal ESC as escape sequence
            out.push_back(ESC);
            out.push_back(0x00);
            i++;
        } else if (run >= 3) {
            // Emit run
            out.push_back(ESC);
            out.push_back((uint8_t)run);
            out.push_back(cur);
            i += run;
        } else {
            // Emit literal(s)
            for (size_t k = 0; k < run; k++) out.push_back(cur);
            i += run;
        }
    }
    return out;
}

std::vector<uint8_t> RLE::decompress(const std::vector<uint8_t>& data,
                                      uint32_t originalSize) {
    std::vector<uint8_t> out;
    out.reserve(originalSize);
    size_t i = 0;
    while (i < data.size()) {
        if (data[i] == ESC) {
            i++;
            if (i >= data.size()) break;
            uint8_t count = data[i++];
            if (count == 0x00) {
                out.push_back(ESC);
            } else {
                if (i >= data.size()) throw std::runtime_error("RLE: truncated run");
                uint8_t byte = data[i++];
                for (int k = 0; k < count; k++) out.push_back(byte);
            }
        } else {
            out.push_back(data[i++]);
        }
    }
    return out;
}

// ── File-level helpers (reuse FileUtils from codec.cpp) ──────────────────────
static std::string extOf(const std::string& p) {
    auto pos = p.rfind('.'); if (pos==std::string::npos) return "";
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

CodecResult RLECodec::compress(const std::string& inputPath,
                                const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = extOf(inputPath);
    if (res.inputFormat.empty()) res.inputFormat = "(none)";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.empty()) throw std::runtime_error("Input file is empty");
        res.originalSize = data.size();

        auto encoded = RLE::compress(data);

        // Header: magic(4) + origSize(4) + ext_len(1) + ext(N)
        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v){
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        out.push_back('R'); out.push_back('L');
        out.push_back('E'); out.push_back('!');
        push32((uint32_t)data.size());
        std::string ext = res.inputFormat;
        out.push_back((uint8_t)ext.size());
        for (char c : ext) out.push_back((uint8_t)c);
        out.insert(out.end(), encoded.begin(), encoded.end());

        std::string dir = outputDir.empty() ? dirOf(inputPath) : outputDir;
        std::string outPath = dir + "/" + baseOf(inputPath) + "_compressed.rle";
        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Cannot write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".rle";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

CodecResult RLECodec::decompress(const std::string& inputPath,
                                  const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = ".rle";
    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.size() < 6) throw std::runtime_error("File too small");
        if (data[0]!='R'||data[1]!='L'||data[2]!='E'||data[3]!='!')
            throw std::runtime_error("Not a valid .rle file");

        size_t idx = 4;
        uint32_t origSize = data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
        idx += 4;
        uint8_t extLen = data[idx++];
        std::string origExt(reinterpret_cast<char*>(&data[idx]), extLen);
        idx += extLen;
        res.outputFormat = origExt;

        std::vector<uint8_t> payload(data.begin()+idx, data.end());
        auto decoded = RLE::decompress(payload, origSize);

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
