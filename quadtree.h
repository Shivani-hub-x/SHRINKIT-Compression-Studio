#pragma once
#include <vector>
#include <cstdint>
#include <string>

// ── Pixel formats we support ─────────────────────────────────────────────────
struct RGBPixel  { uint8_t r, g, b; };
struct GrayPixel { uint8_t v; };

// ── Image container (always stored as RGB internally) ────────────────────────
struct Image {
    int width  = 0;
    int height = 0;
    std::vector<RGBPixel> pixels; // row-major, top-left origin

    RGBPixel& at(int x, int y)       { return pixels[y * width + x]; }
    const RGBPixel& at(int x, int y) const { return pixels[y * width + x]; }
};

// ── Quadtree node ─────────────────────────────────────────────────────────────
struct QTNode {
    bool    isLeaf  = false;
    uint8_t r, g, b;           // average colour (only used for leaves)
    int x, y, w, h;            // region this node covers
    QTNode* children[4] = {};  // TL, TR, BL, BR

    ~QTNode() {
        for (auto* c : children) delete c;
    }
};

// ── Quadtree codec ────────────────────────────────────────────────────────────
class QuadTree {
public:
    static const int   THRESHOLD = 20;  // colour variance threshold
    static const int   MIN_SIZE  = 2;   // minimum block side length

    // Build quadtree from image
    static QTNode* build(const Image& img, int x, int y, int w, int h);

    // Render quadtree back to image (reconstructed)
    static void    render(const Image& src, QTNode* node, Image& out);

    // Flatten tree to bytes (for file storage)
    static std::vector<uint8_t> serialise(QTNode* root);

    // Rebuild tree from bytes
    static QTNode* deserialise(const std::vector<uint8_t>& data, size_t& idx,
                                int x, int y, int w, int h);

    static void freeTree(QTNode* node) { delete node; }

private:
    static bool   isUniform(const Image& img, int x, int y, int w, int h,
                             uint8_t& avgR, uint8_t& avgG, uint8_t& avgB);
};
