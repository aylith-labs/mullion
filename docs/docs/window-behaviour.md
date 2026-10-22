# Window behaviour

Two settings about how the application itself behaves rather than how the terminal inside it
looks: whether things animate, and where this settings page appears.

## Motion

**Appearance → Motion** decides whether tabs, panes, dialogs and settings pages animate.

| Choice | Behaviour |
|---|---|
| Follow Windows | Reads the system setting — Settings → Accessibility → Visual effects → Animation effects. |
| Always animate | Animations on, regardless of the system setting. |
| Never animate | Animations off, regardless of the system setting. |

*Follow Windows* is the default and is almost always the right answer: turning animations off
system-wide is a recognised accessibility need, and an app that ignores it is the problem.
The explicit options exist for the two cases the system setting cannot express — you want a
calm Terminal and animated everything else, or the reverse.

What this actually affects: tab open and close, pane splits, dialog entry, the settings
navigation, and the expand animation on a settings group. It does not affect anything the
program running inside a pane draws.

> [!TIP]
> If settings groups feel sluggish to open, this is the setting. *Never animate* makes an
> expander snap open instead of easing.

## Where settings open

**Appearance → Open settings in** decides what happens when you open this page.

| Choice | What you get |
|---|---|
| A tab | Settings as a tab in the current window. The default, and upstream's only behaviour. |
| A dialog over the window | Settings floating over your terminal, which stays visible behind it. Closes with `Esc`. |
| Its own window | A separate top-level window, with its own taskbar entry. |

Each suits a different habit:

- **A tab** is right if you treat settings as a place you go and come back from. It costs
  you a tab slot while it is open.
- **A dialog** is right for a quick change you want to *see* take effect — your terminal is
  still on screen behind it, so you can watch a colour scheme or a font size land. `Esc`
  dismisses it, so it costs nothing to open.
- **Its own window** is right for a long session of editing: you can put it on a second
  monitor, alt-tab to it, and resize it independently of the terminal.

The choice applies from the next time you open settings; it does not move a settings page
that is already open.

## See also

- [Tabs and the tab strip](tab-strip.md) — where the tabs sit and what they show.
- [Provenance marks](fork-marks.md) — both settings on this page are additions.
