// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Single-flight and backoff for a recurring probe of something that can hang.
//
// Written for the session-resume WSL probe after 2026-09-30, when a wedged
// wslservice made every `wsl.exe -- sh -s` hang: the probe was fired on every
// persist tick (~15 s) with nothing tracking whether the previous one had
// come back, so hung calls piled up and each left a console host behind.
//
// The gate answers one question -- may a probe start now? -- and never
// blocks. It is deliberately free of Win32 and of any clock, so the timing
// rules can be tested with made-up times (UnitTests_Control/ProbeGateTests).
// Not thread-safe by itself; the owner holds a lock around it.

#pragma once

#include <algorithm>
#include <cstdint>

namespace TerminalApp
{
    class ProbeGate
    {
    public:
        struct Timing
        {
            // Minimum gap between the STARTS of two healthy probes. The
            // persist timer can tick every 5 s; the probe forks a few
            // processes per /proc entry inside the distro, and a resume
            // command up to this stale is fine.
            uint64_t MinSpacingMs{ 30'000 };
            // First wait after a failure, doubled per consecutive failure...
            uint64_t BaseBackoffMs{ 30'000 };
            // ...up to this.
            uint64_t MaxBackoffMs{ 300'000 };
        };

        // What End() changed, so the caller can log a state change once
        // rather than on every poll.
        enum class Transition
        {
            None,
            WentDown, // first failure after a success (or ever)
            Recovered, // first success after one or more failures
        };

        ProbeGate() = default;
        explicit ProbeGate(Timing timing) noexcept :
            _timing{ timing } {}

        // Claims the probe. False when one is still outstanding (never two at
        // once, however long the first takes), or when the spacing or backoff
        // has not elapsed. `urgent` skips the healthy spacing -- the shutdown
        // capture wants the freshest answer -- but never the backoff: a
        // target known not to be answering is not asked again early.
        bool TryBegin(uint64_t nowMs, bool urgent = false) noexcept
        {
            if (_inFlight)
            {
                return false;
            }
            if (_everStarted)
            {
                const auto notBefore = _failures > 0 ? _backoffUntilMs : (urgent ? 0 : _lastStartMs + _timing.MinSpacingMs);
                if (nowMs < notBefore)
                {
                    return false;
                }
            }
            _inFlight = true;
            _everStarted = true;
            _lastStartMs = nowMs;
            return true;
        }

        // Releases the claim. Only the holder of a successful TryBegin calls it.
        Transition End(uint64_t nowMs, bool succeeded) noexcept
        {
            _inFlight = false;
            if (succeeded)
            {
                const auto wasDown = _failures > 0;
                _failures = 0;
                _backoffUntilMs = 0;
                return wasDown ? Transition::Recovered : Transition::None;
            }

            ++_failures;
            _backoffUntilMs = nowMs + CurrentBackoffMs();
            return _failures == 1 ? Transition::WentDown : Transition::None;
        }

        // The wait that follows the current run of failures; 0 when healthy.
        uint64_t CurrentBackoffMs() const noexcept
        {
            if (_failures == 0)
            {
                return 0;
            }
            // Shift capped well below 64 so the doubling cannot overflow.
            const auto shift = std::min<uint32_t>(_failures - 1, 20);
            return std::min(_timing.BaseBackoffMs << shift, _timing.MaxBackoffMs);
        }

        bool InFlight() const noexcept { return _inFlight; }
        uint32_t ConsecutiveFailures() const noexcept { return _failures; }

    private:
        Timing _timing{};
        bool _inFlight{ false };
        bool _everStarted{ false };
        uint64_t _lastStartMs{ 0 };
        uint64_t _backoffUntilMs{ 0 };
        uint32_t _failures{ 0 };
    };
}
