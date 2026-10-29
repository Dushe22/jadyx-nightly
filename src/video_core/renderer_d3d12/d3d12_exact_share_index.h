// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace D3D12 {

/// Live immutable values found again by a hashed identity, shared only when their bytes are
/// equal: a hash only says where to look. Holds weak references, so it never keeps a value alive
/// and never counts its memory. Callers load outside the lock (Share takes it briefly).
template <typename Key, typename Value, typename KeyHash>
class ExactShareIndex {
public:
    /// The live value registered for key, or null.
    std::shared_ptr<const Value> Find(const Key& key) const {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(key);
        return it != entries.end() ? it->second.lock() : nullptr;
    }

    /// Returns the live value registered for key when it equals value; otherwise registers value
    /// unless another live one owns the key (a collision), and returns value itself.
    std::shared_ptr<const Value> Share(const Key& key, std::shared_ptr<const Value> value) {
        std::scoped_lock lock{mutex};
        auto& slot = entries[key];
        if (auto live = slot.lock()) {
            return *live == *value ? live : value;
        }
        slot = value;
        // shortcut: prunes expired slots only on growth past twice the last live count; switch to
        // an LRU if titles ever reach millions of distinct pipelines.
        if (entries.size() > 2 * pruned_size) {
            std::erase_if(entries, [](const auto& entry) { return entry.second.expired(); });
            pruned_size = (std::max)(std::size_t{64}, entries.size());
        }
        return value;
    }

    std::size_t Size() const {
        std::scoped_lock lock{mutex};
        return entries.size();
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<Key, std::weak_ptr<const Value>, KeyHash> entries;
    std::size_t pruned_size{64};
};

} // namespace D3D12
