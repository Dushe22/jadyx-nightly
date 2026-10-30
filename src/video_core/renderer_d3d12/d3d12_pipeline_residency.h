// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_memory_guard.h"

// Pure policy for which graphics PSOs stay resident: no device, file or clock, so every decision is
// covered by tools/xbox/tests/pipeline-residency.cpp.
//
// A compiled PSO is the driver's: about 100 KiB each, against some 12 KiB for the pipeline's own
// bookkeeping and a share of its DXIL. A game that built every pipeline of its disk cache at boot
// (17,000 for one of them) spent 1.3 GiB on PSOs before its first frame. So a pipeline keeps its
// DXIL and only has a PSO while it is drawn: the ones used in recent sessions are built at boot,
// the rest when first drawn, and a PSO idle for long enough is released and built again from the
// kept DXIL if it is drawn again.
namespace D3D12 {

/// How long an idle PSO is kept, in frames, for the app's free memory: for good while there is
/// plenty, minutes while there is room, seconds as the memory guard's thresholds near.
///
/// Releasing an idle PSO saves some 80 KiB and costs a driver compile (about 50 ms on the Series,
/// a stall when drawn again); with gigabytes free that trade only loses, so eviction waits until
/// free memory falls under PLENTY_FREE. The background prewarm of dormant PSOs stops at the same
/// line, so the two never undo each other's work.
struct PipelineResidencyPolicy {
    static constexpr std::uint64_t KEEP_IDLE_FRAMES = ~std::uint64_t{0}; ///< Never released.
    static constexpr std::uint64_t IDLE_FRAMES = 3 * 60 * 60;      ///< Three minutes at 60 FPS.
    static constexpr std::uint64_t LOW_IDLE_FRAMES = 10 * 60;      ///< Ten seconds.
    static constexpr std::uint64_t EMERGENCY_IDLE_FRAMES = 60;     ///< One second.
    static constexpr std::size_t SWEEP_PER_FRAME = 256;            ///< Pipelines checked a frame.
    /// 1.5 GiB: some 19,000 PSOs, more than the largest disk cache seen builds after dedup.
    static constexpr std::uint64_t PLENTY_FREE = 1536 * MemoryGuardPolicy::MiB;

    /// limit 0 is an unknown limit: as with the memory guard, it never implies pressure, but it
    /// is not known to be plenty either, so idle PSOs keep the minutes limit.
    [[nodiscard]] static constexpr std::uint64_t IdleFrames(std::uint64_t used,
                                                            std::uint64_t limit) noexcept {
        if (limit == 0) {
            return IDLE_FRAMES;
        }
        const std::uint64_t free = used >= limit ? 0 : limit - used;
        if (free <= MemoryGuardPolicy::EMERGENCY_FREE) {
            return EMERGENCY_IDLE_FRAMES;
        }
        if (free <= MemoryGuardPolicy::LOW_FREE) {
            return LOW_IDLE_FRAMES;
        }
        if (free > PLENTY_FREE) {
            return KEEP_IDLE_FRAMES;
        }
        return IDLE_FRAMES;
    }

    /// Whether a dormant PSO may be built ahead of its first draw: only with plenty free, and
    /// never with an unknown limit.
    [[nodiscard]] static constexpr bool MayPrewarm(std::uint64_t used,
                                                   std::uint64_t limit) noexcept {
        return limit != 0 && used < limit && limit - used > PLENTY_FREE;
    }
};

/// Admission of optional work (building pipelines ahead of their first draw) against the app's
/// memory: it never takes the last RESERVE free, it spends at most a session budget learned from
/// the last session's worst moment, it measures what each item really cost, and it yields to the
/// game's own draws. Nothing waits on it: a draw that needs a pipeline builds it as before.
///
/// The budget follows the previous session: what it spent plus what it still had free past the
/// reserve at its lowest (less, when it fell under the reserve). Pipelines stay once made, so a
/// session that spent X and bottomed out at M would have bottomed out at M + X without them.
class PrewarmAdmission {
public:
    static constexpr std::uint64_t KiB = 1024;
    static constexpr std::uint64_t MiB = 1024 * KiB;
    /// Free memory optional work leaves alone: the experiment's reserve (memory phase 0).
    static constexpr std::uint64_t RESERVE = 256 * MiB;
    /// Once stopped by memory, it starts again only this much above the line, so it does not
    /// flap at the edge while the game loads.
    static constexpr std::uint64_t RESUME_MARGIN = 64 * MiB;
    /// A pipeline's bookkeeping and DXIL (the PSO is the driver's, outside the app's commit on
    /// the Series), until measured.
    static constexpr std::uint64_t INITIAL_COST = 16 * KiB;
    static constexpr std::uint64_t MIN_COST = 4 * KiB;
    static constexpr std::uint64_t MAX_COST = 1 * MiB;
    /// With no last session to learn from.
    static constexpr std::uint64_t FIRST_SESSION_BUDGET = 16 * MiB;
    static constexpr std::uint64_t MAX_BUDGET = 256 * MiB;
    /// Frames without a pipeline made for a draw before optional work runs again: the game's
    /// draws come first, and new ones arrive in bursts as it enters an area.
    static constexpr std::uint64_t DEMAND_QUIET_FRAMES = 30;

