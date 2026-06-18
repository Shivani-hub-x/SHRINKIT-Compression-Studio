# Huffman Compressor
### Cross-platform GUI — Windows & macOS
Lossless Huffman coding for text & image files
![image alt](https://github.com/Shivani-hub-x/SHRINKIT-Compression-Studio/blob/86443dd3bc71edfcf066423504f5fdd3effd9516/Screenshot%202026-06-18%20151325.png)
![image alt](https://github.com/Shivani-hub-x/SHRINKIT-Compression-Studio/blob/e28838dda4e8f8357fd161cc04b8e6016b7bcd32/Screenshot%202026-06-18%20161027.png)
![image alt](https://github.com/Shivani-hub-x/SHRINKIT-Compression-Studio/blob/84f0a17793b62a1f4ac08307172344aadbb17fd9/Screenshot%202026-06-18%20161040.png)
![image alt](https://github.com/Shivani-hub-x/SHRINKIT-Compression-Studio/blob/ae0e72d4d72808a36670984eee6dae8562b3f03d/Screenshot%202026-06-18%20161134.png)
![image alt](https://github.com/Shivani-hub-x/SHRINKIT-Compression-Studio/blob/575955f718a76dd3f5df16232c422b7c63bd3339/Screenshot%202026-06-18%20161333.png)
![image alt](https://github.com/Shivani-hub-x/SHRINKIT-Compression-Studio/blob/7b1ce32763abd0f4ee22f6f6733cfe91084e0f55/Screenshot%202026-06-18%20161343.png)
---

## 📁 Project Structure

```
HuffmanCompressor/
├── huffman.h / huffman.cpp     ← Core Huffman algorithm  (portable)
├── codec.h   / codec.cpp       ← File compress/decompress (portable)
├── main.cpp                    ← GUI using SDL2 + Dear ImGui (cross-platform)
├── CMakeLists.txt              ← Build config (Windows + macOS + Linux)
├── imgui/                      ← Dear ImGui source (you download this)
│   ├── imgui.h / .cpp
│   ├── imgui_draw.cpp
│   ├── imgui_tables.cpp
│   ├── imgui_widgets.cpp
│   └── backends/
│       ├── imgui_impl_sdl2.h / .cpp
│       └── imgui_impl_sdlrenderer2.h / .cpp
└── README.md
```

---

## ⚙️ One-Time Setup: Download Dependencies

### Step 1 — Get Dear ImGui
```bash
# Inside the HuffmanCompressor folder:
git clone https://github.com/ocornut/imgui.git imgui
```
That's it — ImGui is header/source only, no install needed.

### Step 2 — Install SDL2

**macOS (Homebrew):**
```bash
brew install sdl2
```

**Windows (vcpkg):**
```bat
vcpkg install sdl2:x64-windows
```
Or download SDL2-devel from https://github.com/libsdl-org/SDL/releases
and set `SDL2_DIR` in your cmake command.

**Ubuntu/Debian:**
```bash
sudo apt install libsdl2-dev
```

---

## 🔨 Build Instructions

### macOS
```bash
cd HuffmanCompressor
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/HuffmanCompressor.app
```

### Windows (Visual Studio)
```bat
cd HuffmanCompressor
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
build\Release\HuffmanCompressor.exe
```

### Windows (MinGW)
```bat
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\HuffmanCompressor.exe
```

---

## 🖥️ How to Use

| Action | Steps |
|--------|-------|
| **Compress** | Drop any file (`.txt`, `.bmp`, `.png`, `.jpg`, etc.) → LEFT panel → click **Compress** |
| **Decompress** | Drop a `.huf` file → RIGHT panel → click **Decompress** |
| **Auto-route** | Dropping a `.huf` file anywhere auto-routes it to the Decompress panel |

The app displays:
- Input format & file size
- Output format & file size  
- Space saved (compression) or size restored (decompression)
- Full output file path (saved beside the input)

---

## 🔬 Algorithm

1. **Frequency table** — count every byte in the input  
2. **Min-heap** — priority queue ordered by frequency  
3. **Tree build** — merge two lowest-freq nodes until one root remains  
4. **Code gen** — DFS assigns bit strings: `0` = left, `1` = right  
5. **Encode** — replace bytes with variable-length codes, pack bits  
6. **Header** — store magic + original size + full freq table in `.huf`  
7. **Decode** — rebuild tree from stored freqs, walk bit-by-bit  

### .huf File Format
```
[4]  Magic "HUFF" (0x48554646)
[4]  Original file size
[1]  Padding bits in last encoded byte
[2]  Number of unique symbols
[1]  Original extension length (N)
[N]  Original extension string  e.g. ".txt"
[K×5] Frequency table: (1 byte symbol + 4 byte freq) × K symbols
[...]  Packed Huffman bitstream
```

---

## 📊 Expected Compression Ratios
| File type | Typical saving |
|-----------|----------------|
| Plain text (.txt) | 40–60% smaller |
| BMP image | 20–50% smaller |
| Pre-compressed (.jpg/.png) | ~0% (may grow slightly) |

All operations are **lossless** — decompressed output is byte-for-byte identical to original.

rm -rf build
cmake -B build
cmake --build build
./build/HuffmanCompressor.app/Contents/MacOS/HuffmanCompressor
