// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Keeps profiles.list in the order the new tab dropdown shows its profiles.
//
// The dropdown has had its own layout since new tab menu customization landed,
// but everything that numbers profiles - ctrl+shift+1..9, newTab's "index",
// the jump list, the Profiles page - still walks profiles.list. The two drift
// apart, and the shortcut labels in the dropdown come out as 4, 7, 3, 1, ...
// (GH#8914). This file makes the dropdown the authority: wherever the dropdown
// shows a profile, profiles.list follows its order.

#include "pch.h"
#include "CascadiaSettings.h"
#include "FolderEntry.h"
#include "MatchProfilesEntry.h"
#include "ProfileEntry.h"

using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Microsoft::Terminal::Settings;
using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;

namespace
{
    bool _contains(const std::vector<winrt::Microsoft::Terminal::Settings::Model::Profile>& profiles, const winrt::guid& guid)
    {
        return std::ranges::any_of(profiles, [&](const auto& p) { return p.Guid() == guid; });
    }
}

// The profile a dropdown entry points at, as an object of *this* settings.
//
// An entry read from JSON carries a name. Its Profile is NOT null until
// _resolveNewTabMenuProfiles runs: WINRT_PROPERTY value-initializes it, and a
// value-initialized runtimeclass is a freshly activated empty Profile (which is
// why the resolver writes nullptr over it). So the name goes first. An entry
// the editor added has no name and a real Profile; after Copy() that Profile
// still belongs to the settings that were copied, so it is looked up again by
// GUID rather than used as it is.
Model::Profile CascadiaSettings::_profileForMenuEntry(const Model::ProfileEntry& entry) const
{
    if (const auto name = winrt::get_self<implementation::ProfileEntry>(entry)->ProfileName(); !name.empty())
    {
        if (const auto profile = GetProfileByName(name))
        {
            return profile;
        }
    }
    if (const auto profile = entry.Profile())
    {
        return FindProfile(profile.Guid());
    }
    return nullptr;
}

// Depth-first walk of the dropdown. A null in `shown` marks where the
// "remaining profiles" entry sits; it can only be expanded once every entry,
// including those after it, has claimed its profiles.
void CascadiaSettings::_collectMenuProfiles(const IVector<Model::NewTabMenuEntry>& entries,
                                            std::vector<Model::Profile>& shown,
                                            std::vector<Model::Profile>& claimed,
                                            bool& sawRemaining) const
{
    if (!entries)
    {
        return;
    }

    for (const auto& entry : entries)
    {
        if (!entry)
        {
            continue;
        }

        switch (entry.Type())
        {
        case NewTabMenuEntryType::Profile:
        {
            // Same rule as _resolveNewTabMenuProfilesSet: a hidden profile is
            // not shown even when it is named explicitly.
            if (const auto profile = _profileForMenuEntry(entry.as<Model::ProfileEntry>()); profile && !profile.Hidden())
            {
                shown.emplace_back(profile);
                claimed.emplace_back(profile);
            }
            break;
        }
        case NewTabMenuEntryType::MatchProfiles:
        {
            const auto matchEntry{ winrt::get_self<implementation::MatchProfilesEntry>(entry.as<Model::MatchProfilesEntry>()) };
            for (const auto& profile : _activeProfiles)
            {
                if (matchEntry->MatchesProfile(profile))
                {
                    shown.emplace_back(profile);
                    claimed.emplace_back(profile);
                }
            }
            break;
        }
        case NewTabMenuEntryType::RemainingProfiles:
        {
            // Only the first one counts; the loader warns about the rest.
            if (!sawRemaining)
            {
                sawRemaining = true;
                shown.emplace_back(nullptr);
            }
            break;
        }
        case NewTabMenuEntryType::Folder:
        {
            const auto folderEntry{ winrt::get_self<implementation::FolderEntry>(entry.as<Model::FolderEntry>()) };
            _collectMenuProfiles(folderEntry->RawEntries(), shown, claimed, sawRemaining);
            break;
        }
        default:
            break;
        }
    }
}

