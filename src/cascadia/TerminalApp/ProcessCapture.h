// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Running a child process to completion and reading everything it wrote,
// with a deadline.
//
// A thin wrapper over TerminalUtils::CaptureProcessEx (src/inc/ProcessCaptureImpl.h),
// which PaneSessionCapture's WSL probe calls directly because it needs to know
// whether the child timed out. The integration fetch pipeline uses this for its
// `command` steps. There is one implementation so the two cannot drift: the
// CREATE_NO_WINDOW, the kill-on-close job object and the polled read are all
// load-bearing, and none of them is obvious.

#pragma once

#include <string>
#include <string_view>

namespace TerminalApp
{
    // Blocking. Launches `commandLine`, writes `stdinData` to the child's stdin
    // (nothing at all when it is empty), and reads stdout+stderr to EOF or to
    // the deadline, whichever comes first. The child runs in a job object, and
    // its whole tree -- console host included -- is terminated before this
    // returns, at the deadline or after a clean exit alike.
    //
    // Returns what was read, which is empty when the process could not be
    // started. The bytes are whatever the child wrote -- no decoding happens
    // here, because the caller knows whether it expects UTF-8.
    //
    // The command line is taken by value because CreateProcessW insists on a
    // writable buffer and may modify it in place.
    std::string RunProcessCapture(std::wstring commandLine, std::string_view stdinData, unsigned long timeoutMs);
}
