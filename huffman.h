#pragma once
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <queue>
#include <memory>
#include <cstdint>

struct HuffNode {
    uint8_t  symbol;
    uint32_t freq;
    std::shared_ptr<HuffNode> left, right;

    HuffNode(uint8_t s, uint32_t f) : symbol(s), freq(f) {}
    HuffNode(uint32_t f, std::shared_ptr<HuffNode> l, std::shared_ptr<HuffNode> r)
        : symbol(0), freq(f), left(l), right(r) {}

    bool isLeaf() const { return !left && !right; }
};

// Fix: tie-break by symbol so tree is IDENTICAL on encode and decode
struct NodeCmp {
    bool operator()(const std::shared_ptr<HuffNode>& a,
                    const std::shared_ptr<HuffNode>& b) const {
        if (a->freq != b->freq) return a->freq > b->freq;
        return a->symbol > b->symbol; // deterministic tie-break
    }
};

using CodeTable = std::unordered_map<uint8_t, std::string>;

class HuffmanCodec {
public:
    static std::unordered_map<uint8_t, uint32_t>
    buildFreqTable(const std::vector<uint8_t>& data);

    // Takes sorted map for deterministic tree building
    static std::shared_ptr<HuffNode>
    buildTree(const std::unordered_map<uint8_t, uint32_t>& freq);

    static CodeTable generateCodes(const std::shared_ptr<HuffNode>& root);

    static std::vector<uint8_t>
    encode(const std::vector<uint8_t>& data, const CodeTable& codes,
           uint8_t& paddingBits);

    static std::vector<uint8_t>
    decode(const std::vector<uint8_t>& bits, const std::shared_ptr<HuffNode>& root,
           uint8_t paddingBits, uint32_t originalSize);

private:
    static void dfs(const std::shared_ptr<HuffNode>& node,
                    const std::string& prefix, CodeTable& table);
};
