// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// The hidden-child capture and the probe gate that bounds it, tested without
// a Terminal. Written after 2026-09-30, when a wedged wslservice turned the
// session-resume WSL probe into a leak: every hung `wsl.exe -- sh -s` left a
// console host behind, one per ~15 s poll.
//
// Also compiled standalone (PROCESS_CAPTURE_STANDALONE) so it can run as a CI
// preflight -- and on a desktop -- with nothing but cl and the WIL headers.
#ifdef PROCESS_CAPTURE_STANDALONE
#include <windows.h>
#include <wil/resource.h>
#include <iostream>
#include <stdexcept>
#include <string>
#define BEGIN_TEST_CLASS(...)
#define END_TEST_CLASS()
#define TEST_METHOD(name) \
public:                   \
    void name()
#define VERIFY_IS_TRUE(value)                                                                      \
    do                                                                                             \
    {                                                                                              \
        if (!(value))                                                                              \
            throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #value);            \
    } while (false)
#define VERIFY_ARE_EQUAL(expected, actual) VERIFY_IS_TRUE((expected) == (actual))
#define VERIFY_IS_FALSE(value) VERIFY_IS_TRUE(!(value))
#else
#include "pch.h"
#endif
#include <TlHelp32.h>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <utility>
#include <thread>
#include <vector>
#include "../../inc/ProcessCaptureImpl.h"
#include "../TerminalApp/ProbeGate.h"

namespace ControlUnitTests
{
    namespace
    {
        std::wstring BaseNameOf(const wchar_t* exe)
        {
            std::wstring name{ exe };
            for (auto& ch : name)
            {
                ch = static_cast<wchar_t>(towlower(ch));
            }
            return name;
        }

        // Every process whose parent is `parent`, with a handle held open so
        // a later liveness check cannot be fooled by pid reuse.
        struct Descendant
        {
            DWORD Pid;
            std::wstring Name;
            wil::unique_handle Handle;
        };

        void CollectChildren(DWORD parent, const std::vector<PROCESSENTRY32W>& all, std::vector<Descendant>& into)
        {
            for (const auto& entry : all)
            {
                if (entry.th32ParentProcessID == parent && entry.th32ProcessID != parent)
                {
                    Descendant d{ entry.th32ProcessID, BaseNameOf(entry.szExeFile), wil::unique_handle{ OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID) } };
                    const auto pid = d.Pid;
                    into.push_back(std::move(d));
                    CollectChildren(pid, all, into);
                }
            }
        }

