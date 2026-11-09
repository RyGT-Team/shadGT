#include "common/lru_cache.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

struct Image : Common::LRUNode<> {
    struct Info {
        bool tiled{};
        bool IsTiled() const { return tiled; }
    } info;
    bool download{};
    bool freed{};
    bool downloaded{};
    bool SafeToDownload() const { return download; }
};

struct TextureCache {
    struct Instance {
        bool CanReportMemoryUsage() const { return false; }
        u64 GetDeviceMemoryUsage() const { return 0; }
    } instance;
    struct Slots {
        Image* GetSlotId(Image& image) { return &image; }
    } slot_images;
    u64 total_used_memory = 1000;
    u64 trigger_gc_memory = 100;
    u64 pressure_gc_memory = 500;
    u64 critical_gc_memory = 2000;
    u64 gc_tick = 1000;
    unsigned freed{}, downloads{};
    Common::LRUCache<Image> image_lru_cache;
    std::vector<std::unique_ptr<Image>> images;

    void Add(unsigned count, bool download, bool tiled, u64 tick = 1) {
        for (unsigned i = 0; i < count; ++i) {
            auto image = std::make_unique<Image>();
            image->download = download;
            image->info.tiled = tiled;
            image_lru_cache.Insert(*image, tick);
            images.push_back(std::move(image));
        }
    }
    void DownloadImageMemory(Image* image) {
        if (image->info.tiled) throw std::runtime_error("unsupported tiled readback");
        image->downloaded = true;
        ++downloads;
    }
    void FreeImage(Image* image) {
        if (image->download && !image->downloaded)
            throw std::runtime_error("lost the only GPU-written copy");
        image_lru_cache.Free(*image);
        image->freed = true;
        ++freed;
    }
    void GarbageCollectImages();
};

#include "gc-function-under-test.inc"

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RunTests() {
    TextureCache pressure;
    pressure.Add(65, true, true);
    pressure.Add(30, false, true);
    pressure.GarbageCollectImages();
    Check(pressure.freed == 20, "protected old textures must not starve reclaimable textures");
    for (unsigned i = 0; i < 65; ++i)
        Check(!pressure.images[i]->freed, "GPU-written tiled textures must be preserved");

    TextureCache normal;
    normal.total_used_memory = 400;
    normal.Add(65, true, false);
    normal.Add(30, false, false);
    normal.GarbageCollectImages();
    Check(normal.freed == 10 && normal.downloads == 0,
          "normal cleanup must reach reloadable textures without forcing readbacks");

    TextureCache critical;
    critical.total_used_memory = 3000;
    critical.Add(65, true, true);
    critical.Add(100, false, false);
    critical.GarbageCollectImages();
    Check(critical.freed == 60, "pressure and critical passes must retain their deletion limits");

    TextureCache linear;
    linear.Add(3, true, false);
    linear.GarbageCollectImages();
    Check(linear.freed == 3 && linear.downloads == 3,
          "eligible GPU-written linear images need readback before deletion");

    TextureCache recent;
    recent.total_used_memory = 400;
    recent.Add(10, false, false, 995);
    recent.GarbageCollectImages();
    Check(recent.freed == 0, "recently used textures must keep their age protection");

    TextureCache low;
    low.total_used_memory = 50;
    low.Add(10, false, false);
    low.GarbageCollectImages();
    Check(low.freed == 0, "no pressure must preserve the existing trigger threshold");

    TextureCache worst;
    worst.Add(65500, true, true);
    worst.Add(30, false, false);
    const auto start = std::chrono::steady_clock::now();
    worst.GarbageCollectImages();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    Check(worst.freed == 20, "a full protected prefix must not consume the deletion quota");
    std::cout << "PASS: actual collector function, protected prefix starvation, deletion limits, "
                 "preserved GPU data, readbacks, recent textures and trigger threshold; "
              << "65530-entry synthetic scan " << elapsed << " us\n";
}

int main() {
    try {
        RunTests();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