// Put `wanted` into the positions of _allProfiles that those same profiles
// occupy now. Everything else - hidden and orphaned profiles, and any profile
// `wanted` does not mention - keeps its exact index. Returns whether anything
// moved.
bool CascadiaSettings::_permuteProfileSlots(const std::vector<Model::Profile>& wanted)
{
    std::vector<Model::Profile> all{ _allProfiles.Size(), nullptr };
    _allProfiles.GetMany(0, all);

    std::vector<size_t> slots;
    slots.reserve(wanted.size());
    for (size_t i = 0; i < all.size(); ++i)
    {
        if (_contains(wanted, all[i].Guid()))
        {
            slots.emplace_back(i);
        }
    }

    // Every wanted profile is one of ours, so the counts match. If they ever
    // do not, refilling would drop or duplicate a profile: leave it alone.
    if (slots.size() != wanted.size())
    {
        return false;
    }

    auto changed = false;
    for (size_t k = 0; k < slots.size(); ++k)
    {
        if (all[slots[k]].Guid() != wanted[k].Guid())
        {
            all[slots[k]] = wanted[k];
            changed = true;
        }
    }
    if (!changed)
    {
        return false;
    }

    std::vector<Model::Profile> active;
    active.reserve(all.size());
    for (const auto& profile : all)
    {
        // The loader's rule (CascadiaSettings(SettingsLoader&&)), so a reorder
        // never changes *which* profiles are active, only their order.
        if (!profile.Hidden() && !profile.Orphaned())
        {
            active.emplace_back(profile);
        }
    }

    // New vectors, not ReplaceAll. A Reset on _allProfiles reaches whatever is
    // bound to it, and the Dropdown Menu page's profile picker stays subscribed
    // after that page is navigated away from: its x:Bind handler then runs on a
    // torn-down page and access-violates (seen in the test slot under cdb, on
    // the first dirty check after leaving that page). Every other reader fetches
    // AllProfiles() afresh; the picker keeps the old order until it rebinds,
    // which only affects the order it lists profiles in.
    _allProfiles = winrt::single_threaded_observable_vector(std::move(all));
    _activeProfiles = winrt::single_threaded_observable_vector(std::move(active));
    return true;
}

// Called by the loader just before it resolves the dropdown, and by the
// settings editor after every edit. It does not re-resolve the dropdown itself:
// the editor's entries hold profiles that re-resolving by name would discard,
// and the loader resolves straight afterwards anyway.
bool CascadiaSettings::ApplyNewTabMenuProfileOrder()
{
    std::vector<Model::Profile> shown;
    std::vector<Model::Profile> claimed;
    auto sawRemaining = false;
    _collectMenuProfiles(_globals->NewTabMenu(), shown, claimed, sawRemaining);

    // Expand "remaining profiles" in place and drop repeats: a profile named
    // explicitly can also be caught by a matcher, and it counts where it is
    // first shown.
    std::vector<Model::Profile> order;
    order.reserve(_activeProfiles.Size());
    const auto add = [&](const Model::Profile& profile) {
        if (!_contains(order, profile.Guid()))
        {
            order.emplace_back(profile);
        }
    };
    for (const auto& profile : shown)
    {
        if (profile)
        {
            add(profile);
            continue;
        }
        for (const auto& remaining : _activeProfiles)
        {
            if (!_contains(claimed, remaining.Guid()))
            {
                add(remaining);
            }
        }
    }

    return _permuteProfileSlots(order);
}

void CascadiaSettings::ReorderProfiles(const IVectorView<winrt::guid>& order)
{
    std::vector<Model::Profile> wanted;
    wanted.reserve(order.Size());
    for (const auto& guid : order)
    {
        if (const auto profile = FindProfile(guid); profile && !_contains(wanted, guid))
        {
            wanted.emplace_back(profile);
        }
    }
    _permuteProfileSlots(wanted);
}
