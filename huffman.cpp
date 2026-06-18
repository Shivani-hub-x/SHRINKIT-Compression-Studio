#include "huffman.h"
#include <stdexcept>
#include <map>

std::unordered_map<uint8_t, uint32_t>
HuffmanCodec::buildFreqTable(const std::vector<uint8_t>& data) {
    std::unordered_map<uint8_t, uint32_t> freq;
    for (uint8_t b : data) freq[b]++;
    return freq;
}

std::shared_ptr<HuffNode>
HuffmanCodec::buildTree(const std::unordered_map<uint8_t, uint32_t>& freq) {
    std::priority_queue<std::shared_ptr<HuffNode>,
                        std::vector<std::shared_ptr<HuffNode>>,
                        NodeCmp> pq;

    // Insert in sorted order by symbol for full determinism
    std::map<uint8_t, uint32_t> sorted(freq.begin(), freq.end());
    for (auto& [sym, f] : sorted)
        pq.push(std::make_shared<HuffNode>(sym, f));

    if (pq.size() == 1) {
        auto only = pq.top(); pq.pop();
        return std::make_shared<HuffNode>(only->freq,
               only, std::make_shared<HuffNode>(0, 0));
    }

    while (pq.size() > 1) {
        auto a = pq.top(); pq.pop();
        auto b = pq.top(); pq.pop();
        // Internal node: symbol=0, freq=sum
        auto merged = std::make_shared<HuffNode>(a->freq + b->freq, a, b);
        merged->symbol = 0;
        pq.push(merged);
    }
    return pq.top();
}

void HuffmanCodec::dfs(const std::shared_ptr<HuffNode>& node,
                       const std::string& prefix, CodeTable& table) {
    if (!node) return;
    if (node->isLeaf()) {
        table[node->symbol] = prefix.empty() ? "0" : prefix;
        return;
    }
    dfs(node->left,  prefix + "0", table);
    dfs(node->right, prefix + "1", table);
}

CodeTable HuffmanCodec::generateCodes(const std::shared_ptr<HuffNode>& root) {
    CodeTable table;
    dfs(root, "", table);
    return table;
}

std::vector<uint8_t>
HuffmanCodec::encode(const std::vector<uint8_t>& data, const CodeTable& codes,
                     uint8_t& paddingBits) {
    std::string bitStr;
    bitStr.reserve(data.size() * 4);
    for (uint8_t b : data) {
        auto it = codes.find(b);
        if (it == codes.end()) throw std::runtime_error("Symbol not in code table");
        bitStr += it->second;
    }
    paddingBits = (8 - (int)(bitStr.size() % 8)) % 8;
    for (int i = 0; i < paddingBits; i++) bitStr += '0';

    std::vector<uint8_t> packed;
    packed.reserve(bitStr.size() / 8);
    for (size_t i = 0; i < bitStr.size(); i += 8) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; j++)
            if (bitStr[i + j] == '1') byte |= (1 << (7 - j));
        packed.push_back(byte);
    }
    return packed;
}

std::vector<uint8_t>
HuffmanCodec::decode(const std::vector<uint8_t>& bits,
                     const std::shared_ptr<HuffNode>& root,
                     uint8_t paddingBits, uint32_t originalSize) {
    std::vector<uint8_t> output;
    output.reserve(originalSize);
    auto cur = root;
    size_t totalBits = bits.size() * 8 - paddingBits;
    size_t bitCount  = 0;

    for (uint8_t byte : bits) {
        for (int j = 7; j >= 0; j--) {
            if (bitCount++ >= totalBits) goto done;
            bool bit = (byte >> j) & 1;
            cur = bit ? cur->right : cur->left;
            if (!cur) throw std::runtime_error("Decode error: null node reached");
            if (cur->isLeaf()) {
                output.push_back(cur->symbol);
                cur = root;
                if (output.size() == originalSize) goto done;
            }
        }
    }
    done:
    return output;
}
