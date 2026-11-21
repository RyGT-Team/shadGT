// GTS-010: photo export. GT Sport saves Scapes and photo mode pictures to
// download0\APP_DATA\photo as "PJXR" files: a header with the picture size, a tile grid (3x3) and
// each tile's offset, every tile an ordinary JPEG XR image. A background thread watches that
// folder and writes each new photo as a PNG to Photos beside the game, decoded with Windows'
// own JPEG XR codec (WIC). Existing photos and "_t" previews are left in the gallery.
// On unless SHADGT_PHOTO_EXPORT=0 or "photo_export": false in the game's config.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <share.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/game_title.h"

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
            FAILED(factory->CreateFormatConverter(&converter))) {
            return false;
        }
        // GTS-010b: Scapes and photo mode save HDR tiles (48bppRGBHalf, linear scRGB, highlights
        // far above 1.0). A plain conversion to 8 bits clips them; those are read as floats and
        // tone-mapped with the measured photo-mode curve below.
        WICPixelFormatGUID source_format{};
        frame->GetPixelFormat(&source_format);
        const bool hdr = IsEqualGUID(source_format, GUID_WICPixelFormat48bppRGBHalf) ||
                         IsEqualGUID(source_format, GUID_WICPixelFormat64bppRGBAHalf) ||
                         IsEqualGUID(source_format, GUID_WICPixelFormat64bppRGBHalf) ||
                         IsEqualGUID(source_format, GUID_WICPixelFormat96bppRGBFloat) ||
                         IsEqualGUID(source_format, GUID_WICPixelFormat128bppRGBAFloat) ||
                         IsEqualGUID(source_format, GUID_WICPixelFormat128bppRGBFloat);
        if (FAILED(converter->Initialize(frame.p,
                                         hdr ? GUID_WICPixelFormat128bppRGBAFloat
                                             : GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) {
            return false;
        }
        UINT tile_w = 0, tile_h = 0;
        if (FAILED(converter->GetSize(&tile_w, &tile_h)) || tile_w == 0 || tile_h == 0 ||
            tile_w > width || tile_h > height ||
            std::uint64_t(tile_w) * tile_h > UINT_MAX / (hdr ? 16u : 4u)) {
            return false;
        }
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
        if (hdr) {
            std::vector<float> linear(size_t(tile_w) * tile_h * 4);
            if (FAILED(converter->CopyPixels(&rect, tile_w * 16, UINT(linear.size() * 4),
                                             reinterpret_cast<BYTE*>(linear.data())))) {
                return false;
            }
            // The game's own photo-mode tone curve, measured from its live view against the
            // saved HDR data of the same frame: 8-bit output at log2(linear) = -7.8 + 0.4 * i.
            static constexpr float curve[] = {1,   2,   3,   4,   7,   10,  14,  19,  25,
                                              33,  41,  50,  61,  73,  85,  96,  108, 123,
                                              140, 159, 184, 203, 222, 233, 241, 249, 252,
                                              253, 254, 254, 254, 254, 255};
            constexpr int points = int(sizeof(curve) / sizeof(curve[0]));
            const auto encode = [](float v) -> std::uint8_t {
                if (!(v > 0.0f)) return 0; // also NaN
                const float at = (std::log2(v) + 7.8f) / 0.4f;
                if (at <= 0.0f) return std::uint8_t(curve[0] * v / 0.0045f + 0.5f);
                if (at >= points - 1) return 255;
                const int i = int(at);
                const float t = at - float(i);
                return std::uint8_t(curve[i] + (curve[i + 1] - curve[i]) * t + 0.5f);
            };
            for (size_t p = 0; p < size_t(tile_w) * tile_h; ++p) {
                tile[p * 4 + 0] = encode(linear[p * 4 + 2]); // B
                tile[p * 4 + 1] = encode(linear[p * 4 + 1]); // G
                tile[p * 4 + 2] = encode(linear[p * 4 + 0]); // R
                tile[p * 4 + 3] = 0xff;
            }
        } else if (FAILED(converter->CopyPixels(&rect, tile_w * 4, UINT(tile.size()),
                                                  tile.data()))) {
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

// Publish a complete PNG in the same Photos folder as the gallery's native export.
// MoveFileW cannot overwrite a picture, including one exported by another process.
bool PublishPng(IWICImagingFactory* factory, const fs::path& folder, fs::file_time_type when,
                UINT width, UINT height, std::vector<std::uint8_t>& pixels, fs::path& target) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(when);
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
    wchar_t stamp[32]{};
    if (localtime_s(&local, &t) != 0 ||
        !std::wcsftime(stamp, std::size(stamp), L"%Y-%m-%d %H-%M-%S", &local)) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    wchar_t temp[MAX_PATH]{};
    if (ec || !GetTempFileNameW(folder.c_str(), L"pht", 0, temp)) return false;
    if (WritePng(factory, temp, width, height, pixels)) {
        for (unsigned copy = 1; copy <= 999; ++copy) {
            std::wstring stem{stamp};
            if (copy > 1) stem += L" (" + std::to_wstring(copy) + L")";
            target = folder / (stem + L".png");
            if (MoveFileW(temp, target.c_str())) return true;
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) break;
        }
    }
    DeleteFileW(temp);
    return false;
}

bool IsPhoto(const fs::path& path) {
    // The game's saved photos are "pht__<id>_<n>.jxr"; it writes them first under a temporary
    // name (001.jxr) and renames them, so only the final names are exported.
    const std::wstring name = path.filename().wstring();
    return path.extension() == L".jxr" && name.rfind(L"pht_", 0) == 0 &&
           name.find(L"_t.") == std::wstring::npos;
}

struct PhotoRevision {
    std::uintmax_t size;
    fs::file_time_type modified;
    bool operator==(const PhotoRevision&) const = default;
};

bool ExportPhoto(IWICImagingFactory* factory, const fs::path& path, const PhotoRevision& revision,
                 const fs::path& folder) {
    // The final names normally arrive by rename. Refuse a writer still holding the file.
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
        _wfsopen(path.c_str(), L"rb", _SH_DENYWR), std::fclose);
    if (!file || revision.size == 0 || revision.size > UINT32_MAX) return false;
    std::vector<std::uint8_t> data(revision.size);
    if (std::fread(data.data(), 1, data.size(), file.get()) != data.size() ||
        std::fgetc(file.get()) != EOF || std::ferror(file.get())) return false;
    file.reset();
    std::vector<std::uint8_t> pixels;
    UINT width = 0, height = 0;
    fs::path target;
    if (!DecodePhoto(factory, data, width, height, pixels) ||
        !PublishPng(factory, folder, revision.modified, width, height, pixels, target)) return false;
    std::fprintf(stderr, "[patch] GTS-010 photo %ls -> %ls (%ux%u)\n",
                 path.filename().c_str(), target.c_str(), width, height);
    return true;
}

