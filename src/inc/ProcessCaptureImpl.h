// Shared hidden process capture for previews, integration settings and the
// session-resume WSL probe.
//
// Every child runs inside its own job object with KILL_ON_JOB_CLOSE, assigned
// while the child is still suspended. That is the whole defence against the
// 2026-09-30 incident, where a wedged wslservice made every probe's wsl.exe
// hang: TerminateProcess took the wsl.exe down, but the conhost it had created
// for itself survived serving nobody, one per poll -- 205 of them, 1.5 GB, in
// an hour. A console host is created by the console client during its own
// startup, so it is born inside the client's job, and terminating the job
// takes the whole tree: the client, its conhost, and anything it spawned. The
// job handle is also the only reference, so if this process dies mid-capture
// the kernel closes it and the tree goes with us.
//
// The child inherits exactly its two pipe ends (PROC_THREAD_ATTRIBUTE_HANDLE_LIST),
// never whatever else happens to be inheritable in this process at that
// instant -- another thread's pipes included.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
namespace TerminalUtils
{
    enum class CaptureOutcome
    {
        LaunchFailed, // nothing ran
        Exited, // the child exited on its own; ExitCode is valid
        TimedOut, // the deadline passed and the child's job was terminated
    };

    struct CaptureResult
    {
        std::string Output;
        CaptureOutcome Outcome{ CaptureOutcome::LaunchFailed };
        DWORD ProcessId{ 0 };
        DWORD ExitCode{ 0 };
    };

    inline CaptureResult CaptureProcessEx(std::wstring commandLine, std::string_view stdinData, unsigned long timeoutMs)
    {
        CaptureResult result;
        if (commandLine.empty())
        {
            return result;
        }

        wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        if (!job)
        {
            return result;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // No JOB_OBJECT_LIMIT_BREAKAWAY_OK: nothing in the tree may leave it.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            return result;
        }

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };

        // stdin is sized to hold everything we will write, so the WriteFile
        // below completes into the pipe buffer even if the child never reads a
        // byte. A wedged child that is not reading must not be able to block
        // us before the deadline loop even starts.
        const auto stdinBuffer = static_cast<DWORD>(std::min<size_t>(stdinData.size() + 4096, 1u << 20));

        wil::unique_handle inRead, inWrite, outRead, outWrite;
        if (!CreatePipe(inRead.addressof(), inWrite.addressof(), &sa, stdinBuffer) ||
            !CreatePipe(outRead.addressof(), outWrite.addressof(), &sa, 0))
        {
            return result;
        }
        SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);

        // Only these two cross into the child. stdout and stderr share one
        // handle, and the list must not name a handle twice.
        HANDLE inheritList[] = { inRead.get(), outWrite.get() };
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<std::byte> attrBuffer(attrSize);
        const auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize))
        {
            return result;
        }
        const auto deleteAttrs = wil::scope_exit([&]() noexcept { DeleteProcThreadAttributeList(attrs); });
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList, sizeof(inheritList), nullptr, nullptr))
        {
            return result;
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = inRead.get();
        si.StartupInfo.hStdOutput = outWrite.get();
        // Merged into stdout rather than left null: STARTF_USESTDHANDLES hands
        // the child exactly these three, and a null stderr is an invalid handle
        // it may refuse to start with. Anything the child complains about lands
        // in the output, where the caller's parser drops it.
        si.StartupInfo.hStdError = outWrite.get();
        si.lpAttributeList = attrs;

        // CREATE_NO_WINDOW matters on a machine where Windows Terminal is the
        // registered default terminal host: a console child launched without it
        // gets a Terminal window created for it before any hide request could
        // apply. See "Every hidden background launch" in the repo's AGENTS.md.
        //
        // CREATE_SUSPENDED so the child is in the job before it runs a single
        // instruction -- and so before it creates its console host.
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi))
        {
            return result;
        }
        wil::unique_handle process{ pi.hProcess };
        wil::unique_handle thread{ pi.hThread };
        result.ProcessId = pi.dwProcessId;

        if (!AssignProcessToJobObject(job.get(), process.get()))
        {
            // Running it outside a job is exactly the leak this exists to
            // prevent, so it does not run at all.
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), 1000);
            return result;
        }
        ResumeThread(thread.get());
        thread.reset();

        // Close our copies of the child's ends first, or the reads below never
        // see EOF because this process still holds the pipe open.
        inRead.reset();
        outWrite.reset();

        if (!stdinData.empty())
        {
            DWORD written = 0;
            WriteFile(inWrite.get(), stdinData.data(), static_cast<DWORD>(stdinData.size()), &written, nullptr);
        }
        inWrite.reset();

        // Polled rather than a plain blocking read loop, because a blocking
        // ReadFile cannot be given a deadline: if the child is cold, wedged, or
        // mid-shutdown, the read never returns and the timeout below never gets
        // to run. Some callers run while the app is closing, so "wait forever"
        // is not an option any of them may take.
        char buffer[4096];
        const auto deadline = GetTickCount64() + timeoutMs;
        auto timedOut = false;

        for (;;)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(outRead.get(), nullptr, 0, nullptr, &available, nullptr))
            {
                // Every writer closed its end: everything it wrote is read.
                break;
            }

            if (available == 0)
            {
                if (GetTickCount64() > deadline)
                {
                    timedOut = true;
                    break;
                }
                Sleep(20);
                continue;
            }

            DWORD read = 0;
            const auto want = std::min(static_cast<DWORD>(sizeof(buffer)), available);
            if (!ReadFile(outRead.get(), buffer, want, &read, nullptr) || read == 0)
            {
                break;
            }
            result.Output.append(buffer, read);
        }

        // The pipe can close before the process exits; give it a moment, but
        // never past the deadline by more than that moment.
        if (!timedOut && WaitForSingleObject(process.get(), 1000) == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(process.get(), &result.ExitCode);
            result.Outcome = CaptureOutcome::Exited;
        }
        else
        {
            result.Outcome = CaptureOutcome::TimedOut;
        }

        // Unconditional: even a child that exited cleanly may have left
        // something behind in its job. KILL_ON_JOB_CLOSE would do this when
        // `job` goes out of scope; doing it explicitly and waiting means the
        // tree is actually gone by the time we return, not merely doomed.
        TerminateJobObject(job.get(), 1);
        const auto settleBy = GetTickCount64() + 1000;
        for (;;)
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) ||
                accounting.ActiveProcesses == 0 || GetTickCount64() > settleBy)
            {
                break;
            }
            Sleep(10);
        }
        // The job's count drops a moment before the process object is
        // signalled; callers that check the child next should see it gone.
        WaitForSingleObject(process.get(), 500);
        return result;
    }

    inline std::string CaptureProcess(std::wstring commandLine, std::string_view stdinData, unsigned long timeoutMs)
    {
        return CaptureProcessEx(std::move(commandLine), stdinData, timeoutMs).Output;
    }
}
