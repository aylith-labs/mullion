# Tabs and the tab strip

Where the tabs sit, how wide they are, and what each one shows. These live under
**Appearance → Tab strip**.

## Tab position

Four choices, and the interesting ones are the sides.

| Position | What you get |
|---|---|
| Top | The usual strip along the top. |
| Bottom | The same strip, along the bottom. |
| Left | A **vertical sidebar** down the left, which you resize by dragging its inner edge. |
| Right | The same sidebar, on the right. |

The vertical sidebar is the reason to care. A tab in a horizontal strip gets a few
characters of title before it is cut off; a tab in a sidebar gets a whole line, so long
titles — branch names, working directories, agent sessions — stay readable. With many tabs
open the sidebar also scrolls sensibly, whereas a horizontal strip compresses.

> [!NOTE]
> Only **Top** can draw the tabs into the title bar. Choose Bottom, Left or Right and the
> window gets a normal title bar above the content, because there is no longer a tab strip
> up there to put in it.

There is an action for this — `toggleVerticalTabs`, bindable on the
[Shortcuts](keyboard-shortcuts.md) page — which flips between Top and Left. It deliberately
does not cycle all four: a binding whose effect depends on where you already are is
unpredictable to press. Bottom and Right are reachable here and in `settings.json`.

The action changes the position for the current session only; the next settings reload puts
it back. Set it here to make it stick.

## Tab width mode

- **Equal** — every tab the same width, shrinking as you open more.
- **Compact** — inactive tabs shrink to the size of the icon, so the active tab keeps its
  title.
- **Title length** — each tab is as wide as its own title needs.

Compact pairs well with Top; Title length pairs well with a sidebar.

## Profile icon on tabs

Whether the profile's icon appears on its tab: **Show in colour**, **Show in one colour**, or
**Hide**. Monochrome is the quiet option — you keep the at-a-glance distinction between
PowerShell and a WSL distribution without a row of coloured logos.

This setting **overrides the active theme**. A theme can set an icon style of its own; set
this and yours wins, leave it alone and the theme decides.

## Close button on tabs

**Always**, **On hover**, **Active tab only**, or **Never**.

Middle-clicking a tab closes it whichever one you choose, so *Never* removes the button you
keep hitting by accident without removing your ability to close a tab. This also overrides
the active theme.

## A note on themes

Both of the last two are window settings that sit *in front of* the theme. The reason is
practical: the Terminal does not write the built-in `dark`, `light` and `system` themes back
out to your settings file, so a value stored on one of those — which is to say, on the theme
nearly everyone is using — would apply for the session and silently vanish on the next
reload. An override persists.

One consequence worth knowing: the dropdown shows *your override's* value, not the resolved
one. The defaults match what the built-in themes give, so for anyone on a built-in theme the
control reads correctly. A custom theme that sets these will show its own value in the
terminal but the default here, until you touch the control — at which point your override
takes over and the two agree again.

## See also

- [Window behaviour](window-behaviour.md) — animations, and where the settings page opens.
- [Provenance marks](fork-marks.md) — which of these settings upstream has, and which it
  only lets you set in JSON.