    struct Learned {
        std::uint64_t min_free;
        std::uint64_t spent;
    };

    /// The session's budget from what the last one learned (none: the first session's).
    void Start(const Learned* last) noexcept {
        if (last == nullptr) {
            budget = FIRST_SESSION_BUDGET;
        } else {
            const std::uint64_t room = last->spent + last->min_free;
            budget = (std::min)(room > RESERVE ? room - RESERVE : 0, MAX_BUDGET);
        }
        spent = 0;
        min_free = UNKNOWN;
        paused = false;
        pauses = 0;
    }

    /// Every frame with the app's memory (limit 0: unknown, nothing learned).
    void Observe(std::uint64_t used, std::uint64_t limit) noexcept {
        if (limit != 0) {
            min_free = (std::min)(min_free, used < limit ? limit - used : 0);
        }
    }

    /// A pipeline was made for a draw this frame.
    void NoteDemand(std::uint64_t frame) noexcept {
        last_demand = frame;
        has_demand = true;
    }

    /// Whether one more item may start now. An unknown limit never admits.
    [[nodiscard]] bool MayStart(std::uint64_t frame, std::uint64_t used, std::uint64_t limit) noexcept {
        if (limit == 0 || (has_demand && frame - last_demand < DEMAND_QUIET_FRAMES) ||
            spent + cost > budget) {
            return false;
        }
        const std::uint64_t free = used < limit ? limit - used : 0;
        const std::uint64_t needed = RESERVE + cost + (paused ? RESUME_MARGIN : 0);
        if (free < needed) {
            pauses += paused ? 0 : 1;
            paused = true;
            return false;
        }
        paused = false;
        return true;
    }

    /// An admitted item ended; measured is what the memory ledger grew by meanwhile.
    void Finished(std::uint64_t measured) noexcept {
        const std::uint64_t sample = std::clamp(measured, MIN_COST, MAX_COST);
        cost = (3 * cost + sample) / 4;
        spent += sample;
    }

    /// What to keep for the next session; min_free stays at the limit when nothing was seen.
    [[nodiscard]] Learned Learn(std::uint64_t limit) const noexcept {
        return {min_free == UNKNOWN ? limit : min_free, spent};
    }

