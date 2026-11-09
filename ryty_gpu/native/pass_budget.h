#pragma once
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <algorithm>
// Prototype credit lifetime: callers retain the returned token across CPU queue,
// recording, submission and GPU retirement. Only release after completed fence
// or before submission after cancellation. Caller must charge every owned byte.
class PassBudget {
    struct State {std::mutex mutex;std::size_t count=0,bytes=0,max_count,max_bytes,peak_count=0,peak_bytes=0;State(std::size_t c,std::size_t b):max_count(c),max_bytes(b){}};
    std::shared_ptr<State> state_;
public:
    enum class Rejection { None, Count, Bytes };
    struct Snapshot { std::size_t count,bytes,max_count,max_bytes,peak_count,peak_bytes; };
private:
    Rejection CheckLocked(std::size_t bytes) const noexcept {
        if(state_->count>=state_->max_count)return Rejection::Count;
        if(bytes>state_->max_bytes-state_->bytes)return Rejection::Bytes;
        return Rejection::None;
    }
public:
    class Token {
        std::shared_ptr<State> state_;std::size_t bytes_{};
        friend class PassBudget;
        Token(std::shared_ptr<State> state,std::size_t bytes):state_(std::move(state)),bytes_(bytes){}
    public:
        Token(const Token&)=delete;Token& operator=(const Token&)=delete;
        Token(Token&& other) noexcept:state_(std::move(other.state_)),bytes_(other.bytes_){}
        Token& operator=(Token&&)=delete;
        ~Token(){if(state_){std::lock_guard lock(state_->mutex);--state_->count;state_->bytes-=bytes_;}}
    };
    PassBudget(std::size_t count,std::size_t bytes):state_(std::make_shared<State>(count,bytes)){}
    Rejection CheckAcquire(std::size_t bytes) const {std::lock_guard lock(state_->mutex);return CheckLocked(bytes);}
    std::optional<Token> TryAcquire(std::size_t bytes, Rejection* rejection=nullptr) {
        std::lock_guard lock(state_->mutex);
        const auto why=CheckLocked(bytes);if(rejection)*rejection=why;if(why!=Rejection::None)return std::nullopt;
        ++state_->count;state_->bytes+=bytes;
        state_->peak_count=(std::max)(state_->peak_count,state_->count);state_->peak_bytes=(std::max)(state_->peak_bytes,state_->bytes);
        return Token(state_,bytes);
    }
    Snapshot TakeSnapshot() noexcept {std::lock_guard lock(state_->mutex);auto result=Snapshot{state_->count,state_->bytes,state_->max_count,state_->max_bytes,state_->peak_count,state_->peak_bytes};state_->peak_count=state_->count;state_->peak_bytes=state_->bytes;return result;}
};
