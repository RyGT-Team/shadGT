#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Native::Parallel {
// Test-only control: exactly one ASCII byte, 0 or 1. I/O occurs only here.
inline std::optional<bool> ParseMode(const std::string& s) noexcept {
    if(s.size()!=1 || (s[0]!='0' && s[0]!='1')) return std::nullopt;
    return s[0]=='1';
}
inline bool InitialMode(const char* mode_file,const char* initial) noexcept {
    return !(mode_file && *mode_file && initial && initial[0]=='0' && !initial[1]);
}
struct ModeRequest {std::uint64_t generation;bool enabled;};
class ModeController {
    std::filesystem::path path_;std::atomic<std::uint64_t> request_;
    std::mutex mutex_;std::condition_variable wake_;bool stopping_{};std::thread thread_;
    void Run() noexcept {
        for(;;) {
            try {
                std::ifstream file(path_,std::ios::binary);char data[2]{};
                if(file){file.read(data,2);const auto n=file.gcount();auto parsed=ParseMode(std::string(data,static_cast<std::size_t>(n)));
                    if(parsed){auto old=request_.load(std::memory_order_relaxed);if(bool(old&1)!=*parsed)request_.store(((old>>1)+1)*2+*parsed,std::memory_order_release);}}
            } catch(...) {}
            std::unique_lock lock(mutex_);if(wake_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping_;}))return;
        }
    }
public:
    explicit ModeController(std::filesystem::path path,bool initial):path_(std::move(path)),request_(initial?1:0),thread_([this]{Run();}){}
    ~ModeController(){Stop();}
    ModeController(const ModeController&)=delete;ModeController& operator=(const ModeController&)=delete;
    void Stop() noexcept {{std::lock_guard lock(mutex_);stopping_=true;}wake_.notify_one();if(thread_.joinable())thread_.join();}
    ModeRequest Requested() const noexcept {auto r=request_.load(std::memory_order_acquire);return {r>>1,bool(r&1)};}
};
// Recorder alone calls Apply only at a fully drained submission boundary.
class AppliedMode {
    ModeRequest applied_;
public:
    explicit AppliedMode(bool initial):applied_{0,initial}{}
    std::optional<ModeRequest> Apply(ModeRequest request,bool safe_boundary) noexcept {
        if(!safe_boundary || request.generation<=applied_.generation)return std::nullopt;
        applied_=request;return applied_;
    }
    bool Enabled()const noexcept{return applied_.enabled;}
};
}
