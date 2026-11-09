// GTS-010: photo export. GT Sport saves Scapes and photo mode pictures to
// download0\APP_DATA\photo as "PJXR" files: a header with the picture size, a tile grid (3x3) and
// each tile's offset, every tile an ordinary JPEG XR image. A background thread watches that
// folder and writes each new photo as a PNG to the user's Pictures folder
// (Pictures\Gran Turismo Sport), decoded with Windows' own JPEG XR codec (WIC). Photos already
// there when the game starts are left alone, as are the "_t" previews.
// On unless RYGT_PHOTO_EXPORT=0 or "photo_export": false in the game's config.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

bool ConfigFlag(const char* key, bool fallback); // ryty_game_patch.cpp

namespace {

namespace fs = std::filesystem;

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() {
        return &p;
    }
    T* operator->() const {
        return p;
    }
};

std::uint32_t Read32(const std::vector<std::uint8_t>& data, size_t at) {
    std::uint32_t value;
    std::memcpy(&value, data.data() + at, 4);
    return value;
}

std::uint16_t Read16(const std::vector<std::uint8_t>& data, size_t at) {
    std::uint16_t value;
    std::memcpy(&value, data.data() + at, 2);
    return value;
}

// Decodes one PJXR photo into a BGRA picture; false if it is not one.
bool DecodePhoto(IWICImagingFactory* factory, const std::vector<std::uint8_t>& data,
                 std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& pixels) {
    if (data.size() < 0x50 || std::memcmp(data.data(), "PJXR", 4) != 0) {
        return false;
    }
    width = Read32(data, 8);
    height = Read32(data, 12);
    const std::uint32_t cols = Read16(data, 16);
    const std::uint32_t rows = Read16(data, 18);
    const std::uint32_t count = Read32(data, 24);
    if (count == 0 || count != cols * rows || count > 64 || width == 0 || height == 0 ||
        width > 16384 || height > 16384 || data.size() < 32 + 4 * size_t(count)) {
        return false;
    }
    pixels.assign(size_t(width) * height * 4, 0);
    std::vector<std::uint32_t> col_x(cols, 0), row_y(rows, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t start = Read32(data, 32 + 4 * i);
        const std::uint32_t end = i + 1 < count ? Read32(data, 32 + 4 * (i + 1))
                                                : std::uint32_t(data.size());
        if (start >= end || end > data.size()) {
            return false;
        }
        Com<IWICStream> stream;
        Com<IWICBitmapDecoder> decoder;
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICFormatConverter> converter;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data.data() + start),
                                                end - start)) ||
            FAILED(factory->CreateDecoderFromStream(stream.p, nullptr,
                                                    WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) ||
            FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) {
            return false;
        }
        UINT tile_w = 0, tile_h = 0;
        converter->GetSize(&tile_w, &tile_h);
        // Tiles go row by row, left to right; a column's x comes from the first row's widths,
        // a row's y from the first column's heights.
        const std::uint32_t r = i / cols, c = i % cols;
        if (r == 0 && c + 1 < cols) col_x[c + 1] = col_x[c] + tile_w;
        if (c == 0 && r + 1 < rows) row_y[r + 1] = row_y[r] + tile_h;
        const std::uint32_t x = col_x[c], y = row_y[r];
        if (x >= width || y >= height) {
            continue;
        }
        const UINT copy_w = std::min<UINT>(tile_w, width - x);
        const UINT copy_h = std::min<UINT>(tile_h, height - y);
        std::vector<std::uint8_t> tile(size_t(tile_w) * tile_h * 4);
        const WICRect rect{0, 0, INT(tile_w), INT(tile_h)};
        if (FAILED(converter->CopyPixels(&rect, tile_w * 4, UINT(tile.size()), tile.data()))) {
            return false;
        }
        for (UINT row = 0; row < copy_h; ++row) {
            std::memcpy(pixels.data() + (size_t(y + row) * width + x) * 4,
                        tile.data() + size_t(row) * tile_w * 4, size_t(copy_w) * 4);
        }
    }
    return true;
}

