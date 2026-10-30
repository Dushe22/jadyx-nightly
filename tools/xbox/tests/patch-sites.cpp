// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynarmic's compact patch site lists: one site inline, more spilled, order and kinds kept.
// Build: cl /std:c++20 /EHsc /Isrc\dynarmic\src tools\xbox\tests\patch-sites.cpp

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>
#include "dynarmic/backend/x64/patch_sites.h"

using Dynarmic::Backend::X64::PatchSiteLists;
using Sites = std::vector<std::pair<unsigned, std::uint64_t>>;

static Sites Read(const PatchSiteLists& lists, PatchSiteLists::Slot slot) {
    Sites out;
    lists.ForEach(slot, [&](unsigned kind, std::uint64_t location) { out.emplace_back(kind, location); });
    return out;
}

int main() {
    static_assert(sizeof(PatchSiteLists::Slot) == 8);
    PatchSiteLists lists;
    PatchSiteLists::Slot empty{}, one{}, many{}, jg{};
    assert(Read(lists, empty).empty());

    // A typical high user-mode address, every kind.
    const std::uint64_t base = 0x7ff6'1234'5000;
    lists.Add(one, 3, base);
    assert((Read(lists, one) == Sites{{3, base}}));

    // Kind 0 with a single site must not read as spilled.
    lists.Add(jg, 0, base + 8);
    assert((Read(lists, jg) == Sites{{0, base + 8}}));

    for (unsigned i = 0; i < 6; ++i) {
        lists.Add(many, i % 4, base + 16 * i);
    }
    Sites expected;
    for (unsigned i = 0; i < 6; ++i) {
        expected.emplace_back(i % 4, base + 16 * i);
    }
    assert(Read(lists, many) == expected);
    // Spilling one slot leaves the others alone.
    assert((Read(lists, one) == Sites{{3, base}}));
    assert((Read(lists, jg) == Sites{{0, base + 8}}));

    // A second spilled slot gets its own list.
    PatchSiteLists::Slot other{};
    lists.Add(other, 0, base + 0x100);
    lists.Add(other, 0, base + 0x200);
    assert((Read(lists, other) == Sites{{0, base + 0x100}, {0, base + 0x200}}));
    assert(Read(lists, many) == expected);

    lists.Clear();
    PatchSiteLists::Slot fresh{};
    lists.Add(fresh, 1, base);
    lists.Add(fresh, 2, base + 4);
    assert((Read(lists, fresh) == Sites{{1, base}, {2, base + 4}}));
    std::cout << "patch-sites: ok\n";
}
