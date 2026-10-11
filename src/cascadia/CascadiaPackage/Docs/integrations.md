# Integrations

An integration teaches the Terminal what a link or a piece of output *refers to*, so that
hovering it can show you the thing itself rather than its URL. Hover a pull request and see
its title, state and reviewers; hover a Jira key printed by a build script and see the
ticket.

The Integrations page lists every integration the Terminal found, with where it came from
and a switch. A disabled integration is never contacted, whatever a rule says.

## What ships with the Terminal

| Integration | Recognises | Needs |
|---|---|---|
| **GitHub** | `github.com/<owner>/<repo>/pull/<n>`, `/issues/<n>`, `/commit/<sha>` | Nothing, if the GitHub CLI is signed in. Otherwise a personal access token. |
| **Jira** | `https://<host>/browse/<KEY>`, and opt-in bare keys like `CAB-8209` in plain text | Site host, plus your account email and an API token |
| **Slack** | `https://<workspace>.slack.com/archives/<channel>/p<ts>` permalinks, threads included | A bot token with `channels:history`, `groups:history` and `users:read` |
| **Stith** | `stith://session/<id>`, `stith://focus/<id>`, `stith://copy/<id>`, server URLs, and bare session ids | Server URL |
| **shefrd** | Multiplexer pane addresses like `w1N:p39`, and `shefrd://pane/<id>` | Stith server URL |

GitHub tries `gh auth token` first and only falls back to a stored token, so if you already
use the CLI there is nothing to configure.

## Setting one up

Open an integration and you get up to six sections.

**Settings** — plain, non-secret values: a site host, a server URL. These are saved in
`settings.json` under `"integrations"`, keyed by integration id.

**Credentials** — one box per secret, with a *stored* indicator and a **Clear** button once
saved.

> [!IMPORTANT]
> Credentials are **never** written to `settings.json`. They go into Windows Credential
> Manager (Control Panel → Credential Manager → Web Credentials) under the resource
> `WindowsTerminal/Integrations/<id>`, one entry per field. The vault is per Windows user,
> so every Terminal built from the same source sees the same stored credentials.
>
> A credential is only ever sent to the host the integration is configured for. A
> look-alike link pointing somewhere else is refused before anything is sent.

Until every required setting and credential is filled in, the integration shows **Not
configured** and is never contacted at all.

**Show in tooltip** — a checkbox per field, choosing which ones appear on the card and in
what order. Leave them all clear to use the integration's own defaults. When an integration
groups its fields ("Details", "Development"), each group gets a tri-state header checkbox
that selects or clears the whole group at once.

**Show as tabs** — longer content the integration can fetch, shown behind a small tab strip
rather than among the fields: a description, a list of comments. With no tab enabled the
card shows no tab strip.

**Detect in output** — patterns this integration can recognise in plain terminal output,
each with an **Add as rule** button. Pressing it creates a matching rule on the
[Link Tooltip](link-tooltips.md) page, which you can then rename, reorder or delete. This is
the opt-in step that makes a bare `CAB-8209` in your build log hoverable.

**Source** — read-only: either `built-in`, or the path of the file it was loaded from.

## What a card can do

Beyond showing fields, an integration can declare **actions** — something the card does to
the thing behind the link. Jira's status transitions are the worked example: pick one,
press Apply, and the badge on the card changes.

Any preview can also be sent to a pane instead of a hover card, with **Show in pane**, which
is the better surface for a long description or a comment thread.

An integration whose service has a page of its own for the thing can offer it as
**Details**. Pressing it grows the card and shows that page inside it, with all of its tabs
and sections. Stith uses this for its full session view. The card stays open while you read:
press **Esc**, click the terminal, or press **Close details** to put it away. Links that
lead off the page open in your browser. Details needs the WebView2 runtime, which Windows 11
already includes.

## Adding your own

Drop a folder containing an `integration.json` into the directory the Integrations page
names at the bottom of its list, and restart the Terminal. The manifest declares the
patterns it recognises, the settings and credentials it needs, the HTTP steps that fetch
data, and the fields, tabs and actions the card should show.

The full manifest reference — every key, with an annotated example — is in the repository's
`doc/link-previews-and-integrations.md`. It is developer documentation rather than a
user guide, which is why it is not reproduced here.

## See also

- [Link tooltips](link-tooltips.md) — the card integrations draw into, and the rules that
  decide which integration is asked.
- [Regular expressions](regular-expressions.md) — the patterns in *Detect in output*.
