// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace Dynarmic::Backend::X64 {

/// The sites in emitted code that jump to (or load) one block's entrypoint, kept in 8 bytes per
/// linked block: one site inline, since most blocks are linked from one site, and longer lists
/// spilled to a vector this class owns. A boost::small_vector<site, 1> took 32 bytes, and the map
/// holding one per linked block reached 38.5 MiB a core in TOTK.
class PatchSiteLists {
public:
    /// A map value: 0 (no sites), one site, or SPILLED | index into lists.
    using Slot = std::uint64_t;

    /// kind: 0-3; location: a user-mode address (below 2^47), never 0.
    void Add(Slot& slot, unsigned kind, std::uint64_t location) {
        const Slot site = location | (static_cast<Slot>(kind) << KIND_SHIFT);
        if (slot == 0) {
            slot = site;
        } else if (!IsSpilled(slot)) {
            const Slot first = slot;
            slot = SPILLED | static_cast<Slot>(lists.size());
            lists.push_back({first, site});
        } else {
            lists[slot & INDEX_MASK].push_back(site);
        }
    }

    /// Calls f(kind, location) for each site of slot, in the order they were added.
    template <typename F>
    void ForEach(Slot slot, F&& f) const {
        if (slot == 0) {
            return;
        }
        if (!IsSpilled(slot)) {
            f(static_cast<unsigned>(slot >> KIND_SHIFT), slot & ADDRESS_MASK);
            return;
        }
        for (const Slot site : lists[slot & INDEX_MASK]) {
            f(static_cast<unsigned>(site >> KIND_SHIFT), site & ADDRESS_MASK);
        }
    }

    /// Releases every spilled list; slots handed out before are invalid afterwards.
    void Clear() {
        lists = decltype(lists){};
    }

private:
    static constexpr unsigned KIND_SHIFT = 62;
    // Bit 61 is clear in every site: kinds use bits 62-63 and addresses stay below 2^47.
    static constexpr Slot SPILLED = Slot{1} << 61;
    static constexpr Slot INDEX_MASK = SPILLED - 1;
    static constexpr Slot ADDRESS_MASK = (Slot{1} << KIND_SHIFT) - 1;

    [[nodiscard]] static bool IsSpilled(Slot slot) noexcept {
        return (slot >> KIND_SHIFT) == 0 && (slot & SPILLED) != 0;
    }

    std::vector<std::vector<Slot>> lists;
};

} // namespace Dynarmic::Backend::X64
