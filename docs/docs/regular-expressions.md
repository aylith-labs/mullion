# Regular expressions

Three places in the Terminal take a regular expression: the **profile matcher** on the
Dropdown Menu page, the **Pattern** field on a Link Tooltip rule, and the patterns an
integration declares for itself.

All of them use the same engine — **ICU regular expressions**, the flavour Java and Perl
programmers will recognise. It is *not* the .NET flavour, so a few .NET-only conveniences
(`\p{IsGreek}`, balancing groups, right-to-left matching) are not available.

## The syntax you will actually use

| Pattern | Matches |
|---|---|
| `abc` | the literal text `abc` |
| `.` | any one character |
| `\d` `\w` `\s` | a digit, a word character, a whitespace character |
| `\D` `\W` `\S` | anything *but* those |
| `[abc]` `[^abc]` | one of `a` `b` `c`; anything but |
| `[a-z]` | a character in a range |
| `a*` `a+` `a?` | zero or more, one or more, zero or one |
| `a{2}` `a{2,}` `a{2,5}` | exactly, at least, between |
| `a\|b` | either side |
| `(ab)+` | a group, repeated |
| `(?:ab)` | a group that does not capture |
| `(?<name>ab)` | a named capture — see below |
| `^` `$` | start and end of the text |
| `\b` | a word boundary |
| `(?=ab)` `(?!ab)` | the next text is / is not `ab`, without consuming it |

Escape anything with a meaning you do not want: `\.` `\\` `\(` `\[` `\+` `\?` `\$`.

## Anchoring and case: it differs by field

This is the single most common surprise, and the answer is not the same everywhere.

| Where | Has to match | Case |
|---|---|---|
| Profile matcher | the **whole** value | sensitive |
| Link Tooltip rule, kind **Link** | **anywhere** inside the link | insensitive |
| Link Tooltip rule, kind **Text** | found **anywhere** in the output; what it finds becomes the match | sensitive |

So for the profile matcher, `Ubuntu` does **not** match the profile named `Ubuntu 22.04` —
`Ubuntu.*` does, and `^` and `$` are implied, so adding them changes nothing. For a link
rule, `github\.com` is enough; you do not have to describe the rest of the URL.

For a text rule, anchor it yourself with `\b` if you want whole words: `\bCAB-\d+\b` rather
than `CAB-\d+`, which would also match the `CAB-8` inside `XCAB-81`.

> [!TIP]
> Start a pattern with `(?i)` to make it case-insensitive wherever it is not already:
> `(?i).*ubuntu.*` matches `Ubuntu 22.04`, `UBUNTU` and `my ubuntu box`.

## Named captures

An integration or a text rule can pull pieces out of the match and use them in a URL or a
field. Name them with `(?<name>…)`:

```
\bPROJ-(?<number>\d+)\b
```

> [!WARNING]
> ICU's rule for a capture name is **a letter followed by letters and digits** —
> underscores are not allowed. `(?<ts_s>…)` does not merely fail to capture: ICU rejects
> the whole pattern, so the rule never matches anything at all. Write `(?<tsS>…)`.

Two names are always available without being captured: `match` (the whole match) and `uri`
(the text the pattern was run against).

## The profile matcher, specifically

The Dropdown Menu page's **Profile matcher** adds one menu entry that stands for a *group*
of profiles. It has three fields, each a regular expression:

| Field | Matched against |
|---|---|
| Profile name | the profile's display name |
| Profile source | the extension that provided it, e.g. `Windows.Terminal.Wsl` |
| Commandline | the profile's command line |

A profile joins the group if **any** filled field matches it — the fields are *or*, not
*and*. An empty field is ignored entirely rather than matching everything, so leaving two
of the three blank is the normal case.

Useful ones:

- Name `.*` — every profile with a name at all.
- Name `(?i).*powershell.*` — anything with PowerShell in its name. Note both halves:
  `(?i)` because the matcher is case-sensitive, and `.*` on each end because the whole name
  has to match.
- Source `Windows\.Terminal\.Wsl` — every WSL distribution.
- Commandline `.*\.exe` — profiles that launch an executable directly.

A pattern that does not compile is reported as invalid and the entry matches nothing, which
is why an empty menu group usually means a typo rather than no profiles.

## Performance

Patterns are run against terminal output as it scrolls, so they are given a time budget and
are abandoned if they exceed it. Avoid nested unbounded quantifiers — `(\w+)*` and friends —
which can take exponential time on text that nearly matches.

## See also

- [Link tooltips](link-tooltips.md) — where rule patterns are written.
- [Integrations](integrations.md) — patterns an integration suggests, ready to add as rules.
