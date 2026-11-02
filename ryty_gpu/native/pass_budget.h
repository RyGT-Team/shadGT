#pragma once
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
// Prototype credit lifetime: callers retain the returned token across CPU queue,
// recording, submission and GPU retirement. Only release after completed fence
// or before submission after cancellation. Caller must charge every owned byte.
class PassBudget {
    struct State {std::mutex mutex;std::size_t count=0,bytes=0,max_count,max_bytes;State(std::size_t c,std::size_t b):max_count(c),max_bytes(b){}};
    std::shared_ptr<State> state_;
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
    std::optional<Token> TryAcquire(std::size_t bytes) {
        std::lock_guard lock(state_->mutex);
        if(state_->count>=state_->max_count || bytes>state_->max_bytes-state_->bytes)return std::nullopt;
        ++state_->count;state_->bytes+=bytes;return Token(state_,bytes);
    }
};
