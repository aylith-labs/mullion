# Keyboard shortcuts

A shortcut here is two things joined together: an **action** (what happens) and zero or
more **key chords** (what you press to make it happen). The Shortcuts page lists every
action the Terminal knows about, and lets you give each one the keys you want.

An action with no key chord is not broken. It still shows up in the command palette
(`Ctrl+Shift+P`) and can still be put on the new tab dropdown, so leaving a shortcut
unbound is a reasonable thing to do.

## Reading the list

Each row on the Shortcuts page is one action. The row shows its name, the chords currently
bound to it, and — when there are more chords than fit — *and N more*. Open a row to see
the action's own settings: which profile a `newTab` opens, which direction a `splitPane`
splits, how far a `scrollUp` scrolls.

Actions you have not touched are the defaults that ship with the Terminal. Editing one
does not destroy the default; it writes your version into `settings.json` alongside it.

## Adding and editing a chord

1. Open the action's row.
2. Press **Add shortcut** under *Shortcuts*.
3. Press the keys you actually want. The box records the chord rather than typing into it,
   so `Ctrl+Shift+T` is captured as a chord and not as three keystrokes.
4. Accept it.

A chord is one or more modifiers plus one key: `ctrl`, `shift`, `alt` and `win`, then a
letter, digit, function key, or a named key like `tab`, `space`, `pgdn`, `esc`, `plus`,
`period`. Modifiers alone are not a chord.

If the chord you pressed already belongs to another action, the Terminal says so and asks
whether to overwrite it. Answering yes moves the chord; the action that had it keeps
working and simply loses that way of reaching it.

> [!NOTE]
> A chord the operating system or another app has already claimed never reaches the
> Terminal. Windows keeps `Win+L`, `Ctrl+Alt+Del` and a handful of others for itself, and
> no setting here can take them back.

## Scan codes, for keyboards that disagree

A chord can name a physical key rather than the character printed on it, written as
`sc(N)` — `win+sc(41)` is the default for quake mode, and it means "the key below Esc"
whatever that key produces on your layout. Use this when a shortcut should sit in the same
*place* on every keyboard.

## Action IDs

Every action carries an **ID** — `Terminal.OpenNewTab`, `Terminal.FindText`,
`Terminal.ToggleVerticalTabs`. IDs are how the rest of the Terminal refers to an action
without repeating its definition:

- the **Dropdown Menu** page can put an action on the new tab menu by ID;
- a **link tooltip** rule can add a custom button that runs an action by ID;
- `keybindings` in `settings.json` maps a chord to an ID.

Your own actions get an ID too. Give a new shortcut a name and the Terminal derives an ID
from it; you can set one explicitly in `settings.json` if you want a stable handle.

## What this looks like in settings.json

Two lists, and they do different jobs. `actions` defines *what* can happen and gives each
one an ID. `keybindings` says which keys reach which ID.

```jsonc
"actions": [
    { "command": { "action": "newTab", "profile": "Ubuntu" }, "name": "Ubuntu here", "id": "User.UbuntuHere" }
],
"keybindings": [
    { "keys": "ctrl+shift+u", "id": "User.UbuntuHere" },
    { "keys": "ctrl+shift+w", "id": null }
]
```

Binding an ID to `null` removes a default chord without replacing it. That is the honest
way to free up a key the Terminal uses and you want for your shell.

## The command palette is not the same thing

`Ctrl+Shift+P` searches actions by name and runs one. It reaches everything on this page,
including actions with no chord at all, and it is usually faster than inventing a
shortcut for something you do twice a month.

## See also

- [Regular expressions](regular-expressions.md) — for the patterns the dropdown menu and
  link tooltip rules match with.
- [Link tooltips](link-tooltips.md) — custom tooltip buttons run actions by ID.
