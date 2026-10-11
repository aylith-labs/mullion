# Link tooltips

Hover a URL in the terminal and a card appears above it. At its simplest the card tells you
where the link really goes — which is the whole point, because the text of a link and its
destination are not obliged to agree. Beyond that it carries buttons for doing something
with the link, and, when an [integration](integrations.md) recognises it, a live preview of
what is behind it.

All of this lives on the **Link Tooltip** page, which has two halves: **Defaults**, which
apply to every link, and **Rules**, which change the defaults for links that match.

## What counts as a link

Three different things, and you can enable them separately under *Which links respond to a
click*:

- **Automatically detected URLs** — anything in the output that looks like a URL.
- **Links the program marked as links** — a program can mark text as a hyperlink with an
  OSC 8 escape sequence, which is how `ls --hyperlink` and many build tools produce
  clickable output.
- **Matches from your rules** — a rule of kind *Text* turns any pattern you like into a
  hoverable match, even though nothing in the output said it was a link. This is how an
  issue key such as `CAB-8209` becomes clickable.

## Defaults

| Setting | What it decides |
|---|---|
| Make links clickable | Whether a click follows a link at all. Off, hovering still highlights and still shows the tooltip — you pick an action from the card instead. |
| Primary action | The chord that follows a link, and what following it means. A left-click chord acts on release and only if the click did not become a selection, so dragging across a URL still selects it. |
| Alternative action | A second chord for doing something else with the same link. |
| Show buttons on the link tooltip | Turns the whole button row off. Which buttons, and which edge they sit on, are chosen inside. |
| Delay before the tooltip appears / disappears | In milliseconds. The hide delay is the time you have to move the pointer onto the card to use its buttons, so `0` puts them out of reach of the mouse. |
| Maximum width | How far a URI may run before it wraps, in device-independent pixels. `0` for no maximum. |
| Show which rule matched | Names the rule the card came from, and says so when no rule matched. Click the name to open that rule. |
| Show link details in a pane instead of a tooltip | Sends the details to a pane rather than a hover card. |

The built-in buttons are **Open**, **Copy link**, **Copy path**, **Show in Explorer** and
**Show in pane**. Which of them apply depends on the link: *Copy path* and *Show in
Explorer* appear for links that resolve to a file. An integration can add **Details**, which
shows its service's own full page for the link inside the card.

## Rules

A rule is a set of match criteria plus the settings it changes. **Rules higher in the list
win**: the first enabled rule that matches a link decides its tooltip and its click
actions. Nothing merges.

### How the list is ordered

By default the rules are grouped by **platform** — the service a rule is about — and sorted
by name inside each group. The platform is worked out from the rule's integration, name and
pattern, so a rule called "GitHub pull request" lands in the GitHub group without being told
to. The groups are drawn in a fixed order:

> GitHub · Unblocked · Jira · Slack · Stith · shefrd · git · files · custom

Because order is precedence, that order is doing real work. Unblocked sits above Jira
because a specific task ID has to be tried before Jira's broad `PROJECT-number` matcher
would swallow it. Rules the Terminal cannot place go into **custom**, which is last — so a
rule of your own never silently outranks a built-in one.

Turn on **Arrange rules by hand** and the groups collapse into one flat list you drag into
whatever order you want. Use it when you need a rule of your own to win over a grouped one.
**Expand all** and **Collapse all** work on the groups when automatic ordering is in effect.

### Matching

| Field | Effect |
|---|---|
| Match kind | **Link** matches detected and embedded hyperlinks. **Text** matches the terminal's output, turning what it finds into a hoverable match. |
| URI schemes | Comma-separated, e.g. `https, stith`. Empty matches any scheme. |
| Pattern | A [regular expression](regular-expressions.md). For a Link rule it is looked for anywhere inside the link and is case-insensitive; for a Text rule it is searched for in the output, case-sensitively, and whatever it finds becomes the match. Empty matches any text. |
| File type | Only applies to links that resolve to a file path. *None* does not constrain by type. |
| Custom file extensions | Comma-separated, e.g. `log, ini`. Merged with the file type group above. |

The file-type groups are Image, Video, Audio, Media, Source code, Document, Archive and
Executable — so "preview any image I hover" is one rule with no pattern at all.

### Overrides

Everything a rule changes is an **override**: tick the box to take the setting off the
defaults, leave it clear to inherit. Show delay, hide delay, maximum width, the button list
and pane-versus-tooltip all work this way, so a rule only states what is different.

Primary and alternative actions are per-rule too — *what* the chord does can change per
link, while the chord itself stays global. Set one to *Do nothing* to make a class of link
un-followable without turning clicking off everywhere.

### Presets

**Preset** fills a rule in from a catalogue of known services and file types: GitHub pull
requests, Jira keys, image files, source locations. Apply one, then edit what you want —
a preset is a starting point written into your rule, not a live link to anything.

**Duplicate rule** makes an independent copy of the current rule and its custom actions,
which is the quickest way to produce a near-identical rule for a second host.

### Custom buttons

A rule can add its own buttons. Each one is a label and an **action ID** — any action from
the [Shortcuts](keyboard-shortcuts.md) page. That is how a link gets a button that does
something only your setup knows how to do.

## Previews, and where they come from

If an [integration](integrations.md) recognises the link, the card grows a section below the
link target: the integration's name and icon, then fields fetched from that service. The
card shows a loading state while the fetch runs and caches the result, so a second hover is
instant.

- Not configured — the card says so and never contacts anything.
- A failed fetch — the error appears inline (`Jira: 401 Unauthorized`) and the card stays
  usable without the preview.

Local files get previews without any integration: Markdown renders with **Formatted / Raw**
buttons, source files get syntax colouring, and images show a thumbnail.

## Everything has an off switch

Nothing here is required. Turn *Make links clickable* off and you keep the tooltips; turn
*Show buttons on the link tooltip* off and you keep a plain "this is where it goes" card;
turn a rule off and it is as though it were not written.

## See also

- [Integrations](integrations.md) — what fetches a preview.
- [Regular expressions](regular-expressions.md) — writing a rule's pattern.
- [Provenance marks](fork-marks.md) — this whole page is specific to this Terminal.
