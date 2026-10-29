// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ExactShareIndex gate (reloadable DXIL dedup): byte-equal values share one live copy, a key
// collision never shares, weak entries neither keep values alive nor block a later reload.
// Build: cl /std:c++20 /EHsc /Isrc tools\xbox\tests\exact-share-index.cpp

#undef NDEBUG
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_exact_share_index.h"

namespace {
struct Hash {
    std::size_t operator()(int key) const noexcept { return static_cast<std::size_t>(key); }
};
using Bytes = std::vector<unsigned char>;
using Index = D3D12::ExactShareIndex<int, Bytes, Hash>;
std::shared_ptr<const Bytes> Make(Bytes b) { return std::make_shared<const Bytes>(std::move(b)); }
} // namespace

int main() {
    Index index;
    auto first = Make({1, 2, 3});
    assert(index.Share(7, first) == first);
    assert(index.Find(7) == first);
    // Equal bytes read back again: the live copy, not a second one.
    auto again = Make({1, 2, 3});
    assert(index.Share(7, again) == first);
    // Same key, different bytes (a hash collision): never shared, the live owner stays.
    auto other = Make({9});
    assert(index.Share(7, other) == other);
    assert(index.Find(7) == first);
    // Weak: dropping every holder frees the value, and a reload registers itself.
    std::weak_ptr<const Bytes> watch = first;
    first.reset();
    again.reset();
    assert(watch.expired() && index.Find(7) == nullptr);
    auto reloaded = Make({1, 2, 3});
    assert(index.Share(7, reloaded) == reloaded && index.Find(7) == reloaded);
    // Expired slots are pruned on growth.
    for (int key = 100; key < 400; ++key) index.Share(key, Make({1}));
    assert(index.Size() < 300);
    std::cout << "exact-share-index: OK\n";
}
