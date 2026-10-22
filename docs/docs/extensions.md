# Extensions

An extension is a small JSON file, written by someone else, that adds to your settings
without being part of them. The usual job is a profile: install a shell or a dev tool, and
it drops an extension that makes its profile appear in your list — you never edit
`settings.json`, and uninstalling the tool takes the profile away again.

These are sometimes called *fragment extensions*, because each one is a fragment of the
same settings format you would otherwise write by hand.

## What an extension may add

| It can | It cannot |
|---|---|
| Add a new profile | Change a global setting |
| Change settings on a profile that already exists | Add or change actions and key chords |
| Add a colour scheme | Add another extension |

The last column is the point: an extension is allowed to offer you a way to start a shell,
and nothing else. It cannot rebind your keys or alter how the Terminal behaves.

## Where they come from

The Extensions page lists each one with a **Scope** of *Current User* or *All Users*, which
is simply which folder it was found in:

- **Current User** — `%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\<source>\*.json`
- **All Users** — `%ProgramData%\Microsoft\Windows Terminal\Fragments\<source>\*.json`

The `<source>` folder name becomes the extension's name, and is also the `source` recorded
on any profile it adds. Apps from the Store use a third route: they declare a
`com.microsoft.windows.terminal.settings` app extension and keep a `Fragments` folder
inside their own package, so uninstalling the app removes its profile with it.

## Turning one off

Each extension has a toggle. Switched off, its profiles and schemes are not loaded at all —
the Terminal behaves as though the file were not there. This is the right control for a
profile you never use but cannot uninstall, and it survives the app that installed it being
updated.

The page also shows, per extension, what it actually did: **Added Profiles**, **Modified
Profiles** and **Added Colour Schemes**, each with a button that jumps to the profile or
scheme in question. That is the quick answer to "where did this profile come from".

## Profiles an extension used to provide

A profile whose `source` names an extension that is no longer installed becomes
*orphaned*. It stays in your settings, and the Terminal says so rather than silently
dropping it, because the command line you customised is still worth keeping. Delete it from
the Profiles page when you are sure.

## Writing one

The file is a JSON object with up to three keys — `profiles`, `schemes`, and nothing else
required:

```jsonc
{
    "profiles": [
        {
            "updates": "{574e775e-4f2a-5b96-ac1e-a2962a402336}",
            "font": { "face": "Cascadia Code" }
        },
        {
            "name": "Acme Shell",
            "commandline": "acmesh.exe",
            "icon": "ms-appx:///ProfileIcons/acme.png",
            "guid": "{2d5c8a4a-4d1d-4a1f-9c3a-0c3f7b8e1a21}"
        }
    ],
    "schemes": [ { "name": "Acme Dark", "background": "#101014", "foreground": "#D8D8DC" } ]
}
```

`updates` names an existing profile's GUID and layers on top of it. A profile with `name`
and `guid` but no `updates` is a new one. Relative and `ms-appx://` icon paths are resolved
against the extension's own folder, and a network path is refused.

Drop the file in the Current User folder above, restart the Terminal, and it appears on the
Extensions page. If it does not, the file failed to parse — the settings-load warning on
startup names which one.

## See also

- [Provenance marks](fork-marks.md) — the Extensions page is upstream's; the marks beside
  settings tell you which parts of the Terminal are not.
