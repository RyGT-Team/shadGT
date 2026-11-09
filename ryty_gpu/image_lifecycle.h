// Opt-in bounded CPU-side lifecycle spans; no GPU queries or readbacks.
#pragma once
#include <cstdint>
#include <type_traits>
namespace ImageLife {
template<class T> std::uint64_t Handle(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<std::uintptr_t>(value);
    else return static_cast<std::uint64_t>(value);
}
}
#if defined(_WIN32) && defined(RYTY_GPU)
namespace ImageLife {
void Initialize();
bool Enabled(const char* kind);
bool ContentProbeEnabled();
bool AliasProbeEnabled();
std::uint64_t Timestamp();
std::uint64_t SlowThresholdTicks();
std::uint64_t Begin(const char* kind, std::uint64_t a=0, std::uint64_t b=0,
                    std::uint64_t c=0, std::uint64_t d=0, std::uint64_t e=0);
void End(std::uint64_t id, const char* kind, std::uint64_t a=0, std::uint64_t b=0);
std::uint64_t SlowStart(const char* kind = nullptr);
void SlowEnd(std::uint64_t start, const char* kind, std::uint64_t address, std::uint64_t size);
struct SlowScope {
    const char* kind; std::uint64_t start, address, size;
    explicit SlowScope(const char* name, std::uint64_t a=0, std::uint64_t b=0, std::uint64_t=0)
        : kind(name), start(SlowStart(name)), address(a), size(b) {}
    ~SlowScope() { SlowEnd(start,kind,address,size); }
    void Finish() { if(start) { SlowEnd(start,kind,address,size); start=0; } }
};
struct Scope {
    const char* kind;
    std::uint64_t id;
    explicit Scope(const char* name, std::uint64_t a=0, std::uint64_t b=0,
                   std::uint64_t c=0, std::uint64_t d=0, std::uint64_t e=0)
        : kind(name), id(Begin(name,a,b,c,d,e)) {}
    explicit Scope(bool enabled, const char* name, std::uint64_t a=0, std::uint64_t b=0,
                   std::uint64_t c=0, std::uint64_t d=0, std::uint64_t e=0)
        : kind(name), id(enabled ? Begin(name,a,b,c,d,e) : 0) {}
    ~Scope() { End(id,kind); }
    bool Active() const { return id != 0; }
    std::uint64_t Id() const { return id; }
    void Finish(std::uint64_t a=0, std::uint64_t b=0) { if(id) { End(id,kind,a,b); id=0; } }
};
}
#else
namespace ImageLife {
inline void Initialize() {}
inline bool Enabled(const char*) { return false; }
inline bool ContentProbeEnabled() { return false; }
inline bool AliasProbeEnabled() { return false; }
inline std::uint64_t Timestamp() { return 0; }
inline std::uint64_t SlowThresholdTicks() { return 0; }
inline std::uint64_t Begin(const char*,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0) { return 0; }
inline void End(std::uint64_t,const char*,std::uint64_t=0,std::uint64_t=0) {}
struct SlowScope { explicit SlowScope(const char*,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0) {} void Finish() {} };
struct Scope { explicit Scope(const char*,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0) {} explicit Scope(bool,const char*,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0,std::uint64_t=0) {} bool Active() const { return false; } std::uint64_t Id() const { return 0; } void Finish(std::uint64_t=0,std::uint64_t=0) {} };
}
#endif

