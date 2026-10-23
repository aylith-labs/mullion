# Outreach log

Newest last. One line per step taken, plus the lesson when there is one.

| Date | Step | Link | Outcome | Lesson |
|---|---|---|---|---|
| 2026-10-19 | Reply on microsoft/terminal#8914 (profile reordering) pointing to the feature page | (deleted) | Deleted by maintainers | |
| 2026-10-19 | Ten more replies on microsoft/terminal issues, spaced 25 minutes apart by a script | (all deleted) | "last warning" on #8849, then a block from the whole Microsoft GitHub org | Never in Microsoft repos; never automate posting; stop at the first "no". |
| 2026-10-20 | Deleted the remaining comments; apology to opencode@microsoft.com | hello@aylith.com Sent | No reply yet; watched by a cloud routine | Apologise once, through the official channel, then wait. |
| 2026-10-20 | Repo metadata: homepage, description, topics; issues enabled | github.com/aylith-labs/mullion | Done | |
| 2026-10-20 | awesome-terminals PR, Windows section | https://github.com/cdleon/awesome-terminals/pull/139 | Open | Merges come in batches every few weeks; the description can carry features and a screenshot table even when the list entry is one line. |
| 2026-10-20 | README banner (light and dark) | README.md | Done | |
| 2026-10-20 | Docs page of the upstream issues Mullion answers, footer link only | https://mullion.aylith.com/upstream.html | Done | The fork's own site is the place to say "this answers upstream #N". |
| 2026-10-20 | Personal apology to Dustin Howett (duhowett@microsoft.com, public on his GitHub profile), Steven's decision and wording | hello@aylith.com Sent | Sent; watched by the same cloud routine | Two apologies are the ceiling: nothing further to anyone at Microsoft unless they write first. |
| 2026-10-21 | Docs page of every public post and listing, full posted text, timeline and table views, footer link only | https://mullion.aylith.com/mentions.html | Done | Keep a public record of what was posted where; Microsoft threads and email stay off it. |
| 2026-10-23 | Scrubbed work identifiers (a workplace Jira key, the employer's name, an internal service name) from mullion, lintel and torbie text and screenshots; OCR-checked every image in the public repos | 52713ba33, lintel 71cd503, torbie eaf4a9bb | Done; two mullion screenshot pairs withdrawn until retaken | Before any outreach, OCR the screenshots and grep the public repos for work names: a listing sends strangers to all of it. Git history still holds the old strings. |
| 2026-10-23 | Fresh portable release from a green build (the previous one predated the close-pane crash fix by 88 commits); README gained a "Try it" section | https://github.com/aylith-labs/mullion/releases/tag/portable-52713ba | Done | Check what a newcomer downloads before every step, not only before Show HN. |
| 2026-10-23 | awesome-wsl PR, Terminals section, WSL features first, trademark note | https://github.com/sirredbeard/awesome-wsl/pull/124 | Open | |
| 2026-10-23 | Daily read-only digest routine for every listed venue | https://claude.ai/code/routines/trig_015tWF1J55Gv6EjG4B9RotTu | Running, 07:30 UTC | A cloud routine reaches only the repos attached as its sources. |

## Next

| Order | Step | Who | Gate |
|---|---|---|---|
| 1 | thechampagne/awesome-windows PR (Terminal section; OSS icon; description ends with a full stop). **Prepared**: branch `steven-pribilinskiy:add-mullion` (52728dc, the entry between MTPuTTY and Putty) and body `drafts/awesome-windows.md`. To open it: `gh pr create --repo thechampagne/awesome-windows --base main --head steven-pribilinskiy:add-mullion --title "Add Mullion to Terminal" --body-file .claude/skills/mullion-outreach/drafts/awesome-windows.md`, then add it to `docs/mentions.html` and to the digest routine's sources (already attached). | PR from Steven's account | 2026-10-24 or later; awesome-wsl#124 has no "no"; re-read Contributing.md first |
| 2 | AlternativeTo listing, as an alternative to Windows Terminal, leading with what differs | Steven, web form | one day after step 1 |
| 3 | Show HN, Steven's own words | Steven | close-pane crash fix verified and promoted (now in the 2026-10-23 portable ZIP); afterTabs vertical-strip bug fixed; Steven free to stay in the thread |
| 4 | One subreddit (r/commandline) | Steven | a week after HN; rules read logged in |
| 5 | The four Stack Exchange answers | Steven | each capability re-checked first |