    [[nodiscard]] std::uint64_t Budget() const noexcept { return budget; }
    [[nodiscard]] std::uint64_t Spent() const noexcept { return spent; }
    [[nodiscard]] std::uint64_t Cost() const noexcept { return cost; }
    [[nodiscard]] std::uint64_t MinFree() const noexcept { return min_free; }
    [[nodiscard]] std::uint64_t Pauses() const noexcept { return pauses; }
    [[nodiscard]] bool Exhausted() const noexcept { return spent + cost > budget; }

private:
    static constexpr std::uint64_t UNKNOWN = ~std::uint64_t{0};
    std::uint64_t budget{};
    std::uint64_t spent{};
    std::uint64_t cost{INITIAL_COST};
    std::uint64_t min_free{UNKNOWN};
    std::uint64_t last_demand{};
    std::uint64_t pauses{};
    bool has_demand{};
    bool paused{};
};

/// Whether releasing idle PSOs under memory pressure gives commit back, measured on the first
/// releases: a few hundred are released, eviction pauses until their fences retire, and the drop
/// in the commit the ledger does not track (DXIL and bookkeeping are tracked) is split among them.
/// Below MIN_RELEASE_BYTES a PSO, pressure eviction stops for the session and idle PSOs are
/// released only in an emergency. On the Series the driver keeps PSOs outside the app's commit:
/// 2737 released at a time of pressure freed nothing and were compiled again mid-game, 24-34 s of
/// stalls a run. A measure at boot is no substitute: the compiles there leave transient heap
/// growth behind (+29 MiB one boot, +263 MiB the next for a similar number of PSOs).
class PipelineReleaseProbe {
public:
    static constexpr std::uint64_t PROBE_PSOS = 256;
    /// Frames for the released PSOs' fences to retire before the commit is read again.
    static constexpr std::uint64_t SETTLE_FRAMES = 120;
    /// A probe that has not released PROBE_PSOS by then starts over from a fresh baseline.
    static constexpr std::uint64_t MAX_FRAMES = 60 * 60;
    static constexpr std::uint64_t MIN_RELEASE_BYTES = 16 * 1024;

    enum class Verdict : std::uint8_t { Unknown, Pays, DoesNotPay };

    /// Called once a frame before the eviction sweep: the PSOs released so far and the commit
    /// the ledger does not track (untracked_known false when the commit is unknown).
    void Tick(std::uint64_t frame, std::uint64_t released, std::uint64_t untracked,
              bool untracked_known) noexcept {
        if (verdict != Verdict::Unknown || !untracked_known) {
            return;
        }
        if (settle_frame != 0) {
            if (frame - settle_frame < SETTLE_FRAMES) {
                return;
            }
            freed = baseline_untracked > untracked ? baseline_untracked - untracked : 0;
            probed = released - baseline_released;
            verdict = freed / probed >= MIN_RELEASE_BYTES ? Verdict::Pays : Verdict::DoesNotPay;
            return;
        }
        if (!has_baseline || released == baseline_released ||
            frame - baseline_frame > MAX_FRAMES) {
            // Nothing released since the baseline (or a stale probe): it moves up to this frame.
            has_baseline = true;
            baseline_untracked = untracked;
            baseline_released = released;
            baseline_frame = frame;
            return;
        }
        if (released - baseline_released >= PROBE_PSOS) {
            settle_frame = frame;
        }
    }

    /// Whether pressure eviction may release PSOs this frame; an emergency releases regardless.
    [[nodiscard]] bool MayRelease() const noexcept {
        return verdict == Verdict::Pays || (verdict == Verdict::Unknown && settle_frame == 0);
    }

    [[nodiscard]] Verdict GetVerdict() const noexcept {
        return verdict;
    }
    [[nodiscard]] std::uint64_t Freed() const noexcept {
        return freed;
    }
    [[nodiscard]] std::uint64_t Probed() const noexcept {
        return probed;
    }

private:
    Verdict verdict{Verdict::Unknown};
    bool has_baseline{};
    std::uint64_t baseline_untracked{};
    std::uint64_t baseline_released{};
    std::uint64_t baseline_frame{};
    std::uint64_t settle_frame{}; ///< 0 while releasing; frame numbers start at 1.
    std::uint64_t freed{};
    std::uint64_t probed{};
};

/// The pipelines drawn in a game's recent sessions, by key hash, persisted next to its disk
/// cache. Each session marks what it draws; a pipeline not drawn for KEEP_SESSIONS sessions is
/// forgotten, so the set follows where the player is instead of growing with the whole game.
class HotPipelineSet {
public:
    /// Short test launches count as sessions too: with 4, a few of them in a row forgot the areas
    /// played before, and their PSOs were built again mid-game. MAX_PREWARM still bounds boot.
    static constexpr std::uint32_t KEEP_SESSIONS = 16;
    /// Pipelines built at boot at most, most recently drawn first.
    static constexpr std::size_t MAX_PREWARM = 4096;
    /// With no history yet (first session, or a file of an older format), the first pipelines
    /// of the disk cache, which is in the order they were first seen: the start of the game.
    static constexpr std::size_t FIRST_SESSION_PREWARM = 2048;

