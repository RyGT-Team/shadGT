// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression for the production executable-storage helper.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed: %s at line %d\n", #x, __LINE__); std::abort(); } } while (false)
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include <xbyak/xbyak.h>
#include "shader_recompiler/ir/passes/srt_walker_storage.h"

using Storage = Shader::SrtWalkerStorage<4096, 2>;
using Fn = int (*)();

static std::vector<std::uint8_t> Emit(int value, std::size_t length = 64) {
    Xbyak::CodeGenerator assembler(length);
    assembler.mov(assembler.eax, value);
    assembler.ret();
    while (assembler.getSize() < length) assembler.nop();
    return {assembler.getCode(), assembler.getCode() + assembler.getSize()};
}
static int Run(const std::shared_ptr<Storage::WalkerLease>& lease) {
    return reinterpret_cast<Fn>(const_cast<std::uint8_t*>(lease->Code()))();
}
int main() {
    Storage storage;
    auto first_code = Emit(17);
    auto first = storage.Publish(first_code.data(), first_code.size());
    const auto first_address = first->Code();
    auto copied = first;
    auto moved = std::move(first);
    CHECK(!first && Run(copied) == 17 && storage.Contains(first_address));
    moved.reset();
    CHECK(storage.Contains(first_address) && Run(copied) == 17);
    copied.reset();
    CHECK(!storage.Contains(first_address));

    auto keeper = storage.Publish(first_code.data(), first_code.size());
    std::vector<std::shared_ptr<Storage::WalkerLease>> live;
    auto second_code = Emit(29);
    for (int i = 0; i < 127; ++i) live.push_back(storage.Publish(second_code.data(), second_code.size()));
    CHECK(Run(keeper) == 17 && Run(live.back()) == 29);
    CHECK(storage.Contains(live.back()->Code()));
    bool exhausted = false;
    try { storage.Publish(second_code.data(), second_code.size()); }
    catch (const Common::RecoverableFailure&) { exhausted = true; }
    CHECK(exhausted && Run(keeper) == 17);
    auto recycled_address = live[40]->Code();
    live[40].reset();
    CHECK(!storage.Contains(recycled_address));
    auto replacement = storage.Publish(first_code.data(), first_code.size());
    CHECK(Run(replacement) == 17);
    live.clear(); replacement.reset(); keeper.reset();
    auto oversized = Emit(71, 8193);
    bool rejected = false;
    try { storage.Publish(oversized.data(), oversized.size()); }
    catch (const Common::RecoverableFailure&) { rejected = true; }
    CHECK(rejected);
    for (int i = 0; i < 1000; ++i) {
        auto tmp = storage.Publish(second_code.data(), second_code.size());
        CHECK(Run(tmp) == 29);
    }
    std::atomic<bool> good{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        auto code = Emit(100 + t);
        for (int i = 0; i < 1000; ++i) {
            auto lease = storage.Publish(code.data(), code.size());
            if (!storage.Contains(lease->Code()) || Run(lease) != 100 + t) good = false;
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(good);

    // A live run crossing bitmap words must remain fully visible to the handler.
    Shader::SrtWalkerStorage<8192, 2> boundary;
    auto prefix_code = Emit(3, 63 * 64);
    auto prefix = boundary.Publish(prefix_code.data(), prefix_code.size());
    auto cross_code = Emit(4, 2 * 64);
    auto cross = boundary.Publish(cross_code.data(), cross_code.size());
    CHECK(boundary.Contains(prefix->Code() + 63 * 64 - 1));
    CHECK(boundary.Contains(cross->Code()));
    CHECK(boundary.Contains(cross->Code() + cross_code.size() - 1));
    CHECK(boundary.Available(cross->Code()) == cross_code.size());
    CHECK(boundary.Available(cross->Code() + 63) == cross_code.size() - 63);
    CHECK(boundary.Available(cross->Code() + 64) == 64);
    CHECK(boundary.Available(cross->Code() + 127) == 1);
    CHECK(boundary.Available(cross->Code() + 128) == 0);
    CHECK(reinterpret_cast<Fn>(const_cast<std::uint8_t*>(prefix->Code()))() == 3);
    CHECK(reinterpret_cast<Fn>(const_cast<std::uint8_t*>(cross->Code()))() == 4);
    cross.reset();

    // A code object larger than one nominal segment gets a single larger allocation.
    Shader::SrtWalkerStorage<4096, 3> large;
    auto large_code = Emit(81, 4097);
    auto large_lease = large.Publish(large_code.data(), large_code.size());
    CHECK(large.Contains(large_lease->Code() + 4096));
    CHECK(large.Available(large_lease->Code() + 4096) == 1);
    CHECK(!large.Contains(large_lease->Code() + 4097)); // allocated slot padding
    CHECK(!large.Contains(large_lease->Code() + 64 * 65));
    CHECK(reinterpret_cast<Fn>(const_cast<std::uint8_t*>(large_lease->Code()))() == 81);

    // FIX-SGT-094: a limit below the compiled segment count is kept, in whole segments.
    using Limited = Shader::SrtWalkerStorage<4096, 4>;
    CHECK(Limited{1}.Limit() == 4096);
    CHECK(Limited{3 * 4096 + 100}.Limit() == 3 * 4096);
    CHECK(Limited{std::size_t{1} << 30}.Limit() == 4 * 4096);
    Limited limited{4096};
    CHECK(limited.MayFit(64) && limited.MayFit(4096) && !limited.MayFit(4097) && !limited.AtWall());
    const auto refusal = [](auto& storage, const std::vector<std::uint8_t>& code) -> std::size_t {
        try { storage.Publish(code.data(), code.size()); }
        catch (const Shader::SrtStorageFull& full) { return full.bytes; }
        return 0;
    };
    std::vector<std::shared_ptr<Limited::WalkerLease>> slots;
    auto slot_code = Emit(5, 64);
    for (int i = 0; i < 64; ++i) slots.push_back(limited.Publish(slot_code.data(), slot_code.size()));
    const auto* limited_base = slots[0]->Code();
    for (int i = 0; i < 64; ++i) CHECK(slots[i]->Code() == limited_base + i * 64);
    CHECK(limited.Reserved() == 4096 && !limited.MayFit(64));
    // The one segment the limit allows is full: refused, with the walker's size.
    CHECK(refusal(limited, slot_code) == 64);
    CHECK(limited.AtWall() && !limited.MayFit(64) && limited.Stats().failures == 1);
    CHECK(limited.Stats().reserved == 4096 && limited.Stats().limit == 4096);
    // A second refusal looks at the segments only, not at their slots.
    const auto scanned = limited.Stats().scanned;
    CHECK(refusal(limited, slot_code) == 64 && limited.Stats().scanned == scanned);
    // Every other slot released: 32 free, none side by side. A two-slot walker is refused unscanned.
    for (int i = 0; i < 64; i += 2) slots[i].reset();
    CHECK(!limited.AtWall() && limited.MayFit(64) && !limited.MayFit(65) && !limited.MayFit(128));
    auto two_slot_code = Emit(6, 128);
    CHECK(refusal(limited, two_slot_code) == 128 && limited.Stats().scanned == scanned);
    CHECK(limited.AtWall());
    // Slot 1 released between free slots 0 and 2: a run of three.
    slots[1].reset();
    CHECK(!limited.AtWall() && limited.MayFit(3 * 64) && !limited.MayFit(3 * 64 + 1));
    auto three_slot_code = Emit(7, 3 * 64);
    auto three = limited.Publish(three_slot_code.data(), three_slot_code.size());
    CHECK(three->Code() == limited_base && limited.Available(three->Code()) == 3 * 64);
    CHECK(reinterpret_cast<Fn>(const_cast<std::uint8_t*>(three->Code()))() == 7);
    slots.clear();
    three.reset();
    CHECK(limited.MayFit(4096) && !limited.MayFit(4097));
    CHECK(limited.Publish(slot_code.data(), slot_code.size()) != nullptr);

    // Released slots join their free neighbours across a bitmap word (slots 63 and 64).
    using Wide = Shader::SrtWalkerStorage<8192, 1>;
    Wide wide;
    std::vector<std::shared_ptr<Wide::WalkerLease>> row;
    for (int i = 0; i < 128; ++i) row.push_back(wide.Publish(slot_code.data(), slot_code.size()));
    for (int i = 0; i < 128; ++i) CHECK(row[i]->Code() == row[0]->Code() + i * 64);
    CHECK(refusal(wide, slot_code) == 64 && !wide.MayFit(64));
    row[62].reset();
    row[63].reset();
    row[65].reset();
    CHECK(wide.MayFit(2 * 64) && !wide.MayFit(3 * 64));
    row[64].reset();
    CHECK(wide.MayFit(4 * 64) && !wide.MayFit(5 * 64));
    auto four_slot_code = Emit(8, 4 * 64);
    auto four = wide.Publish(four_slot_code.data(), four_slot_code.size());
    CHECK(four->Code() == row[0]->Code() + 62 * 64 && wide.Available(four->Code()) == 4 * 64);
    CHECK(wide.Contains(four->Code() + 4 * 64 - 1) && wide.Contains(row[66]->Code()));
    CHECK(reinterpret_cast<Fn>(const_cast<std::uint8_t*>(four->Code()))() == 8);
    // The last free slot taken, the next one is refused without a scan.
    CHECK(refusal(wide, slot_code) == 64);
    const auto wide_scanned = wide.Stats().scanned;
    CHECK(refusal(wide, slot_code) == 64 && wide.Stats().scanned == wide_scanned);
    std::printf("srt_walker_storage: PASS\n");
}
