# Provenance marks: ◆ and ◇

This Terminal is built from a fork of Windows Terminal, and it has settings Microsoft's
build does not. That is useful right up until you look something up: a search for a setting
you found here will not find it in Microsoft's documentation, and an answer written for
upstream may describe a control that does not exist here.

The provenance marks make the boundary visible. Two switches on **Appearance** turn them on,
and a row carries at most one.

| Mark | Switch | Meaning |
|---|---|---|
| **◆** | Mark features added by this fork | This setting does not exist in upstream Windows Terminal at all. |
| **◇** | Mark settings upstream hides in JSON | Upstream has this setting, but only lets you set it by hand in `settings.json`. The control beside the mark is this fork's. |

Both are off by default, because most of the time you just want to change a setting. Turn
them on when you are about to read someone else's documentation, file a bug, or copy a
settings file somewhere else.

## Reading them

A filled **◆** beside a row means: nothing upstream will explain this, and a `settings.json`
written here may not be understood by Microsoft's build. Whole groups can be marked — the
mark sits on the group's header rather than on every row inside it — and the navigation
items for the most fork-specific pages carry it too.

A hollow **◇** means the *setting* is standard and the *control* is not. Upstream
understands the value perfectly; it simply never drew a control for it. Documentation
written for upstream will describe the JSON key but tell you to edit the file by hand.

## Why ◇ exists at all

Upstream ships a number of settings that can only be set by editing `settings.json`. This
fork's position is that a setting worth having is worth showing, so those controls get
built — and then marked, so you can tell which part of the row is the fork's contribution.

Two examples are on the [Tabs](tab-strip.md) page: the profile icon style and the tab close
button. Upstream implemented both and exposed neither.

## Pages that are entirely this fork's

If the marks are on, these navigation items carry one. They have no counterpart upstream, so
their documentation is the topic list you are reading now:

- [Session restore](session-restore.md)
- [Link tooltips](link-tooltips.md)
- [Integrations](integrations.md)
- [Activity log](activity-log.md)
- This Documentation page

## A note on sharing settings files

A `settings.json` from this Terminal may contain keys Microsoft's build has never heard of.
Unknown keys are ignored rather than rejected, so the file still loads — you simply lose the
behaviour those keys described. Going the other way is always safe: anything upstream writes,
this build understands.

## See also

- [Extensions](extensions.md) — upstream's mechanism for adding profiles, unchanged here.
