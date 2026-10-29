// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// BoundedAdmission gate (disk-load queue window): count and byte caps, an oversized item admitted
// only into an empty window, stop wakes a blocked producer with nothing queued, and a ticket
// comes back when its task throws. Build: cl /std:c++20 /EHsc /Isrc tools\xbox\tests\bounded-admission.cpp

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "video_core/renderer_d3d12/d3d12_bounded_admission.h"

using D3D12::BoundedAdmission;
using namespace std::chrono_literals;

namespace {
/// True when Acquire is still blocked after a short wait; stops it afterwards.
bool Blocks(BoundedAdmission& window, std::uint64_t bytes) {
    std::stop_source stop;
    bool admitted = false;
    std::thread producer{[&] { admitted = static_cast<bool>(window.Acquire(bytes, stop.get_token())); }};
    std::this_thread::sleep_for(50ms);
    stop.request_stop();
    producer.join();
    return !admitted;
}
} // namespace

int main() {
    {
        BoundedAdmission window{2, 1000};
        auto a = window.Acquire(10);
        auto b = window.Acquire(10);
        assert(a && b && window.InFlight() == 2);
        assert(Blocks(window, 10)); // count cap
    }
    {
        BoundedAdmission window{8, 100};
        auto a = window.Acquire(60);
        assert(Blocks(window, 50)); // byte cap
        auto b = window.Acquire(40);
        assert(b && window.PeakBytes() == 100);
    }
    {
        BoundedAdmission window{8, 100};
        auto big = window.Acquire(500); // larger than the cap: alone it passes
        assert(big && Blocks(window, 1));
    }
    {
        BoundedAdmission window{1, 100};
        std::vector<BoundedAdmission::Ticket> held;
        held.push_back(window.Acquire(1));
        std::thread waiter{[&] { auto t = window.Acquire(1); assert(t); }};
        std::this_thread::sleep_for(20ms);
        held.clear(); // release wakes the waiter
        waiter.join();
        assert(window.InFlight() == 0 && window.PeakCount() == 1);
    }
    {
        BoundedAdmission window{1, 100};
        try {
            auto t = window.Acquire(5);
            throw std::runtime_error("task failed");
        } catch (const std::runtime_error&) {
        }
        assert(window.InFlight() == 0); // returned during unwinding
    }
    std::cout << "bounded-admission: OK\n";
}
