# Link previews and integration plugins

Hovering a link (or a piece of plain text that a rule recognizes) in the terminal can show a
card with live information pulled from an external tool — a GitHub pull request's state and
checks, a Jira issue's summary and status, a Slack message's author and text, a Stith session's
name and state. This is driven by **integration plugins**: small JSON manifests that describe how
to recognize a match, how to fetch data for it, and how to display the result. Six ship built in:
**GitHub**, **Jira**, **Slack**, **Stith**, **shefrd**, and **Unblocked Code**.

This document covers the feature end to end: what ships out of the box, the Integrations
settings page, text-pattern matching, the plugin manifest format for anyone writing their own,
and the security rules that keep credentials from leaking.

The manifest format is **Lintel**, shared with the Tabby fork and specified at
[`lintel.aylith.com`](https://lintel.aylith.com). A manifest that works here works there; the
canonical copies of the built-ins live in the `lintel` repo and are synced into this one.

## What a link preview is

Jira descriptions and comments retain tables and callouts. Comment identities use a round
40-pixel avatar with the name above the timestamp in the pane; popovers use a compact
single line. Code blocks have no line numbers and expose Copy and, when the line is wider
than the viewport, Wrap on hover or keyboard focus.

The ticket title stays above the tabs. Status and Change status sit together below it;
type, priority and parent links share that header. Type and priority open Jira searches.
Display fields can declare `placement: "header"` or `placement: "status"`, plus a `link`
template, so other integrations can use the same layout.

Embedded links and recognized identifiers use the same rules and integration provider as
buffer links. Panes enable their tooltips; showing tooltips inside a popover is opt-in,
with at most four nesting levels. The complete popover has a configurable maximum height.

Absolute POSIX paths on Windows use the producing WSL distribution. If it is unknown,
registered distributions are probed off the UI thread and only a unique existing match is
accepted. Explicit drive paths such as `Z:\home\stevenp\x.png` remain Windows paths.
Lintel owns the shared detection patterns, path policy, language mappings and icons.


Local Markdown previews offer **Formatted / Raw** buttons in both the hover card and pane.
Formatted mode displays YAML frontmatter as metadata and applies syntax colors to fenced
code. Invalid metadata retains its original YAML with an error. Raw mode preserves the
source; source-code file previews also use syntax highlighting.

Comment lists respect the tab's body format in both the hover card and pane. Jira's ADF
comments and GitHub's Markdown comments render emphasis, links, lists and code instead of
showing literal markup.

The file footer shows the full language or file type, a shared Lintel icon and readable size.
Lintel's `file-types.json` owns these names, icons, extensions and special filenames such as
Dockerfile. A source location like `Program.cs#L194` still matches the source-code rule.

Both preset menus have search. The rule editor's **Duplicate rule** button makes an
independent copy of the current rule and its actions. The shared catalog includes Unblocked
Code task IDs: `UNB-123` opens `https://getunblocked.com/dashboard/team/current/coding-task/UNB-123`.
Add its preset from Link Tooltip settings. To contribute a new preset without building a
terminal, use [Lintel's proposal form or contribution wizard](https://github.com/aylith-labs/lintel/blob/main/CONTRIBUTING.md).

Hover a hyperlink (or a text match — see below) that a plugin recognizes, and the hyperlink card
grows a section below the usual link target: an icon and name for the plugin, then a small set of
fields fetched from that tool. The card shows a loading state while the fetch is in flight, and
caches the result for the plugin's configured lifetime so a second hover is instant.

A card is not limited to a list of fields. A plugin can also declare:

- **[Field groups](#field-groups)** — named sets of fields ("Details", "Development") that the
  card keeps together and the settings page turns on and off as a unit.
- **[Tabs](#tabs)** — secondary content behind a small tab strip: a description rendered as
  Markdown or Atlassian Document Format, or a list of comments with author, avatar and time.
- **[Actions](#actions)** — something the card can *do* to the thing behind the link. Jira's
  transition picker is the worked example: choose a transition, press Apply, and the status badge
  changes.

The same preview can also be opened in a pane instead of a tooltip, via **Show in pane** on the
card — see [`panes.md`](panes.md) for the pane surface itself.

If the plugin isn't configured (missing a required setting or credential), the card shows a muted
"`<Plugin>`: not configured" line instead of attempting a fetch. If a fetch fails, the card shows
the error inline (for example, "`Jira: 401 Unauthorized`") — it stays usable, just without a
preview.

## Built-in plugins

| Plugin | Recognizes | Needs | Notes |
|---|---|---|---|
| **GitHub** | `https://github.com/<owner>/<repo>/pull/<n>`, `/issues/<n>` and `/commit/<sha>` links | Nothing, if the [GitHub CLI](https://cli.github.com) is signed in. Otherwise a personal access token (credential) | Tries `gh auth token` first and falls back to a stored token — see [Authenticating from a local CLI](#authenticating-from-a-local-cli-the-gh-then-token-pattern). Tabs for the description and comments; Details / Activity / Commit field groups. |
| **Jira** | `https://<host>/browse/<KEY>` links, and (opt-in) issue keys like `CAB-8209` in plain text | Site host (setting) + account email and API token (credentials) | Credentials are only ever sent to the configured host — see [Host guarding](#host-guarding). Create an API token at `id.atlassian.com` → Security → API tokens. Carries the transition [action](#actions), a Development [field group](#field-groups) fed by Jira's `dev-status` API, and Description / Comments [tabs](#tabs). |
| **Slack** | `https://<workspace>.slack.com/archives/<channel>/p<ts>` permalinks, including thread replies (`?thread_ts=`) | A bot token (credential) | The token needs the `channels:history`, `groups:history`, and `users:read` scopes, and the bot must be a member of the channel it's reading. |
| **Stith** | `stith://session/<id>`, `stith://focus/<id>`, `stith://copy/<id>`, `https://<server>/(s\|agent\|sessions\|embed/s)/<id>` links, and a **bare session id** printed with no scheme around it | Server URL (setting) | No credentials. The fetch allows an untrusted certificate, so a self-signed `lvh.me` cert doesn't block the preview. A [`detectPatterns`](#detectpatterns) entry makes a bare `stith://…` in plain output hoverable without OSC 8. Each verb is matched separately so a `focus` link is never read as a `session` one, and `focus` carries an [`open` action](#open-actions) that raises the terminal the session runs in. |
| **shefrd** | Multiplexer pane addresses like `w1N:p39`, and `shefrd://pane/<id>` links | Stith server URL (setting) | Reads and focuses through stith, which already holds a socket to every multiplexer server — so the same manifest works from Windows, from WSL, and from inside a pane, with no binary invoked directly. Clicking a pane id focuses that pane ([`open`](#open-actions)); the card shows its tab, space, agent and status. |

Each plugin's fields (which pieces of data show on the card, and in what order) can be trimmed on
the Integrations settings page — see below.

## The Integrations settings page

The Integrations page lists every discovered plugin (built-in and user-installed) with its
source and an enable/disable toggle. Opening a plugin shows:

- **Settings** — plain, non-secret values like a site host or server URL. These are stored in
  `settings.json` under `"integrations"` (see [Where configuration lives](#where-configuration-lives-in-settingsjson)).
- **Credentials** — a password box per credential field, with a "stored" indicator and a Clear
  button once one is saved. Saving writes straight to the Windows credential vault; it never
  touches `settings.json`.
- **Show in tooltip** — a checkbox per display field. Each box records only itself: a field nobody
  has touched stays at the manifest's `"default"`, and a box set back to that default stops being
  recorded at all. The order the fields appear in on the card is the manifest's, never the order the
  boxes were ticked. When the manifest declares [field groups](#field-groups), the checkboxes are
  grouped under a tri-state header checkbox that selects or clears the whole group; fields that
  belong to no group fall into an implicit "Details" group.
- **Tabs** — a checkbox per [tab](#tabs) the manifest declares. With no tab enabled the card shows
  no tab strip at all.
- **Text matching** — any of the plugin's *suggested* text matchers (see below), each with an
  **Add as rule** button that creates a matching Link Tooltip rule for you.
- **Source** — read-only, either `built-in` or the path of the manifest file it was loaded from.

### Where configuration lives (in `settings.json`)

Only non-secret settings and field selections are persisted to `settings.json`, under a top-level
`"integrations"` object keyed by plugin id:

```jsonc
"integrations": {
    "jira": {
        "enabled": true,
        "settings": { "host": "acme.atlassian.net" },
        "fields": { "devPullRequests": true, "updated": false },
        "tabs": { "comments": true }
    }
}
```

- `enabled` — whether the plugin is used for matching and preview at all.
- `settings` — the plugin's non-secret setting values, keyed by the manifest's setting `key`.
- `fields` — the display fields the user turned **on or off**, keyed by field key. `true` shows one
  the manifest hides by default; `false` hides one it shows.
- `tabs` — the same, for [tabs](#tabs).

**A key that is not mentioned takes the manifest's own `"default"`.** That is the point of the
shape rather than a list of "the fields to show": a field a manifest gains later has never been put
in front of anyone, so it arrives at its default instead of being read as one the user declined. A
list could not say that — absence in a list meant "turned off" and "did not exist yet" at the same
time, so every new `"default": true` field was invisible to everyone who had ever ticked a box.

Only decisions are stored. Setting a box back to what the manifest says **erases** the key rather
than writing it explicitly, so the file stays the size of what the user actually changed, and a
manifest that later changes its own default is followed rather than overruled by a value nobody
chose.

The older form — `"fields": [ "summary", "status" ]`, the complete set of keys to show — is still
read, so an existing `settings.json` keeps working. It is converted to overrides the first time a
box is ticked. Because that form cannot distinguish a field the user hid from one that did not
exist when it was written, the conversion keeps only the keys it names: a field you had hidden
reappears once, at its manifest default.

### Where credentials live

Credentials are **never** written to `settings.json`. They're stored in the Windows credential
vault (Credential Manager → Web Credentials), one vault "resource" per plugin, one credential per
field key:

```
Resource: WindowsTerminal/Integrations/<plugin id>
User name: <credential key>       e.g. "email", "token"
```

For example, saving Jira's email and token creates two Web Credentials entries under
`WindowsTerminal/Integrations/jira`. The vault is per Windows user, so both Terminal build slots
(and every Terminal built from this repo) see the same stored credentials — that's intended, not
a leak.

## Text matching

Link previews aren't limited to actual hyperlinks. A **Link Tooltip rule** with `"match": "text"`
turns an arbitrary ICU regular expression into a hoverable, clickable match anywhere it appears in
terminal output — not just inside a real link.

For example, Jira's built-in text matcher recognizes issue keys like `CAB-8209` in plain output.
Once a text rule for that pattern is enabled, any matching text becomes hoverable: the card shows
the same Jira preview a real `/browse/` link would, Open navigates to the issue, and Copy link
copies its URL.

Rules of the road:

- Text patterns are scanned over the visible viewport (± one page) on every output-idle rescan,
  same as the built-in URL and file-path detectors.
- Up to **16** text patterns can be active at once.
- The **first enabled rule** (in list order) whose criteria match wins — overrides from multiple
  matching rules are not merged.
- A plugin's suggested text matcher can be turned into a rule with one click, via **Add as rule**
  on the Integrations page. That's the easiest way to enable one; a rule can also be written by
  hand on the Link Tooltip page.

### Link Tooltip rule fields relevant to previews

A `HyperlinkTooltipRule` (Link Tooltip page, `settings.json` under a rules array) has these
preview-related keys:

| JSON key | Values | Meaning |
|---|---|---|
| `match` | `"link"` (default) \| `"text"` | Whether `pattern` runs against a detected/OSC 8 hyperlink's URI, or is scanned over plain terminal text. |
| `pattern` | regex string | Link rules: an ECMAScript regex searched in the link text. Text rules: an ICU regex scanned over the buffer; the whole match becomes the hoverable text. |
| `integration` | `""` (auto) \| `"none"` \| a plugin id | Which plugin previews a match. Empty lets the plugins' own matchers decide; `"none"` disables preview for links this rule matches. |
| `preview` | boolean, default `true` | Whether to show a preview at all for links/text this rule matches. |
| `buttons` | array of string | Which buttons the card shows for this rule: `open`, `copyLink`, `copyPath`, `reveal` (Show in Explorer), `showInPane`. An empty or absent list inherits the global `hyperlink.tooltipButtons`. |
| `showInPane` | boolean, unset = inherit | Open the preview in a pane instead of a tooltip for this rule. Unset inherits the global `hyperlink.previewInPane`. |
| `primaryAction` | `""` (inherit) \| `"none"` \| an action id | What the primary click chord does for links this rule matches. |
| `alternativeAction` | `""` (inherit) \| `"none"` \| an action id | The same, for the alternative chord. Chords themselves are global — only the action is per-rule. |

These global settings back them, in `settings.json` alongside the rules:

| JSON key | Default | Meaning |
|---|---|---|
| `hyperlink.tooltipButtons` | `["copyLink", "showInPane"]` | The buttons every rule shows unless it overrides them. **Open is off by default** — a card is for looking, and the link is still clickable. |
| `hyperlink.tooltipHint` | `true` | Whether the card ends with the "Click to follow link" line. Hidden anyway when `hyperlink.clickable` is off. |
| `hyperlink.previewInPane` | `false` | Hovering opens the preview in a pane rather than a card. |
| `hyperlink.manualRuleOrder` | `false` | Leave the rules in the order you arranged them (and let the settings UI drag them) rather than grouping and sorting the list. |

## Clicking a link

Detection and clicking are separate. `experimental.detectURLs` governs only the two built-in URL
regexes — rules keep matching when it is off — and `hyperlink.clickable` governs whether a click
activates a link at all. With clicking off, links are still detected, highlighted and previewed;
the card's buttons are how you act on one.

Two chords are configurable, each a modifier plus a mouse gesture plus the action it runs:

| JSON key | Default | Values |
|---|---|---|
| `hyperlink.clickable` | `true` | boolean |
| `hyperlink.clickableKinds` | `["detected", "rules", "osc8"]` | which kinds of link a click reaches |
| `hyperlink.primaryClickModifier` | `"none"` | `none`, `ctrl`, `alt`, `shift`, `win`, `ctrlAlt`, `ctrlShift`, `altShift`, `ctrlAltShift` |
| `hyperlink.primaryClickGesture` | `"left"` | `left`, `middle`, `double` |
| `hyperlink.primaryAction` | `"open"` | a button id, an `actions` entry id, or `none` |
| `hyperlink.alternativeClickModifier` | `"ctrl"` | as above |
| `hyperlink.alternativeClickGesture` | `"left"` | as above |
| `hyperlink.alternativeAction` | `"open"` | as above |

An action id is one of the card's own button ids (`open`, `copyLink`, `copyPath`, `reveal`,
`showInPane`) or the id of an entry under `actions`, which is invoked with the hovered link
available as `%u` — so binding a chord to a button's id does exactly what pressing that button
does. Modifiers are matched exactly, so `ctrl` does not fire while Ctrl+Shift is held. A `left`
chord resolves on mouse release and only if the click did not become a selection, so dragging
across a URL still selects it; `middle` and `double` resolve on press. `alt` and `shift` also
start block and extended selections — on a link the chord wins, and selection is untouched
everywhere else.

**Show in pane** puts the same preview — header, tabs, field groups, actions — into a real pane
with room to read it. **Pane only — follow hovered links here** immediately updates the existing
pane as the pointer crosses links, suppressing hover cards in that tab. **Pin link** is separate:
other hovered links temporarily replace the pinned content, and leaving restores it. Without a
pin, the last hovered link stays visible. Explicitly choosing **Show in pane** clears the pin.
Closing the pane restores hover cards. See [`panes.md`](panes.md).

Zero-count detail rows are omitted from popovers and remain muted in the full pane. Jira avatars
use the largest standard avatar URL (48×48); a field named Parent never selects GitHub's commit UI.

## Writing a plugin manifest

A plugin is one folder containing `integration.json`. User plugins live under:

```
%LOCALAPPDATA%\Microsoft\Windows Terminal\Integrations\<folder>\integration.json
```

(A bare `<name>.json` directly in that `Integrations` folder is also picked up.) Discovery order
is: built-ins first, then the user directory — **a user manifest with the same `id` replaces a
built-in of the same id**, so a plugin can be forked, edited, and dropped in place of a shipped
one. `IntegrationRegistry::Refresh()` re-scans on every settings reload; a manifest that fails to
parse is skipped and logged, never fatal.

### Full annotated example (Jira)

```jsonc
{
    "id": "jira",
    "name": "Jira",
    "icon": "",
    "version": 1,
    "cacheSeconds": 300,

    // Plain values the user fills in on the Integrations page. Stored in settings.json.
    "settings": [
        {
            "key": "host",
            "label": "Site host",
            "placeholder": "acme.atlassian.net",
            "description": "The Jira Cloud site (or Jira Server host) whose links to preview. Credentials are only ever sent to this host.",
            "required": true
        }
    ],

    // Secret values. Stored in the Windows credential vault, never in settings.json.
    // A credential is treated as secret unless its key is "email", "user", or "username",
    // or it explicitly sets "secret": false.
    "credentials": [
        { "key": "email", "label": "Account email", "description": "The Atlassian account the API token belongs to.", "secret": false },
        { "key": "token", "label": "API token", "description": "Create one at id.atlassian.com -> Security -> API tokens.", "secret": true }
    ],

    // How this plugin claims a link or a piece of text.
    "matchers": [
        {
            "kind": "link",
            "pattern": "^https?://[^/]+/browse/(?<key>[A-Z][A-Z0-9]+-\\d+)",
            // The hovered URI's host must equal the "host" setting's value, or this
            // matcher does not fire -- see "Host guarding" below.
            "hostSetting": "host"
        },
        {
            "kind": "text",
            "pattern": "\\b(?<key>[A-Z][A-Z0-9]{1,9}-\\d{1,7})\\b",
            // Offered on the Integrations page as "Add as rule".
            "suggested": true,
            "description": "Issue keys in output, like CAB-8209",
            // How a text match becomes a URL for Open / Copy link / click.
            "link": "https://{{settings.host}}/browse/{{key}}"
        }
    ],

    // The request(s) that fetch preview data. Steps run in order; a later step can
    // read an earlier step's result through {{stepId:/json/pointer}}.
    "fetch": [
        {
            "id": "issue",
            "type": "http",
            "method": "GET",
            "url": "https://{{settings.host}}/rest/api/3/issue/{{key}}?fields=summary,status,resolution,assignee,priority,issuetype,updated,reporter,description,parent",
            "auth": { "type": "basic", "user": "{{credentials.email}}", "password": "{{credentials.token}}" },
            "headers": { "Accept": "application/json" },
            "timeoutMs": 8000
        },
        // The Comments tab, and nothing else, needs this -- so it is not requested
        // until the tab is opened. See "Lazily fetched tabs" below.
        { "id": "comments", "optional": true, "deferred": true, "url": "https://{{settings.host}}/rest/api/3/issue/{{key}}/comment?maxResults=50", /* … */ },
        // The options for the transitions action, below.
        { "id": "transitions", "optional": true, "url": "https://{{settings.host}}/rest/api/3/issue/{{key}}/transitions?expand=transitions.fields", /* … */ },
        // Jira's Development panel. Undocumented and not present on every site,
        // hence "optional"; both need the numeric issue id the first step returned.
        { "id": "devsummary", "optional": true, "when": "{{issue:/id}}", "url": "https://{{settings.host}}/rest/dev-status/latest/issue/summary?issueId={{issue:/id}}", /* … */ },
        { "id": "devdetail",  "optional": true, "when": "{{issue:/id}}", "url": "https://{{settings.host}}/rest/dev-status/latest/issue/detail?issueId={{issue:/id}}&applicationType=GitHub&dataType=pullrequest", /* … */ }
    ],

    // Which fields belong together, for the card and for the settings checkboxes.
    "fieldGroups": [
        { "key": "details",     "label": "Details",     "fields": [ "summary", "parent", "issueKey", "status", "resolution", "type", "priority", "assignee", "reporter", "updated" ] },
        { "key": "development", "label": "Development", "fields": [ "devBranches", "devCommits", "devPullRequests", "devPullLinks", "devBuilds", "devDeployments" ] }
    ],

    // The card's field list. "default": true fields show unless the user picks a
    // different set on the Integrations page. With more than one fetch step,
    // every path has to name its step: an unqualified path reads whichever step
    // ran last, which is not the one you meant.
    "fields": [
        { "key": "summary",  "label": "Summary",  "path": "issue:/fields/summary",              "kind": "title",  "default": true },

        // The breadcrumb above the title: the parent first, then this issue, with
        // each one's work-type icon. The `/` between them is the card's, not the
        // manifest's -- see "placement" in the display-field reference.
        { "key": "parent",   "label": "Parent",   "path": "issue:/fields/parent/key",           "kind": "text",   "placement": "breadcrumb",
          "iconPath": "issue:/fields/parent/fields/issuetype/iconUrl", "link": "https://{{settings.host}}/browse/{{issue:/fields/parent/key}}", "default": true },
        { "key": "issueKey", "label": "Key",      "path": "issue:/key",                         "kind": "text",   "placement": "breadcrumb",
          "iconPath": "issue:/fields/issuetype/iconUrl", "link": "https://{{settings.host}}/browse/{{issue:/key}}", "default": true },

        { "key": "status",   "label": "Status",   "path": "issue:/fields/status/name",          "kind": "badge",  "placement": "status", "colorPath": "issue:/fields/status/statusCategory/colorName", "default": true },
        { "key": "type",     "label": "Type",     "path": "issue:/fields/issuetype/name",       "kind": "text",   "iconPath": "issue:/fields/issuetype/iconUrl", "default": true },
        { "key": "priority", "label": "Priority", "path": "issue:/fields/priority/name",        "kind": "text",   "iconPath": "issue:/fields/priority/iconUrl", "default": true },
        { "key": "assignee", "label": "Assignee", "path": "issue:/fields/assignee/displayName", "kind": "text",   "iconPath": "issue:/fields/assignee/avatarUrls/48x48", "default": true },
        { "key": "reporter", "label": "Reporter", "path": "issue:/fields/reporter/displayName", "kind": "text",   "iconPath": "issue:/fields/reporter/avatarUrls/48x48", "default": false },
        { "key": "updated",  "label": "Updated",  "path": "issue:/fields/updated",              "kind": "text",   "format": "relativeTime", "default": true },

        { "key": "devBranches",     "label": "Branches",      "path": "devsummary:/summary/branch/overall/count",      "kind": "text", "default": false },
        { "key": "devCommits",      "label": "Commits",       "path": "devsummary:/summary/repository/overall/count",  "kind": "text", "default": false },
        { "key": "devPullRequests", "label": "Pull requests", "path": "devsummary:/summary/pullrequest/overall/count", "kind": "text", "default": false },
        // One row per pull request, rather than a count of them. See "Repeating a
        // field over an array".
        { "key": "devPullLinks",    "label": "Pull request",  "each": "devdetail:/detail/0/pullRequests",
          "path": "item:/name", "linkPath": "item:/url", "kind": "link", "default": true }
    ],

    // Secondary content behind a tab strip. See "Tabs" below.
    "tabs": [
        { "key": "description", "label": "Description", "kind": "body", "path": "issue:/fields/description", "format": "adf", "default": true },
        { "key": "comments",    "label": "Comments",    "kind": "list", "step": "comments", "path": "comments:/comments", "format": "adf",
          "itemAuthorPath": "/author/displayName", "itemAvatarPath": "/author/avatarUrls/48x48",
          "itemBodyPath": "/body", "itemTimePath": "/created", "default": false }
    ],

    // Something the card can do, not just show. See "Actions" below for the
    // full version of this one.
    "actions": [
        { "key": "transition", "label": "Move to", "kind": "choice", "optionsPath": "transitions:/transitions", /* … */ }
    ]

    // "html": "…" — see "HTML representation" below.
    // "embed": "{{settings.baseUrl}}/embed/s/{{id}}" — see "Embed pages" below.
}
```

### Manifest reference

Top-level keys:

| Key | Type | Meaning |
|---|---|---|
| `id` | string, required | Unique plugin id. A missing `id` makes the whole manifest invalid. |
| `name` | string | Display name. Defaults to `id` if omitted. |
| `icon` | string | Icon shown next to the plugin's name. |
| `version` | number | Manifest schema version, informational. |
| `cacheSeconds` | number | How long a fetched preview is cached per matched text/link. Negative values are clamped to `0`. |
| `settings` | array of [field](#settings--credentials-fields) | Non-secret configuration, stored in `settings.json`. |
| `credentials` | array of [field](#settings--credentials-fields) | Secret configuration, stored in the credential vault. |
| `matchers` | array of [matcher](#matchers) | How the plugin claims a link or text match. |
| `fetch` | array of [fetch step](#fetch-steps) | The request pipeline that produces preview data. |
| `fields` | array of [display field](#display-fields) | How the fetched result renders on the card. |
| `fieldGroups` | array of [field group](#field-groups) | Groups the display fields into named sets for the card and the settings checkboxes. |
| `tabs` | array of [tab](#tabs) | Secondary content — a description, a comment list — shown behind a tab strip. |
| `actions` | array of [action](#actions) | Things the card can do to the thing behind the link. |
| `detectPatterns` | array of ICU regex | Patterns the terminal scans plain output for while this plugin is enabled, so text it owns becomes hoverable without OSC 8. See [detectPatterns](#detectpatterns). |
| `html` | string | A full HTML document string, rendered in place of `fields`. See [HTML representation](#html-representation). |
| `embed` | string | URL template of the service's own page for the matched thing, offered on the card and in the pane as **Show *name* page**. See [Embed pages](#embed-pages-show-name-page). |

#### Settings / credentials fields

| Key | Type | Meaning |
|---|---|---|
| `key` | string | Referenced from templates as `{{settings.<key>}}` / `{{credentials.<key>}}`. |
| `label` | string | Shown on the Integrations page. Defaults to `key` if omitted. |
| `placeholder` | string | Placeholder text for the input box. |
| `description` | string | Help text shown under the field. |
| `required` | boolean | Settings only, in effect: a plugin isn't fetched from until every `required` setting and credential has a value. |
| `secret` | boolean | Credentials only. Defaults to `true` unless the key is `email`, `user`, or `username` — those default to visible, non-masked text. |

#### Matchers

| Key | Type | Meaning |
|---|---|---|
| `kind` | `"link"` \| `"text"` | Whether `pattern` runs against a hovered link's URI, or is scanned over terminal text. |
| `pattern` | ICU regex | Named capture groups (`(?<name>…)`) become template variables. |
| `hostSetting` | string | **Link matchers only.** The URI's host must equal the named setting's current value, or this matcher does not fire. See [Host guarding](#host-guarding). |
| `link` | template string | How a match turns into a URL for Open / Copy link / click. Text matchers need one to be clickable at all; a link matcher uses one to say what the *openable* form of a scheme it owns is. |
| `open` | string | What a click should **do**, when following a URL is the wrong answer. Names an `actions` entry of this manifest, or a built-in button id. See [open actions](#open-actions). |
| `suggested` | boolean | **Text matchers only.** Offered on the Integrations page's "Add as rule" list. |
| `description` | string | Shown next to a suggested text matcher. |

##### Open actions

Some things are not usefully "opened". A multiplexer pane id names a pane to bring to the front; a
web page describing that pane is a consolation prize. `open` says so:

```jsonc
{ "kind": "text", "pattern": "\\b(?<pane>w[0-9A-Z]+:p[0-9A-Z]+)\\b", "open": "focus" }
```

Clicking a match runs that action instead of following a URL. Copy link is unaffected — it still
copies what `link` resolves to — and if the action fails, the click falls back to opening `link`
rather than doing nothing.

This is also how a manifest owns a **scheme** without an OS protocol handler. `stith://focus/<id>`
handed to `ShellExecute` needs something registered for `stith:` that implements *that verb*; when
one is registered and answers only some of the verbs, the rest come back `SE_ERR_NOASSOC` and the
user is told the link is invalid. With `open`, the terminal runs the manifest's own request and
never consults the shell.

#### Fetch steps

| Key | Type | Meaning |
|---|---|---|
| `id` | string | Referenced by later steps and by field `path`s as `<id>:<pointer>`. |
| `type` | `"http"` (default) \| `"command"` | `http` issues a request; `command` runs a local process and parses its stdout as JSON. |
| `url` | template string | `http` only. |
| `method` | string | `http` only. Defaults to `GET`. |
| `headers` | object of string→template string | `http` only. |
| `auth` | object | `http` only. `{ "type": "basic", "user": "…", "password": "…" }`, `{ "type": "bearer", "value": "…" }`, or `{ "type": "header", "header": "X-Api-Key", "value": "…" }`. All of `user`/`password`/`value` are templates. |
| `body` | template string | `http` only. |
| `allowUntrustedCertificate` | boolean | `http` only. Accepts an invalid/self-signed TLS certificate for this request. |
| `commandLine` | template string | `command` only. |
| `stdin` | template string | `command` only, optional. |
| `timeoutMs` | number | Both. Defaults to `8000` when unset or ≤ 0. |
| `when` | template string | Run this step only if the template expands to a non-empty string. |
| `unless` | template string | Skip this step if the template expands to a non-empty string. |
| `optional` | boolean | A failing step is recorded and stepped over instead of ending the fetch. See [Optional steps](#optional-steps). |
| `deferred` | boolean | Not run by the fetch that paints the card. Run when a tab naming this step (`"step": "<id>"`) is first opened. See [Lazily fetched tabs](#lazily-fetched-tabs). |

Slack's manifest uses `when`/`unless` on a shared step `id` (`"message"`) to pick between a
top-level `conversations.history` call and a `conversations.replies` call, depending on whether
the link carried a `thread_ts`:

```jsonc
"fetch": [
    { "id": "message", "unless": "{{thread}}", "url": "…conversations.history…" },
    { "id": "message", "when": "{{thread}}",    "url": "…conversations.replies…" },
    { "id": "user",    "when": "{{message:/messages/-1/user}}", "url": "…users.info?user={{message:/messages/-1/user}}" }
]
```

Note the repeated `id`: two steps can share one id, and the later one that actually runs overwrites
the earlier result. That is what lets a `path` like `message:/messages/-1/text` resolve the same
way whichever branch fired.

##### Optional steps

The fetch pipeline normally stops at the first step that fails, and the card shows that error. A
step marked `"optional": true` is different: its failure is recorded and the pipeline carries on
without it. Anything that reads its result simply resolves to nothing, and the fields that depend
on it are skipped — an empty value never renders a row.

Reach for it in two situations:

- **An endpoint that may not exist.** Jira's `dev-status` API is undocumented and can return an
  error on any given site, so both of Jira's Development steps are optional. A site without it
  still gets a working card, just without the Development group.
- **A probe that is allowed to come back empty.** GitHub's `gh auth token` step is optional
  because "the CLI isn't installed" and "the CLI isn't signed in" are both ordinary outcomes, not
  failures.

##### Authenticating from a local CLI (the gh-then-token pattern)

A `command` step's stdout must parse as JSON, so a CLI that prints a bare token needs a wrapper.
GitHub's manifest runs one:

```jsonc
{
    "id": "ghtoken",
    "type": "command",
    "optional": true,
    "commandLine": "pwsh -NoProfile -NonInteractive -Command \"@{ token = (gh auth token 2>$null | Out-String).Trim() } | ConvertTo-Json -Compress\"",
    "timeoutMs": 4000
}
```

Everything after it comes in pairs that differ only in where the bearer token comes from, gated so
exactly one of each pair runs:

```jsonc
{ "id": "item", "when": "{{ghtoken:/token}}", "unless": "{{isissue}}{{sha}}",
  "url": "…/pulls/{{number}}", "auth": { "type": "bearer", "value": "{{ghtoken:/token}}" } },
{ "id": "item", "when": "{{ispull}}",         "unless": "{{ghtoken:/token}}",
  "url": "…/pulls/{{number}}", "auth": { "type": "bearer", "value": "{{credentials.token}}" } }
```

Three details make this work:

- **`when` is a presence test, not an expression.** There is no `and`, so the two conditions are
  split across the two slots a step has: the CLI variant puts "a CLI token exists" in `when` and
  the *wrong* link kinds in `unless`; the token variant puts the *right* link kind in `when` and
  "a CLI token exists" in `unless`.
- **Concatenation is an `or`.** `"{{isissue}}{{sha}}"` is non-empty when the link was an issue *or*
  a commit, because only one matcher fires per hover and the other groups expand to nothing. The
  same trick builds one URL out of two mutually exclusive values:
  `…/commits/{{sha}}{{item:/head/sha}}/check-runs` uses the hovered commit's sha for a commit link
  and the pull request's head sha for a pull link.
- **A marker group is just a named group over a literal.** GitHub's matchers capture
  `(?<ispull>pull)` and `(?<isissue>issues)` from the path purely so later steps have something to
  test. An earlier step's *result* works as a discriminator too: `{{item:/head/sha}}` is present
  only for pull requests, which is how the reviews step knows not to run for an issue.

If the CLI is missing, `pwsh` is missing, or the user is signed out, the probe step fails, it is
optional so the fetch continues, and `{{ghtoken:/token}}` stays empty — which is exactly the
condition the stored-token variants wait for. Nothing has to detect the failure explicitly.

#### Display fields

| Key | Type | Meaning |
|---|---|---|
| `key` | string | Identifies the field for the user's field-selection list. Defaults to `label` if omitted. |
| `label` | string | Shown next to the value. |
| `path` | JSON pointer | Where in a fetch step's result this field's value lives. See [Paths](#paths-json-pointers). |
| `kind` | see below | How the value renders. |
| `iconPath` | JSON pointer | A small (≤ 24 px) icon shown with the value. A `.svg` URL is rasterized; anything else is decoded as a bitmap. |
| `each` | JSON pointer | A pointer to an **array**. The field then renders once per element, up to ten. See [Repeating a field](#repeating-a-field-over-an-array). |
| `placement` | `"status"` \| `"header"` \| `"breadcrumb"` \| unset | Where on the card the value goes instead of into the labelled field list. `status` is the badge beside the status dropdown; `breadcrumb` is the trail of ids above the title, joined with `/` in manifest order; `header` is a chip row under it. Unset is an ordinary labelled row. |
| `link` | template string | Where the value goes when clicked, built out of pieces. Every substitution that came off the wire is percent-encoded, which is what makes it safe to paste a key into a query. |
| `linkPath` | JSON pointer | Where the value goes when clicked, when the result already carries the whole URL. Not escaped, and it wins over `link`. A pull request's own `url` needs this: run through `link` its scheme would come back as `https%3A%2F%2F`. |
| `colorPath` | JSON pointer | A JSON pointer to a color value, for `badge` fields. |
| `color` | string | A literal color (`"#rrggbb"` or a name — see below), for `badge` fields. It is the **fallback**: `colorPath` is resolved first, and `color` is used only when that pointer yields nothing. |
| `format` | `"relativeTime"` \| `"date"` \| unset | Formats the raw value: `relativeTime` turns an ISO 8601 timestamp into "3 h ago"; `date` renders it as a date. |
| `default` | boolean | Whether the field is shown when the user has expressed no opinion about this key. It is a real default, not a first-run seed: see [Where configuration lives](#where-configuration-lives-in-settingsjson). |

`kind` values: `text`, `title` (bold, first line), `subtitle`, `badge` (a colored pill), `link`,
`image` (≤ 24 px, from `iconPath`), `multiline` (up to 6 lines).

A field whose value resolves to an empty string is **not rendered at all** — no label, no row. That
is what lets one field list serve several link shapes: GitHub's `additions`/`deletions` fields sit
in the manifest unconditionally and simply vanish on an issue link, which has no such numbers.

##### Repeating a field over an array

A count is not a link. Jira's dev-status endpoint knows how many pull requests an issue has *and*
what they are called and where they live, and "Pull requests 2" is the least useful of those three.
`each` turns one field declaration into one row per array element:

```jsonc
{
    "key": "devPullLinks", "label": "Pull request",
    "each": "devdetail:/detail/0/pullRequests",
    "path": "item:/name",
    "linkPath": "item:/url",
    "kind": "link", "default": true
}
```

Inside such a field, **`item:` is the element under construction** — in `path`, `iconPath`,
`colorPath`, `linkPath` and in a `{{…}}` template. It is an ordinary step id as far as every reader
is concerned, which is why no new syntax is needed. A manifest that already has a step called
`item` (GitHub's does) keeps it: the name is shadowed for the length of the loop and put back
afterwards.

The field is one entry in the user's field list however many rows it produces, and at most ten rows
are drawn — a card is not a table.

Badge colors are matched case-insensitively. `#rrggbb` is taken literally; otherwise a small set of
names is recognized, chosen to cover Jira's `statusCategory.colorName` values and the obvious status
words:

| Renders as | Names |
|---|---|
| Green | `green`, `success`, `done`, `live`, `running`, `active` |
| Amber | `yellow`, `inprogress`, `in progress`, `warning`, `waiting`, `idle` |
| Red | `red`, `error`, `failed`, `blocked`, `dead`, `stale` |
| Blue | `blue`, `info`, `new`, `open` |
| Gray | anything else, including Jira's `blue-gray` |

#### Field groups

A `fieldGroups` entry names a set of display fields that belong together. The card and the pane
render them under the group's label, and the Integrations settings page gives the group a tri-state
header checkbox that selects or clears all of its fields at once.

| Key | Type | Meaning |
|---|---|---|
| `key` | string | Identifies the group. |
| `label` | string | Heading shown on the card and in settings. |
| `fields` | array of string | Display field `key`s, in the order the group shows them. |

```jsonc
"fieldGroups": [
    { "key": "details",     "label": "Details",     "fields": [ "summary", "status", "assignee", "updated" ] },
    { "key": "development", "label": "Development", "fields": [ "devBranches", "devCommits", "devPullRequests" ] }
]
```

Grouping is presentation only: a field's `key` still has to appear in `fields`, and a field named by
no group falls into an implicit "Details" group. A group whose fields all resolve to empty values
does not render.

#### Tabs

A tab is secondary content — too long for a field row — shown behind a compact tab strip above the
field list. Two shapes:

- **`"kind": "body"`** renders one value as a block of text. `path` points at that value.
- **`"kind": "list"`** repeats an author/avatar/body/time row over an array. `path` points at the
  array, and the four `item*Path` pointers are relative to *each element* of it.

| Key | Type | Meaning |
|---|---|---|
| `key` | string | Identifies the tab for the user's tab-selection list. |
| `label` | string | The tab's caption. |
| `kind` | `"body"` (default) \| `"list"` | One long value, or a repeating list. |
| `path` | JSON pointer | `body`: the value. `list`: the array to repeat over. |
| `format` | `"markdown"` \| `"adf"` \| `"text"` | How to interpret the body text. `markdown` goes through the Terminal's Markdown renderer; `adf` is [Atlassian Document Format](https://developer.atlassian.com/cloud/jira/platform/apis/document/structure/), a JSON document that is flattened to text; `text` is used verbatim. |
| `itemAuthorPath` | JSON pointer | `list` only, relative to each element. |
| `itemAvatarPath` | JSON pointer | `list` only, relative to each element. |
| `itemBodyPath` | JSON pointer | `list` only, relative to each element. |
| `itemTimePath` | JSON pointer | `list` only, relative to each element. |
| `step` | string | The `deferred` fetch step this tab's content comes from. See [Lazily fetched tabs](#lazily-fetched-tabs). |
| `default` | boolean | Whether the tab is offered when the user has expressed no opinion about this key, exactly as a display field's `default` works. |

```jsonc
"tabs": [
    {
        "key": "description", "label": "Description", "kind": "body",
        "path": "issue:/fields/description", "format": "adf", "default": true
    },
    {
        "key": "comments", "label": "Comments", "kind": "list",
        "path": "issue:/fields/comment/comments", "format": "adf",
        "itemAuthorPath": "/author/displayName",
        "itemAvatarPath": "/author/avatarUrls/24x24",
        "itemBodyPath": "/body",
        "itemTimePath": "/created",
        "default": false
    }
]
```

When the array a `list` tab needs *is* the whole result of a step — GitHub's issue-comments endpoint
returns a bare JSON array — point at the step with an empty pointer: `"path": "comments:"`. An empty
JSON pointer means "the whole document", per RFC 6901.

##### Lazily fetched tabs

**No tab's content is built until the user opens it.** The fetch produces a strip of names; the
first visit to one asks for its content, and the answer is kept, so the second visit is immediate.
That is free for a tab whose data is already in hand — the step results the card was built from are
cached beside the preview — and it is what keeps flattening fifty comment bodies off the path that
paints a hover card.

A tab whose content is not in hand at all goes further. Mark the step `deferred` and name it from
the tab:

```jsonc
"fetch": [
    { "id": "issue", "url": "…/issue/{{key}}?fields=summary,status,…" },
    { "id": "comments", "optional": true, "deferred": true,
      "url": "…/issue/{{key}}/comment?maxResults=50" }
],
"tabs": [
    { "key": "comments", "label": "Comments", "kind": "list",
      "step": "comments", "path": "comments:/comments", "format": "adf",
      "itemAuthorPath": "/author/displayName", "itemBodyPath": "/body" }
]
```

The card's own fetch skips that step entirely. Opening the tab runs it — and *only* it, against the
results already in hand, so a deferred step may read an earlier step's result (`{{issue:/id}}`) as
freely as any other step. A spinner stands in while the request is out.

Two consequences worth knowing:

- A tab with nothing behind it is normally dropped rather than shown empty, and a deferred tab
  cannot be: whether an issue has comments is not known until the request is made. Such a tab is
  always offered, and opening it is what finds out.
- A built tab is written back into the preview it came from, and that preview is the cached one, so
  the request happens once per cache entry rather than once per visit. A hover after `cacheSeconds`
  has passed starts again from a fresh fetch and a fresh strip of names.

#### Actions

An action is something the card can do to the thing behind the link, rather than something it
reads. Two kinds:

- **`"kind": "button"`** fires one request.
- **`"kind": "choice"`** offers a list of options that an earlier fetch step produced, and fires a
  request for whichever one the user picks. The chosen option's id is available to the request
  template as `{{choice}}`.

| Key | Type | Meaning |
|---|---|---|
| `key` | string | Identifies the action. |
| `label` | string | Shown on the control. Defaults to `key`. |
| `kind` | `"button"` (default) \| `"choice"` | |
| `optionsPath` | JSON pointer | **Choice only.** The array of options, in a fetch step's result. |
| `optionIdPath` | JSON pointer | Relative to one option: the value `{{choice}}` becomes. |
| `optionLabelPath` | JSON pointer | Relative to one option: what to show in the list. |
| `optionBadgePath` | JSON pointer | Relative to one option: a badge shown beside the label — the state this option leads to. |
| `optionColorPath` | JSON pointer | Relative to one option: the badge's color. |
| `optionTargetIdPath` | JSON pointer | Relative to one option: the id of the state this option moves the thing *to*. |
| `currentStatePath` | JSON pointer | Where the thing's *current* state id lives. With `optionTargetIdPath`, this is what lets an undo find the option that leads back. |
| `optionFieldsPath` | JSON pointer | Relative to one option: fields the far end demands before it will accept the change. When the chosen option declares any, the card shows a small form and waits for them. |
| `method` | string | Defaults to `POST` (a fetch step defaults to `GET`). |
| `url` | template string | |
| `body` | template string | |
| `headers` | object of string→template string | |
| `auth` | object | Same shape as a [fetch step's](#fetch-steps). |
| `allowUntrustedCertificate` | boolean | |
| `timeoutMs` | number | Defaults to `8000`. |

##### Worked example: the Jira transition picker

Jira's transitions are a list of moves out of the issue's current status, each with a name, the
status it leads to, and sometimes a set of fields that must be filled in before Jira will accept it
(the "pick a resolution" dialog). One fetch step gets the list:

```jsonc
{
    "id": "transitions",
    "type": "http",
    "method": "GET",
    "optional": true,
    "url": "https://{{settings.host}}/rest/api/3/issue/{{key}}/transitions?expand=transitions.fields",
    "auth": { "type": "basic", "user": "{{credentials.email}}", "password": "{{credentials.token}}" },
    "headers": { "Accept": "application/json" }
}
```

and one action turns it into a control:

```jsonc
{
    "key": "transition",
    "label": "Move to",
    "kind": "choice",

    // Where the options are, and how to read one of them.
    "optionsPath": "transitions:/transitions",
    "optionIdPath": "/id",                                   // -> {{choice}}
    "optionLabelPath": "/name",                              // "Start progress"
    "optionBadgePath": "/to/name",                           // "In Progress"
    "optionColorPath": "/to/statusCategory/colorName",       // "yellow"
    "optionTargetIdPath": "/to/id",
    "optionFieldsPath": "/fields",                           // Jira's required-field form

    // Where the issue's current status id is, so an undo can look for the
    // transition whose /to/id leads back to it.
    "currentStatePath": "issue:/fields/status/id",

    // What to send once the user presses Apply.
    "method": "POST",
    "url": "https://{{settings.host}}/rest/api/3/issue/{{key}}/transitions",
    "body": "{\"transition\":{\"id\":\"{{choice}}\"}}",
    "auth": { "type": "basic", "user": "{{credentials.email}}", "password": "{{credentials.token}}" },
    "headers": { "Accept": "application/json", "Content-Type": "application/json" }
}
```

The card renders each option as `<name> → <badge>`, the way Jira's own menu does. Applying one runs
the request and refreshes the preview, bypassing the cache, so the status badge updates in place.

Undo is offered **only when the far end provides a way back** — that is, when some other transition
in the list has an `optionTargetIdPath` equal to the `currentStatePath` value from before the
change. Jira workflows are often one-directional, so this frequently isn't available; the card says
so rather than pretending.

Values the user typed into a required-field form are merged into the request as a top-level
`"fields"` object alongside what `body` declares, and are individually available to the template as
`{{field.<key>}}`.

#### detectPatterns

`detectPatterns` is a list of ICU regexes the terminal scans plain output for while the plugin is
enabled. Each match becomes hoverable exactly as if a text rule had matched it, which is how a
scheme the plugin owns works even when nothing marked it as a link:

```jsonc
"detectPatterns": [ "stith://(?:session|focus)/[A-Za-z0-9-]{4,64}" ]
```

Without this, `stith://session/abc123` printed as plain text is just text; with it, hovering shows
the same preview an OSC 8 hyperlink to the same URI would. The patterns join the same pool the
built-in URL and file-path detectors and any text rules use, so the **16 active text patterns**
limit covers them too.

This is distinct from a `"kind": "text"` [matcher](#matchers). A text matcher describes a pattern
the plugin *can* preview and offers it on the settings page as "Add as rule" — the user opts in. A
`detectPatterns` entry needs no rule and no opt-in beyond enabling the plugin, so it belongs to
schemes the plugin unambiguously owns, not to loose patterns like a bare issue key.

### Templates

Every templated string (`url`, `body`, headers, auth values, `commandLine`, `stdin`, `when`,
`unless`, a matcher's `link`) is expanded with `{{…}}` substitutions:

| Template | Expands to |
|---|---|
| `{{match}}` | The whole matched text. |
| `{{<name>}}` | A named capture group from the matcher's pattern, e.g. `{{key}}`. |
| `{{uri}}` | The hovered link's full URI (link matchers). |
| `{{settings.<key>}}` | A configured setting's value. |
| `{{credentials.<key>}}` | A stored credential's value. |
| `{{<stepId>:<json-pointer>}}` | A value from an earlier fetch step's JSON result. |
| `{{choice}}` | **[Actions](#actions) only.** The id of the option the user picked. |
| `{{field.<key>}}` | **[Actions](#actions) only.** A value the user typed into the action's required-field form. |

Values substituted into a `url` are URL-encoded.

**An unknown name expands to nothing** — not to an error, and not to the literal `{{name}}`. That
is deliberate, and it is what makes `when`/`unless` work as presence tests: a capture group that
belongs to a matcher which didn't fire, or a step that didn't run, simply leaves an empty string
behind.

### Paths (JSON pointers)

`path`, `iconPath`, and `colorPath` are [RFC 6901](https://www.rfc-editor.org/rfc/rfc6901) JSON
pointers into a fetch step's result:

- With a single fetch step, an unqualified pointer (`/fields/summary`) points into that step's
  result.
- With multiple steps, qualify with the step id: `message:/messages/-1/text`. An unqualified
  pointer falls back to **whichever step ran last and produced something**, which is rarely the one
  you meant — Jira's fields all say `issue:/…` for exactly this reason.
- An **empty** pointer after the step id (`comments:`) means the step's whole result, which is how
  a tab reads an endpoint that returns a bare JSON array.
- `-1` as an array index means **the last element** — Slack's manifest uses
  `message:/messages/-1/text` because `conversations.history`/`conversations.replies` return the
  message as the last (and often only) entry in a `messages` array.

## Host guarding

A link matcher's `hostSetting` names one of the plugin's own settings; the hovered URI's host must
equal that setting's **current, configured** value or the matcher does not fire at all — no
fetch, and no credential is touched. This is what stops a look-alike link
(`https://evil.example/browse/ABC-1`) from ever seeing a Jira token: the fetch simply never
starts, because the matcher never matched in the first place.

`hostSetting` is for a plugin whose host the *user* supplies. Where the host is fixed, pin it in the
pattern instead: GitHub's matchers begin `^https://github\.com/`, and every one of its fetch steps
addresses `api.github.com` literally, so there is no configured value a look-alike link could ever
match against.

More generally, a plugin's fetch pipeline never runs unless:

1. The plugin is **enabled** on the Integrations page, and
2. Every setting/credential marked `required` (or otherwise needed by the templates in use) has a
   value.

A plugin with nothing marked `required` — GitHub, whose token is optional because the CLI is tried
first — is therefore always considered configured, and hovering a matching link fetches as soon as
the plugin is enabled.

Otherwise the card shows a muted "not configured" line. Secrets never appear in `settings.json` —
see [Where credentials live](#where-credentials-live) — so sharing or backing up `settings.json`
never leaks a token.

## Caching

Each plugin's `cacheSeconds` controls how long a resolved preview is kept per matched
text/link — a second hover of the same link within that window skips the fetch entirely. GitHub,
Jira and Slack default to 300 seconds; Stith, whose data changes quickly, defaults to 30. Errors are
cached briefly too, so a broken credential doesn't retry on every hover.

Running an [action](#actions) refreshes that entry rather than reading it, so a status the user just
changed is never served stale from the cache.

## HTML representation

A manifest's `html` key is a full HTML document, given inline as a string (there is no
file-reference form). When the feature is enabled, the card renders this document in a WebView2
control instead of the plain `fields` list.

The page runs in a locked-down host: all navigation except the initial load is blocked, and there
is no context menu, no dev tools, and no zoom. Two things are injected before the page loads:

- `window.__data` — an object keyed by fetch step `id`, holding each step's JSON result (the same
  data `path`/`iconPath`/`colorPath` pointers resolve against for a `fields` card).
- `window.__uri` — the URI or text that was hovered.

The page has two ways to talk back to the host, both through `chrome.webview.postMessage`:

- `{ "height": <number> }` — resize the card to fit the content, clamped to **40–320 px**.
- `{ "open": "https://…" }` — open a link through the Terminal's normal link handling, the same
  path Open/Copy link use for a `fields` card.

Because the host applies its color scheme from the app theme, an `html` page should declare
`color-scheme: light dark` in its CSS rather than assuming one theme.

```jsonc
// integration.json
"html": "<!doctype html><html><head><meta charset='utf-8'><style>:root{color-scheme:light dark}body{font:12px system-ui;margin:6px}</style></head><body><div id='s'></div><script>const d=window.__data.issue.fields;document.getElementById('s').textContent=d.summary+' — '+d.status.name;function report(){chrome.webview.postMessage({height:document.body.scrollHeight})}window.addEventListener('load',report);new ResizeObserver(report).observe(document.body);document.body.addEventListener('click',e=>{if(e.target.tagName==='A'){e.preventDefault();chrome.webview.postMessage({open:e.target.href})}});</script></body></html>"
```

Rendering it requires the `Feature_HyperlinkPreviewHtml` feature flag (`src/features.xml`),
`WebView2Loader.dll` next to the app (packaged by `src/cascadia/CascadiaResources.build.items`), and
an installed WebView2 Runtime. Without any of them the card falls back to the `fields` list.

**Known limit: an `html` page does not show yet.** Its host (`HyperlinkHtmlHost`) parents a WebView2
*window* to the Terminal's top-level window, which is `WS_EX_NOREDIRECTIONBITMAP` with a XAML island
that does not clip siblings — the page loads and is never seen. Embed pages below use
`WebEmbedHost`, a composition-hosted WebView2, which does show; `html` should move onto it.

## Embed pages (Show *name* page)

A manifest's `embed` key is a URL template, expanded with the same values as a matcher's `link`
(`{{settings.*}}`, the matcher's named groups such as `{{id}}`). When it is set, the card and the
link preview pane both show a **Show *name* page** button (**Show Stith page**). In the card it
grows the card to fit the pane; in the pane it replaces the summary and tab strip. Either way the
page is loaded with `theme=dark` or `theme=light` appended to the query string.

The page is hosted by `Microsoft.Terminal.Control.WebEmbedHost`: a WebView2
`CoreWebView2CompositionController` whose root visual is a child visual of a XAML `Grid`, so it is
drawn by XAML's compositor and clips, scrolls and layers with the card or pane. Pointer input on
that `Grid` is forwarded to the page by hand (`SendMouseInput`), which is why the `Grid` needs a
`Background`.

Unlike `html`, this is a live page with its own routing:

- Navigation **within the page's origin** is allowed, so the page's own tabs and links work.
  Navigation to any other origin, and anything that asks for a new window (`target=_blank`,
  `window.open`), is cancelled and opened through the Terminal's normal link handling instead.
- A certificate error is accepted **only for the page's own origin**, matching what
  `allowUntrustedCertificate` does for fetch steps.
- Showing the page again for the same URL keeps it as it was left; nothing reloads it.
- The card stays up while the page is showing — it is something being read, not a hover.
  **Esc** (in the page or the terminal), a click on the terminal, **Hide *name* page**, a resize of
  the pane, or switching tabs puts it away. In the pane, Esc returns to the summary.
- In the pane the button is a switch: while it is on, each link shown there opens on its page.

Stith's manifest uses `"{{settings.baseUrl}}/embed/s/{{id}}?host=mullion"`, its chromeless
session view.

## Adding a third-party plugin

Drop a new folder under:

```
%LOCALAPPDATA%\Microsoft\Windows Terminal\Integrations\<your-plugin-id>\integration.json
```

and it's picked up the next time the registry refreshes (a settings reload triggers this). Use
the [Jira example](#full-annotated-example-jira) above as a starting template; a matcher, at least
one fetch step, and a small `fields` list is the whole surface area — `fieldGroups`, `tabs`,
`actions` and `detectPatterns` are all optional additions on top of that. Giving a user plugin the
same `id` as `github`, `jira`, `slack`, or `stith` replaces that built-in outright — useful for
forking a shipped plugin or handing ownership of it to another project's own repo.

## Built-in local file previews

Local Windows, UNC and WSL file links preview without an integration or tooltip rule.
The card shows up to 30 text lines or a first image/PDF page; **Show in pane** provides
selectable text, line numbers, image/PDF zoom, PDF pages, and Office sheet/slide selectors.
**Fit** scales the entire image or PDF page into the preview area and follows width changes;
50%, 100%, and 200% return to a fixed zoom level.
Existing preview switches and explicit integration selections still apply. File previews
do not change the “No rule matched” attribution: built-in capability is independent of rules.

Supported content: UTF-8 and BOM-marked UTF-16 text; PNG/JPEG/GIF/BMP/TIFF (first frame);
PDF pages; DOCX paragraphs and tables; XLSX cells with cached formula values; PPTX slide text.
Office content is extracted locally without Office, macros, recalculation or external relationships.
Legacy Office formats and other binary formats show metadata and an explanation.

Limits: 1 MiB of extracted text, 50 MiB input, 32 MiB of Office XML, 16 megapixels per image,
200 rows × 50 columns per sheet. PDF navigation is capped at 10,000 pages. Truncation is labeled.
New hovers and pane Refresh reread files; preview content is not cached on disk.

### Shared preset catalog

Lintel owns the ordered catalog and examples. `LinkTooltipPresets.g.h` is generated from
Lintel's `presets.json` and canonical integration matchers. From a Lintel checkout, run:

```sh
node conformance/sync-presets.mjs --terminal <terminal-checkout> --torbie <torbie-checkout>
node conformance/sync-presets.mjs --check --terminal <terminal-checkout> --torbie <torbie-checkout>
```

Commit generated artifacts with the consumer changes; normal builds require no network or
Lintel checkout. Saved custom rules are retained. New presets include Text files, PDF files,
and Office documents; file presets have separate extension criteria.

### GitHub organizations and account status

Open **Integrations → GitHub** to see the authenticated account name, login,
avatar and connection status. **Refresh** checks it again. GitHub CLI credentials
for github.com take precedence over the saved PAT, consistently for the account
check and previews. Jira and Slack also expose account checks when enabled;
other integrations explicitly report that an account check is unavailable.

A bare `repo#number` names no owner, so one is found in this order (Lintel
SPEC, "Resolving repo#number"):

1. **Remembered repositories** — an owner picked for that repository name on a
   card. Listed in GitHub settings, where each can be forgotten.
2. **Pinned organizations** — editable rows, reordered by dragging (or ↑/↓).
   Pinning is only needed to break a tie between two reachable organizations
   that share a repository name.
3. **Everything the account can reach** — the login, its organizations and the
   owners of its repositories (`/user`, `/user/orgs`, `/user/repos`), cached for
   30 minutes. Settings shows which ones are tried after the pinned rows.
4. **Repository search** — every public repository with exactly that name,
   most-starred first (cached 10 minutes; search is rate-limited). With
   **Assume the most-starred repository** on (the default) the card resolves to
   it, says so in an *Owner* row, and its action row offers to change it. With it
   off, or when a remembered owner has no such number, the card lists the owners
   with a filter box; choosing one remembers it in `repoOwners`.

**Pin a reachable organization** discovers organizations visible to the active
credentials, including organization owners of accessible repositories for
fine-grained tokens. Discovery can be partial; manual additions are always
available.

The repo#number preset now recognizes references inside prose, including
`See terminal#18920 for the details`. Untouched older copies of the shipped
preset migrate automatically; custom expressions remain unchanged. A click can
resolve a reference even before the hover preview has loaded. Resolution no
longer shares a repository-only cache across issue numbers or owner preferences.

Lintel owns the preset and the `account.provider` / `editor: github-owners`
metadata. Account checks use read-only endpoints and hidden CLI processes;
tokens and raw authentication responses are never shown in settings.
