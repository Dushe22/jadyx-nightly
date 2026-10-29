// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <utility>

namespace D3D12 {

/// Caps work queued but not finished by count and by bytes. The producer blocks in Acquire; the
/// task gives its share back with a Ticket when it ends, on whatever thread runs it. Producers
/// must never be the threads that run the tasks, or a full window waits on itself.
class BoundedAdmission {
public:
    BoundedAdmission(std::uint64_t max_count_, std::uint64_t max_bytes_)
        : max_count{(std::max)(max_count_, std::uint64_t{1})}, max_bytes{max_bytes_} {}

    /// Returned to the window when destroyed, also when the task throws.
    class Ticket {
    public:
        Ticket() = default;
        Ticket(BoundedAdmission* owner_, std::uint64_t bytes_) : owner{owner_}, bytes{bytes_} {}
        Ticket(Ticket&& other) noexcept
            : owner{std::exchange(other.owner, nullptr)}, bytes{other.bytes} {}
        Ticket& operator=(Ticket&&) = delete;
        ~Ticket() {
            if (owner) owner->Release(bytes);
        }
        explicit operator bool() const noexcept {
            return owner != nullptr;
        }

    private:
        BoundedAdmission* owner{};
        std::uint64_t bytes{};
    };

    /// Blocks until the window has room; an item larger than the byte cap waits for an empty
    /// window. An empty Ticket only when stop was requested: queue nothing then.
    [[nodiscard]] Ticket Acquire(std::uint64_t bytes, std::stop_token stop = {}) {
        std::unique_lock lock{mutex};
        const bool admitted = condition.wait(lock, stop, [&] {
            return count < max_count && (count == 0 || in_flight + bytes <= max_bytes);
        });
        if (!admitted) {
            return {};
        }
        ++count;
        in_flight += bytes;
        peak_count = (std::max)(peak_count, count);
        peak_bytes = (std::max)(peak_bytes, in_flight);
        return Ticket{this, bytes};
    }

    std::uint64_t PeakCount() const {
        std::scoped_lock lock{mutex};
        return peak_count;
    }
    std::uint64_t PeakBytes() const {
        std::scoped_lock lock{mutex};
        return peak_bytes;
    }
    std::uint64_t InFlight() const {
        std::scoped_lock lock{mutex};
        return count;
    }

private:
    void Release(std::uint64_t bytes) {
        {
            std::scoped_lock lock{mutex};
            --count;
            in_flight -= bytes;
        }
        condition.notify_all();
    }

    const std::uint64_t max_count;
    const std::uint64_t max_bytes;
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::uint64_t count{};
    std::uint64_t in_flight{};
    std::uint64_t peak_count{};
    std::uint64_t peak_bytes{};
};

} // namespace D3D12