bool WritePng(IWICImagingFactory* factory, const fs::path& path, std::uint32_t width,
              std::uint32_t height, std::vector<std::uint8_t>& pixels) {
    // Photos are opaque: alpha off so viewers do not show them see-through.
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xff;
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(height, width * 4, UINT(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return false;
    }
    return true;
}

fs::path OutputFolder() {
    PWSTR pictures = nullptr;
    fs::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) {
        folder = fs::path{pictures} / L"Gran Turismo Sport";
    }
    CoTaskMemFree(pictures);
    return folder;
}

fs::path UniqueName(const fs::path& folder, const fs::file_time_type& when) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(when);
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
    localtime_s(&local, &t);
    wchar_t stamp[64];
    std::wcsftime(stamp, 64, L"GT Sport %Y-%m-%d %H-%M-%S", &local);
    fs::path path = folder / (std::wstring{stamp} + L".png");
    for (int n = 2; fs::exists(path); ++n) {
        path = folder / (std::wstring{stamp} + L" (" + std::to_wstring(n) + L").png");
    }
    return path;
}

bool IsPhoto(const fs::path& path) {
    // The game's saved photos are "pht__<id>_<n>.jxr"; it writes them first under a temporary
    // name (001.jxr) and renames them, so only the final names are exported.
    const std::wstring name = path.filename().wstring();
    return path.extension() == L".jxr" && name.rfind(L"pht_", 0) == 0 &&
           name.find(L"_t.") == std::wstring::npos;
}

void Watch(fs::path source) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return;
    }
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "[patch] GTS-010: no Windows imaging codecs, photo export off\n");
        return;
    }
    const fs::path folder = OutputFolder();
    if (folder.empty()) {
        std::fprintf(stderr, "[patch] GTS-010: no Pictures folder, photo export off\n");
        return;
    }
    std::fprintf(stderr, "[patch] GTS-010 photo export to %ls\n", folder.c_str());
    std::set<fs::path> done;
    std::map<fs::path, std::uintmax_t> growing; // size at the last look
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(source, ec)) {
        done.insert(entry.path()); // already there at start: not exported
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        for (const auto& entry : fs::directory_iterator(source, ec)) {
            const fs::path path = entry.path();
            if (done.contains(path) || !IsPhoto(path)) {
                continue;
            }
            const std::uintmax_t size = fs::file_size(path, ec);
            if (ec || size == 0) {
                continue;
            }
            // Written completely once its size holds for one look.
            auto last = growing.find(path);
            if (last == growing.end() || last->second != size) {
                growing[path] = size;
                continue;
            }
            growing.erase(path);
            done.insert(path);
            std::vector<std::uint8_t> data(size);
            if (std::FILE* f = _wfopen(path.c_str(), L"rb")) {
                data.resize(std::fread(data.data(), 1, data.size(), f));
                std::fclose(f);
            }
            std::uint32_t width = 0, height = 0;
            std::vector<std::uint8_t> pixels;
            fs::create_directories(folder, ec);
            const fs::path target = UniqueName(folder, fs::last_write_time(path, ec));
            if (DecodePhoto(factory.p, data, width, height, pixels) &&
                WritePng(factory.p, target, width, height, pixels)) {
                std::fprintf(stderr, "[patch] GTS-010 photo %ls -> %ls (%ux%u)\n",
                             path.filename().c_str(), target.filename().c_str(), width, height);
            } else {
                std::fprintf(stderr, "[patch] GTS-010 photo %ls could not be converted\n",
                             path.filename().c_str());
            }
        }
    }
}

} // namespace

void InstallPhotoExport() {
    if (const char* env = std::getenv("RYGT_PHOTO_EXPORT"); env && env[0] == '0') {
        return;
    }
    if (!ConfigFlag("photo_export", true)) {
        return;
    }
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    const fs::path source =
        fs::path{exe}.parent_path() / L"download0" / L"APP_DATA" / L"photo";
    std::thread(Watch, source).detach();
}
