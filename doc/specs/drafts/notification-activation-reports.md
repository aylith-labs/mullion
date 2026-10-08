# Report notification clicks to the program that sent them

Status: requested, not started. Upstream prior art: microsoft/terminal#7718 (OSC 777, closed), where a
maintainer names the missing piece: "Clicking on a notification spawns a second Terminal instance.
We'll have to communicate back to the sender."

## What is true now

An OSC 777 notification raises mullion's window, tab and pane on a click, but the program that sent it
never learns of the click. A multiplexer running inside (shefrd, herdr, tmux) therefore cannot take the
user to the space, tab and pane the notification was about: to it, the click looks like an alt-tab.

mullion is also indistinguishable from Windows Terminal by environment, so a program cannot tell which
protocol to use.

## Ask

1. **Identify mullion.** Set `TERM_PROGRAM=mullion` in every session, and add `TERM_PROGRAM` to
   `WSLENV` so programs under WSL see it.
2. **Implement kitty's desktop-notification protocol, OSC 99**
   (https://sw.kovidgoyal.net/kitty/desktop-notifications/): metadata `i=<id>`, `d=0` chunking with
   `p=title|body`, and `a=focus,report`. Show the notification through the existing toast path, focus
   window, tab and pane on activation as OSC 777 does today, and with `report` write
   `ESC ] 99 ; i=<id> ; ESC \` to that pane's input.
3. **Optional:** answer the `p=?` capability query, so programs can detect support without relying on
   `TERM_PROGRAM`.

## Sender side

Already shipped in shefrd (`26efe08ad`): where the terminal reports clicks (kitty today, mullion once
`TERM_PROGRAM=mullion` is set) it sends OSC 99 with `i=shefrd-<n>:a=focus,report`, reads the report off
stdin, and focuses the pane the notification came from. Plain Windows Terminal keeps OSC 777.