        std::vector<PROCESSENTRY32W> Snapshot()
        {
            std::vector<PROCESSENTRY32W> all;
            wil::unique_handle snap{ CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
            PROCESSENTRY32W entry{ sizeof(entry) };
            if (snap && Process32FirstW(snap.get(), &entry))
            {
                do
                {
                    all.push_back(entry);
                } while (Process32NextW(snap.get(), &entry));
            }
            return all;
        }

        bool Exited(const Descendant& d)
        {
            // No handle means the process was already gone when we looked.
            // The kill has been issued by the time the capture returns, but a
            // process in a terminated job finishes exiting asynchronously, so
            // allow it a moment -- a leaked one is still here minutes later.
            return !d.Handle || WaitForSingleObject(d.Handle.get(), 2'000) == WAIT_OBJECT_0;
        }
    }

    class ProcessCaptureTests
    {
        BEGIN_TEST_CLASS(ProcessCaptureTests)
        END_TEST_CLASS()

        TEST_METHOD(CapturesStdinRoundTrip);
        TEST_METHOD(TimeoutKillsWholeTreeIncludingConhost);
        TEST_METHOD(ExitedChildLeavesNothingBehind);
        TEST_METHOD(GateIsSingleFlight);
        TEST_METHOD(GateSpacesHealthyProbes);
        TEST_METHOD(GateBacksOffExponentiallyAndCaps);
        TEST_METHOD(GateReportsStateChangesOnce);
        TEST_METHOD(GatedHangingProbeNeverOverlaps);
    };

    void ProcessCaptureTests::CapturesStdinRoundTrip()
    {
        // sort.exe reads all of stdin before writing, so this proves stdin is
        // delivered and closed (sort would wait forever otherwise) and stdout
        // read to EOF.
        const auto result = TerminalUtils::CaptureProcessEx(L"sort.exe", "banana\r\napple\r\n", 10'000);
        VERIFY_IS_TRUE(result.Outcome == TerminalUtils::CaptureOutcome::Exited);
        VERIFY_ARE_EQUAL(static_cast<DWORD>(0), result.ExitCode);
        VERIFY_IS_TRUE(result.Output == "apple\r\nbanana\r\n");
    }

    void ProcessCaptureTests::TimeoutKillsWholeTreeIncludingConhost()
    {
        // The incident's shape: a console client that hangs, with a child of
        // its own. cmd.exe stands in for wsl.exe, ping for whatever it runs.
        std::vector<Descendant> seen;
        std::atomic<bool> stop{ false };
        std::thread watcher([&] {
            // Record our children's trees while the capture is still running,
            // so the conhost is observed alive rather than inferred.
            const auto deadline = GetTickCount64() + 5'000;
            while (!stop && GetTickCount64() < deadline)
            {
                const auto all = Snapshot();
                std::vector<Descendant> now;
                CollectChildren(GetCurrentProcessId(), all, now);
                const auto hasConhost = std::any_of(now.begin(), now.end(), [](const auto& d) { return d.Name == L"conhost.exe"; });
                const auto hasPing = std::any_of(now.begin(), now.end(), [](const auto& d) { return d.Name == L"ping.exe"; });
                if (hasConhost && hasPing)
                {
                    seen = std::move(now);
                    return;
                }
                Sleep(50);
            }
        });

        const auto started = GetTickCount64();
        const auto result = TerminalUtils::CaptureProcessEx(LR"(cmd.exe /d /c "ping -n 60 127.0.0.1 >nul")", {}, 1'500);
        const auto elapsed = GetTickCount64() - started;
        stop = true;
        watcher.join();

        VERIFY_IS_TRUE(result.Outcome == TerminalUtils::CaptureOutcome::TimedOut);
        // Deadline plus the bounded settle waits, nowhere near ping's 60 s.
        VERIFY_IS_TRUE(elapsed < 6'000);

        VERIFY_IS_FALSE(seen.empty());
        auto sawConhost = false;
        for (const auto& d : seen)
        {
            sawConhost |= d.Name == L"conhost.exe";
#ifdef PROCESS_CAPTURE_STANDALONE
            std::wcerr << L"  " << d.Name << L" pid " << d.Pid << (Exited(d) ? L" exited" : L" ALIVE") << L"\n";
#endif
            VERIFY_IS_TRUE(Exited(d));
        }
        // Without this the test would pass on a machine that never made a
        // console host, and prove nothing about the leak.
        VERIFY_IS_TRUE(sawConhost);
    }

    void ProcessCaptureTests::ExitedChildLeavesNothingBehind()
    {
        // A child that exits on its own but leaves a grandchild running --
        // the shape of a CLI that forks a daemon. `start /b` detaches nothing
        // from the job. The grandchild inherits our pipe handle despite its
        // redirection (inheritance passes every inheritable handle down, not
        // just the std ones), so the read runs to the deadline: that is fine,
        // what matters is that nothing survives it.
        const auto started = GetTickCount64();
        const auto result = TerminalUtils::CaptureProcessEx(LR"(cmd.exe /d /c "start /b ping -n 60 127.0.0.1 >nul 2>nul & echo done")", {}, 1'500);
        VERIFY_IS_TRUE(result.Outcome != TerminalUtils::CaptureOutcome::LaunchFailed);
        VERIFY_IS_TRUE(result.Output.find("done") != std::string::npos);
        VERIFY_IS_TRUE(GetTickCount64() - started < 6'000);

        // Nothing parented to the child we ran is still running. A snapshot
        // can list a process that has exited but whose object someone still
        // holds, so each match is asked, not merely counted.
        for (const auto& entry : Snapshot())
        {
            if (entry.th32ParentProcessID != result.ProcessId)
            {
                continue;
            }
            const Descendant d{ entry.th32ProcessID, BaseNameOf(entry.szExeFile), wil::unique_handle{ OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID) } };
            VERIFY_IS_TRUE(Exited(d));
        }
    }

    void ProcessCaptureTests::GateIsSingleFlight()
    {
        TerminalApp::ProbeGate gate;
        VERIFY_IS_TRUE(gate.TryBegin(0));
        // However much time passes, and however urgent, never a second one
        // while the first is outstanding -- this is what stops the pile-up.
        VERIFY_IS_FALSE(gate.TryBegin(15'000));
        VERIFY_IS_FALSE(gate.TryBegin(3'600'000, true));
        gate.End(3'600'000, true);
        VERIFY_IS_TRUE(gate.TryBegin(3'600'000));
    }

    void ProcessCaptureTests::GateSpacesHealthyProbes()
    {
        TerminalApp::ProbeGate gate{ { 30'000, 30'000, 300'000 } };
        VERIFY_IS_TRUE(gate.TryBegin(1'000));
        gate.End(2'000, true);
        VERIFY_IS_FALSE(gate.TryBegin(16'000)); // next persist tick
        VERIFY_IS_TRUE(gate.TryBegin(16'000, true)); // shutdown skips spacing
        gate.End(16'500, true);
        VERIFY_IS_FALSE(gate.TryBegin(40'000)); // spacing runs from the last start
        VERIFY_IS_TRUE(gate.TryBegin(46'000));
    }

    void ProcessCaptureTests::GateBacksOffExponentiallyAndCaps()
    {
        TerminalApp::ProbeGate gate{ { 30'000, 30'000, 300'000 } };
        uint64_t now = 0;
        const uint64_t expected[] = { 30'000, 60'000, 120'000, 240'000, 300'000, 300'000 };
        for (const auto wait : expected)
        {
            VERIFY_IS_TRUE(gate.TryBegin(now));
            now += 10'000; // the probe hangs to its deadline
            gate.End(now, false);
            VERIFY_ARE_EQUAL(wait, gate.CurrentBackoffMs());
            // Backoff binds even the urgent shutdown path.
            VERIFY_IS_FALSE(gate.TryBegin(now + wait - 1, true));
            now += wait;
        }
        VERIFY_IS_TRUE(gate.TryBegin(now));
        gate.End(now, true);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, gate.CurrentBackoffMs());
    }

    void ProcessCaptureTests::GateReportsStateChangesOnce()
    {
        using T = TerminalApp::ProbeGate::Transition;
        TerminalApp::ProbeGate gate{ { 0, 1, 1 } };
        uint64_t now = 0;
        const auto run = [&](bool ok) {
            now += 10;
            VERIFY_IS_TRUE(gate.TryBegin(now));
            now += 10;
            return gate.End(now, ok);
        };
        VERIFY_IS_TRUE(run(true) == T::None);
        VERIFY_IS_TRUE(run(false) == T::WentDown);
        VERIFY_IS_TRUE(run(false) == T::None); // silent while still down
        VERIFY_IS_TRUE(run(false) == T::None);
        VERIFY_IS_TRUE(run(true) == T::Recovered);
        VERIFY_IS_TRUE(run(true) == T::None);
    }

    void ProcessCaptureTests::GatedHangingProbeNeverOverlaps()
    {
        // The incident end to end, compressed: ticks every 100 ms against a
        // probe that hangs for its whole 1 s deadline. Before the gate, each
        // tick started another hung child.
        TerminalApp::ProbeGate gate{ { 0, 60'000, 60'000 } };
        std::mutex lock;
        std::atomic<int> launched{ 0 };
        std::atomic<int> concurrent{ 0 };
        std::atomic<int> peak{ 0 };
        std::vector<std::thread> workers;

        for (auto tick = 0; tick < 20; ++tick)
        {
            auto mine = false;
            {
                std::lock_guard guard{ lock };
                mine = gate.TryBegin(GetTickCount64());
            }
            if (mine)
            {
                ++launched;
                workers.emplace_back([&] {
                    const auto now = ++concurrent;
                    auto seenPeak = peak.load();
                    while (now > seenPeak && !peak.compare_exchange_weak(seenPeak, now))
                    {
                    }
                    const auto result = TerminalUtils::CaptureProcessEx(LR"(cmd.exe /d /c "ping -n 60 127.0.0.1 >nul")", {}, 1'000);
                    --concurrent;
                    std::lock_guard guard{ lock };
                    gate.End(GetTickCount64(), result.Outcome == TerminalUtils::CaptureOutcome::Exited);
                });
            }
            Sleep(100);
        }
        for (auto& worker : workers)
        {
            worker.join();
        }

        VERIFY_ARE_EQUAL(1, launched.load()); // one hang, then backing off
        VERIFY_ARE_EQUAL(1, peak.load());
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, gate.ConsecutiveFailures());
    }
}

#ifdef PROCESS_CAPTURE_STANDALONE
int main()
{
    try
    {
        ControlUnitTests::ProcessCaptureTests tests;
        const std::pair<const char*, void (ControlUnitTests::ProcessCaptureTests::*)()> cases[] = {
            { "CapturesStdinRoundTrip", &ControlUnitTests::ProcessCaptureTests::CapturesStdinRoundTrip },
            { "TimeoutKillsWholeTreeIncludingConhost", &ControlUnitTests::ProcessCaptureTests::TimeoutKillsWholeTreeIncludingConhost },
            { "ExitedChildLeavesNothingBehind", &ControlUnitTests::ProcessCaptureTests::ExitedChildLeavesNothingBehind },
            { "GateIsSingleFlight", &ControlUnitTests::ProcessCaptureTests::GateIsSingleFlight },
            { "GateSpacesHealthyProbes", &ControlUnitTests::ProcessCaptureTests::GateSpacesHealthyProbes },
            { "GateBacksOffExponentiallyAndCaps", &ControlUnitTests::ProcessCaptureTests::GateBacksOffExponentiallyAndCaps },
            { "GateReportsStateChangesOnce", &ControlUnitTests::ProcessCaptureTests::GateReportsStateChangesOnce },
            { "GatedHangingProbeNeverOverlaps", &ControlUnitTests::ProcessCaptureTests::GatedHangingProbeNeverOverlaps },
        };
        for (const auto& [name, test] : cases)
        {
            std::cout << name << "... " << std::flush;
            (tests.*test)();
            std::cout << "ok\n";
        }
        std::cout << "All " << std::size(cases) << " process capture tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
#endif
