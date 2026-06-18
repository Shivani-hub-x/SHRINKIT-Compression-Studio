#include "quadtree.h"
#include <cmath>
#include <stdexcept>

// ── Check if region is uniform enough to become a leaf ───────────────────────
bool QuadTree::isUniform(const Image& img, int x, int y, int w, int h,
                          uint8_t& avgR, uint8_t& avgG, uint8_t& avgB) {
    long sumR=0, sumG=0, sumB=0;
    int  count = w * h;

    for (int py = y; py < y+h; py++)
        for (int px = x; px < x+w; px++) {
            const auto& p = img.at(px, py);
            sumR += p.r; sumG += p.g; sumB += p.b;
        }

    avgR = (uint8_t)(sumR / count);
    avgG = (uint8_t)(sumG / count);
    avgB = (uint8_t)(sumB / count);

    // Check max deviation from average
    for (int py = y; py < y+h; py++)
        for (int px = x; px < x+w; px++) {
            const auto& p = img.at(px, py);
            if (std::abs((int)p.r - avgR) > THRESHOLD) return false;
            if (std::abs((int)p.g - avgG) > THRESHOLD) return false;
            if (std::abs((int)p.b - avgB) > THRESHOLD) return false;
        }
    return true;
}

// ── Build quadtree recursively ────────────────────────────────────────────────
QTNode* QuadTree::build(const Image& img, int x, int y, int w, int h) {
    QTNode* node = new QTNode();
    node->x = x; node->y = y; node->w = w; node->h = h;

    uint8_t avgR, avgG, avgB;
    bool uniform = isUniform(img, x, y, w, h, avgR, avgG, avgB);

    if (uniform || w <= MIN_SIZE || h <= MIN_SIZE) {
        node->isLeaf = true;
        node->r = avgR; node->g = avgG; node->b = avgB;
        return node;
    }

    // Split into 4 quadrants
    int hw = w / 2;
    int hh = h / 2;
    int rw = w - hw;   // right width  (handles odd sizes)
    int bh = h - hh;   // bottom height

    node->children[0] = build(img, x,    y,    hw, hh); // TL
    node->children[1] = build(img, x+hw, y,    rw, hh); // TR
    node->children[2] = build(img, x,    y+hh, hw, bh); // BL
    node->children[3] = build(img, x+hw, y+hh, rw, bh); // BR

    return node;
}

// ── Render tree back to image ─────────────────────────────────────────────────
void QuadTree::render(const Image& /*src*/, QTNode* node, Image& out) {
    if (!node) return;
    if (node->isLeaf) {
        for (int py = node->y; py < node->y + node->h; py++)
            for (int px = node->x; px < node->x + node->w; px++)
                out.at(px, py) = { node->r, node->g, node->b };
        return;
    }
    for (auto* c : node->children) render({}, c, out);
}

// ── Serialise: 1 byte flag + 3 bytes colour (leaf) OR flag + 4 children ──────
// Format per node:
//   0x01 = leaf  → [0x01][R][G][B]
//   0x00 = inner → [0x00] then 4 children serialised in order
std::vector<uint8_t> QuadTree::serialise(QTNode* node) {
    std::vector<uint8_t> out;
    if (!node) return out;

    if (node->isLeaf) {
        out.push_back(0x01);
        out.push_back(node->r);
        out.push_back(node->g);
        out.push_back(node->b);
    } else {
        out.push_back(0x00);
        for (auto* c : node->children) {
            auto sub = serialise(c);
            out.insert(out.end(), sub.begin(), sub.end());
        }
    }
    return out;
}

// ── Deserialise ───────────────────────────────────────────────────────────────
QTNode* QuadTree::deserialise(const std::vector<uint8_t>& data, size_t& idx,
                               int x, int y, int w, int h) {
    if (idx >= data.size())
        throw std::runtime_error("Deserialise: unexpected end of data");

    QTNode* node = new QTNode();
    node->x = x; node->y = y; node->w = w; node->h = h;

    uint8_t flag = data[idx++];

    if (flag == 0x01) {
        if (idx + 2 >= data.size())
            throw std::runtime_error("Deserialise: truncated leaf");
        node->isLeaf = true;
        node->r = data[idx++];
        node->g = data[idx++];
        node->b = data[idx++];
    } else {
        int hw = w / 2, hh = h / 2;
        int rw = w - hw, bh = h - hh;
        node->children[0] = deserialise(data, idx, x,    y,    hw, hh);
        node->children[1] = deserialise(data, idx, x+hw, y,    rw, hh);
        node->children[2] = deserialise(data, idx, x,    y+hh, hw, bh);
        node->children[3] = deserialise(data, idx, x+hw, y+hh, rw, bh);
    }
    return node;
}