void Watch(fs::path root, std::string serial) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return;
    }
    Com<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "[patch] GTS-010: no Windows imaging codecs, photo export off\n");
        return;
    }
    const fs::path folder = root / L"Photos";
    std::fprintf(stderr, "[patch] GTS-010 photo export to %ls\n", folder.c_str());
    // FIX-SGT-049: libc can keep the legacy download0 on a mixed install or after a
    // failed migration. Observe both possible stores, including ones created after boot.
    const fs::path photos = fs::path(L"download0") / L"APP_DATA" / L"photo";
    const std::array sources{root / L"user" / L"savedata" / serial / photos, root / photos};
    using Clock = std::chrono::steady_clock;
    struct Status {
        PhotoRevision revision;
        Clock::time_point next;
        std::chrono::seconds delay{2};
    };
    std::map<fs::path, Status> seen;
    bool startup = true;
    for (;;) {
        for (const auto& source : sources) {
            std::error_code ec;
            for (fs::directory_iterator it(source, ec), end; it != end; it.increment(ec)) {
                const fs::path& path = it->path();
                if (!IsPhoto(path) || !it->is_regular_file(ec)) continue;
                const auto size = it->file_size(ec);
                if (ec) continue;
                const auto modified = it->last_write_time(ec);
                if (ec) continue;
                const PhotoRevision revision{size, modified};
                const auto now = Clock::now();
                const Status initial{revision, startup ? Clock::time_point::max() : now};
                auto& status = seen.try_emplace(path, initial).first->second;
                if (status.revision != revision) status = initial;
                if (now < status.next) continue;
                bool exported = false;
                try {
                    exported = ExportPhoto(factory.p, path, revision, folder);
                } catch (const std::bad_alloc&) {
                    // Leave the original in the gallery and try again when memory is available.
                }
                if (exported) {
                    status.next = Clock::time_point::max();
                } else {
                    if (status.delay == std::chrono::seconds(2)) {
                        std::fprintf(stderr, "[patch] photo %ls: export failed, will retry\n",
                                     path.filename().c_str());
                    }
                    status.next = Clock::now() + status.delay;
                    status.delay = std::min(status.delay * 2, std::chrono::seconds(60));
                }
            }
        }
        startup = false;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

} // namespace

void InstallPhotoExport() {
    if (const char* env = std::getenv("SHADGT_PHOTO_EXPORT"); env && env[0] == '0') {
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
    std::thread(Watch, fs::path{exe}.parent_path(), std::string(Common::CurrentGameSerial())).detach();
}