    /// Starts a session from a saved file; an empty, damaged or foreign one starts with no history.
    void Load(std::span<const std::uint8_t> bytes) {
        last_used.clear();
        session = 1;
        if (bytes.size() < HEADER_SIZE || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0 ||
            Read32(bytes, 4) != VERSION) {
            return;
        }
        const std::uint32_t saved_session = Read32(bytes, 8);
        const std::uint32_t count = Read32(bytes, 12);
        if (bytes.size() != HEADER_SIZE + std::size_t{count} * ENTRY_SIZE) {
            return;
        }
        session = saved_session + 1;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t offset = HEADER_SIZE + std::size_t{i} * ENTRY_SIZE;
            const std::uint32_t entry_session = Read32(bytes, offset + 8);
            if (entry_session < session && session - entry_session <= KEEP_SESSIONS) {
                last_used.emplace(Read64(bytes, offset), entry_session);
            }
        }
        BuildPrewarm();
    }

    [[nodiscard]] bool HasHistory() const noexcept {
        return !last_used.empty();
    }

    /// Whether the pipeline should get its PSO at boot. file_index is its position in the disk
    /// cache, for the first session's fallback.
    [[nodiscard]] bool Prewarm(std::uint64_t hash, std::size_t file_index) const {
        if (!HasHistory()) {
            return file_index < FIRST_SESSION_PREWARM;
        }
        return prewarm.contains(hash);
    }

    /// Whether the pipeline was drawn in one of the kept sessions, built at boot or not.
    [[nodiscard]] bool Known(std::uint64_t hash) const {
        return last_used.contains(hash);
    }

    /// The last kept session that drew the pipeline, 0 when none did; higher is more recent.
    [[nodiscard]] std::uint32_t LastSession(std::uint64_t hash) const {
        const auto it = last_used.find(hash);
        return it != last_used.end() ? it->second : 0;
    }

    /// Marks a pipeline drawn this session; true the first time, when the set needs saving.
    bool MarkUsed(std::uint64_t hash) {
        auto [it, inserted] = last_used.try_emplace(hash, session);
        if (!inserted && it->second == session) {
            return false;
        }
        it->second = session;
        return true;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return last_used.size();
    }

    [[nodiscard]] std::size_t PrewarmSize() const noexcept {
        return prewarm.size();
    }

    [[nodiscard]] std::vector<std::uint8_t> Serialize() const {
        std::vector<std::uint8_t> bytes(HEADER_SIZE + last_used.size() * ENTRY_SIZE);
        std::memcpy(bytes.data(), MAGIC, sizeof(MAGIC));
        Write32(bytes, 4, VERSION);
        Write32(bytes, 8, session);
        Write32(bytes, 12, static_cast<std::uint32_t>(last_used.size()));
        std::size_t offset = HEADER_SIZE;
        for (const auto& [hash, entry_session] : last_used) {
            Write64(bytes, offset, hash);
            Write32(bytes, offset + 8, entry_session);
            offset += ENTRY_SIZE;
        }
        return bytes;
    }

private:
    static constexpr char MAGIC[4] = {'D', '3', 'H', 'P'};
    static constexpr std::uint32_t VERSION = 1;
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::size_t ENTRY_SIZE = 12;

    void BuildPrewarm() {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> by_recency;
        by_recency.reserve(last_used.size());
        for (const auto& [hash, entry_session] : last_used) {
            by_recency.emplace_back(entry_session, hash);
        }
        // Most recent session first; ties by hash, so the choice does not depend on map order.
        std::sort(by_recency.begin(), by_recency.end(),
                  [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first
                                                                                : a.second < b.second; });
        by_recency.resize((std::min)(by_recency.size(), MAX_PREWARM));
        prewarm.clear();
        for (const auto& entry : by_recency) {
            prewarm.insert(entry.second);
        }
    }

    static std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    static std::uint64_t Read64(std::span<const std::uint8_t> bytes, std::size_t offset) {
        std::uint64_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    static void Write32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    static void Write64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::uint32_t session{1};
    std::unordered_map<std::uint64_t, std::uint32_t> last_used; ///< hash -> last session drawn
    std::unordered_set<std::uint64_t> prewarm;                  ///< chosen at Load
};

} // namespace D3D12
