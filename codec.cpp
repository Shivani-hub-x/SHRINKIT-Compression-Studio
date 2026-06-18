#include "codec.h"
#include "huffman.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

// ─── FileUtils ───────────────────────────────────────────────────────────────
std::vector<uint8_t> FileUtils::readBinary(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open: " + path);
    auto sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

bool FileUtils::writeBinary(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return f.good();
}

uint64_t FileUtils::fileSize(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    return f ? static_cast<uint64_t>(f.tellg()) : 0;
}

std::string FileUtils::extension(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::string FileUtils::baseName(const std::string& path) {
    std::string p = path;
    for (auto& c : p) if (c == '\\') c = '/';
    auto sl  = p.rfind('/');
    auto dot = p.rfind('.');
    size_t start = (sl  == std::string::npos) ? 0 : sl + 1;
    size_t len   = (dot == std::string::npos || dot < start) ? std::string::npos : dot - start;
    return p.substr(start, len);
}

std::string FileUtils::dirName(const std::string& path) {
    std::string p = path;
    for (auto& c : p) if (c == '\\') c = '/';
    auto sl = p.rfind('/');
    return (sl == std::string::npos) ? "." : p.substr(0, sl);
}

std::string FileUtils::formatSize(uint64_t bytes) {
    char buf[64];
    if      (bytes < 1024)           snprintf(buf, sizeof(buf), "%llu B",   (unsigned long long)bytes);
    else if (bytes < 1024*1024)      snprintf(buf, sizeof(buf), "%.2f KB",  bytes / 1024.0);
    else if (bytes < 1024ULL*1024*1024) snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0*1024));
    else                             snprintf(buf, sizeof(buf), "%.2f GB",  bytes / (1024.0*1024*1024));
    return buf;
}

// ─── isHufFile ───────────────────────────────────────────────────────────────
bool Codec::isHufFile(const std::string& path) {
    if (FileUtils::extension(path) != ".huf") return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    return magic == 0x48554646;
}

// ─── COMPRESS ────────────────────────────────────────────────────────────────
CodecResult Codec::compress(const std::string& inputPath, const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = FileUtils::extension(inputPath);
    if (res.inputFormat.empty()) res.inputFormat = "(none)";

    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.empty()) throw std::runtime_error("Input file is empty");
        res.originalSize = data.size();

        auto freq    = HuffmanCodec::buildFreqTable(data);
        auto root    = HuffmanCodec::buildTree(freq);
        auto codes   = HuffmanCodec::generateCodes(root);
        uint8_t pad  = 0;
        auto encoded = HuffmanCodec::encode(data, codes, pad);

        std::vector<uint8_t> out;
        auto push32 = [&](uint32_t v) {
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
            out.push_back((v>>16)&0xFF); out.push_back((v>>24)&0xFF);
        };
        auto push16 = [&](uint16_t v) {
            out.push_back(v&0xFF); out.push_back((v>>8)&0xFF);
        };

        push32(0x48554646);                         // magic "HUFF"
        push32((uint32_t)data.size());              // original size
        out.push_back(pad);                         // padding bits
        push16((uint16_t)freq.size());              // num symbols

        // Store original extension
        std::string ext = res.inputFormat;
        out.push_back((uint8_t)ext.size());
        for (char c : ext) out.push_back((uint8_t)c);

        // Frequency table
        for (auto& [sym, f] : freq) { out.push_back(sym); push32(f); }

        // Bitstream
        out.insert(out.end(), encoded.begin(), encoded.end());

        std::string dir     = outputDir.empty() ? FileUtils::dirName(inputPath) : outputDir;
        std::string outPath = dir + "/" + FileUtils::baseName(inputPath) + "_compressed.huf";

        if (!FileUtils::writeBinary(outPath, out))
            throw std::runtime_error("Failed to write: " + outPath);

        res.outputPath   = outPath;
        res.resultSize   = out.size();
        res.outputFormat = ".huf";
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}

// ─── DECOMPRESS ──────────────────────────────────────────────────────────────
CodecResult Codec::decompress(const std::string& inputPath, const std::string& outputDir) {
    CodecResult res;
    res.inputFormat = ".huf";

    try {
        auto data = FileUtils::readBinary(inputPath);
        if (data.size() < 12) throw std::runtime_error("File too small");

        size_t idx = 0;
        auto read32 = [&]() -> uint32_t {
            uint32_t v = data[idx]|(data[idx+1]<<8)|(data[idx+2]<<16)|(data[idx+3]<<24);
            idx+=4; return v;
        };
        auto read16 = [&]() -> uint16_t {
            uint16_t v = data[idx]|(data[idx+1]<<8); idx+=2; return v;
        };

        if (read32() != 0x48554646) throw std::runtime_error("Not a valid .huf file");
        uint32_t origSize    = read32();
        uint8_t  paddingBits = data[idx++];
        uint16_t numSymbols  = read16();

        uint8_t extLen = data[idx++];
        std::string origExt(reinterpret_cast<char*>(&data[idx]), extLen);
        idx += extLen;
        res.outputFormat = origExt;

        std::unordered_map<uint8_t, uint32_t> freq;
        for (int i = 0; i < numSymbols; i++) {
            uint8_t sym = data[idx++];
            freq[sym]   = read32();
        }

        auto root    = HuffmanCodec::buildTree(freq);
        std::vector<uint8_t> bits(data.begin() + idx, data.end());
        auto decoded = HuffmanCodec::decode(bits, root, paddingBits, origSize);

        std::string dir     = outputDir.empty() ? FileUtils::dirName(inputPath) : outputDir;
        std::string outPath = dir + "/" + FileUtils::baseName(inputPath) + "_decompressed" + origExt;

        if (!FileUtils::writeBinary(outPath, decoded))
            throw std::runtime_error("Failed to write: " + outPath);

        res.originalSize = data.size();
        res.resultSize   = decoded.size();
        res.outputPath   = outPath;
        res.success      = true;
    } catch (std::exception& e) { res.errorMessage = e.what(); }
    return res;
}
