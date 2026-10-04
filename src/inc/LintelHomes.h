// Mullion's side of Lintel's "~/..." policy (paths/README.md, rule 6): the homes this
// host knows for a pane. Not synced from Lintel -- what a host knows, and how it finds
// out, is the host's business; the policy only says what to do with it.
#pragma once
#include "LintelPaths.h"
#include "../types/inc/utils.hpp"

namespace Mullion
{
    // The Windows profile directory, which "~" means to a non-WSL shell, and the source
    // distribution's home when the pane is WSL. Pass mayTouchFileSystem false on the UI
    // thread: a WSL home is then only what is already cached, and the caller should
    // treat a "~" path that resolves to nothing as not known yet.
    inline std::vector<Lintel::PathHome> PathHomesFor(std::wstring_view sourceDistro, const bool mayTouchFileSystem)
    {
        std::vector<Lintel::PathHome> homes;
        if (auto home = ::Microsoft::Console::Utils::WindowsHomeDirectory(); !home.empty())
        {
            homes.push_back({ std::wstring{}, std::move(home) });
        }
        if (!sourceDistro.empty())
        {
            if (auto home = ::Microsoft::Console::Utils::WslHomeDirectory(sourceDistro, mayTouchFileSystem); !home.empty())
            {
                homes.push_back({ std::wstring{ sourceDistro }, std::move(home) });
            }
        }
        return homes;
    }
}
