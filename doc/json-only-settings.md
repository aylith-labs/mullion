# Settings with no Settings UI

Upstream Windows Terminal ships a number of settings that can only be set by hand-editing
`settings.json`. This fork's position is that a setting worth having is worth showing, so
this file tracks what is still missing a control, and the `aylith.imprintJsonOnly` switch
marks the ones we have since surfaced.

## How this list was produced

`MTSMSettings.h`'s X-macro tables give the full set of settings. Cross-referencing those
names against every `.xaml` and `*ViewModel.h` under `src/cascadia/TerminalSettingsEditor`
gives a first cut, but **a name-based sweep alone is wrong in both directions** and each
entry below was confirmed by hand:

- Pages whose `DataContext` *is* the view model bind bare — `Appearances.xaml` writes
  `{x:Bind CurrentCursorShape}`, not `{x:Bind ViewModel.CurrentCursorShape}` — so a
  prefix-anchored search reports `cursorShape` as missing when it has had a dropdown for
  years.
- View-model properties are frequently renamed relative to the setting.
  `hyperlink.primaryClickModifier` is exposed as `ViewModel.CurrentPrimaryClickModifier`
  (no `Hyperlink` prefix), and `initialPosition` is exposed as `LaunchPositionCard` plus
  `UseDefaultLaunchPosition`. Both look absent to a search for the setting's own name.

So: use a sweep to build the shortlist, then confirm every survivor by opening the page.

## The whole `theme.*` namespace — no UI whatsoever

This is the big one, and it is unambiguous: **no page in the settings editor edits a theme.**
No `.xaml` file under `TerminalSettingsEditor` so much as mentions `Themes`. The theme
*picker* on Globals → Appearance chooses between themes; nothing edits what a theme contains.

| Setting | Type |
|---|---|
| `theme.window.applicationTheme` | light / dark / system |
| `theme.window.frame` | ThemeColor |
| `theme.window.unfocusedFrame` | ThemeColor |
| `theme.window.experimental.rainbowFrame` | bool |
| `theme.window.useMica` | bool |
| `theme.window.showWorkspacesButton` | bool (fork) |
| `theme.tabRow.background` | ThemeColor |
| `theme.tabRow.unfocusedBackground` | ThemeColor |
| `theme.tab.background` | ThemeColor |
| `theme.tab.unfocusedBackground` | ThemeColor |
| `theme.tab.iconStyle` | default / hidden / monochrome |
| `theme.tab.showCloseButton` | always / hover / never / activeOnly |
| `theme.settings.theme` | light / dark / system |

`theme.tab.iconStyle` and `theme.tab.showCloseButton` are the two Tabby calls for "show
profile icon on tab" and "hide tab close button". Upstream implemented both
(microsoft/terminal#8157, #3335) and then exposed neither.

**Both are now exposed**, by option 1 below: the window settings `tabIconStyle` and
`tabCloseButton` override the theme when set, and defer to it when not. They are deliberately
absent from `defaults.json`, because that is what keeps `HasTabIconStyle()` false until the
user actually chooses something — `INHERITABLE_SETTING` generates a `Has` accessor for exactly
this, and it is projected, so `TerminalPage` can ask.

One rough edge, worth knowing and not worth fixing: the dropdown shows the *override's* value,
not the resolved one. The macro defaults match what the built-in themes give
(`IconStyle::Default`, `TabCloseButtonVisibility::Always`), so for anyone on a built-in theme
the control reads correctly. A custom theme that sets these will show its own value in the
terminal but the default in the dropdown, until the control is touched — at which point the
override takes over and the two agree again.

### The trap that shapes how these get exposed

**A theme is not a viable home for a setting the Settings UI writes.**
`CascadiaSettings::ToJson` (`CascadiaSettingsSerialization.cpp`, the `builtinThemes` skip)
deliberately omits the built-in themes when writing user settings back out, so a value
stored on `dark`, `light` or `system` — which is to say, on the default that nearly every
user is on — applies for the session and is silently gone on the next reload.

This is why `tabPosition` became a window setting rather than the `theme.window.tabPosition`
the upstream spec for #835 proposes. Any UI for the rows above has to reckon with the same
thing. Three options, in preference order:

1. **Window-setting override.** Add a window setting that wins when set and falls through to
   the theme when not. The UI writes the override, so it persists; hand-written themes keep
   working unchanged. Costs one extra setting per exposed value.
2. **Materialize a user theme on edit.** Editing a built-in theme silently forks it into a
   user-defined copy. Persists correctly, but it surprises people — their settings.json
   grows a themes block they did not ask for, and their theme stops tracking ours.
3. **Only allow editing user-defined themes**, greying the controls out on built-ins. Honest,
   and useless to the default configuration.

## Globals with no control

| Setting | Notes |
|---|---|
| `disabledProfileSources` | Array. Needs a list editor. |
| `compatibility.controlPipe` | Fork feature. |
| `experimental.useBackgroundImageForWindow` | bool |
| `experimental.enableShellCompletionMenu` | bool |
| `startupActions` | Free-form action string; a text box is honest but unhelpful. |

## Profile settings with no control

| Setting | Notes |
|---|---|
| `compatibility.allowDECNKM` | bool |
| `experimental.pixelShaderPath` | Path. The Rendering section of `Compatibility.xaml` is the natural home (upstream #20200 folded the Rendering page into it). |
| `experimental.pixelShaderImagePath` | Path. |

## Confirmed false positives — do not re-report these

Already exposed, under a different name than the setting:

- `cursorShape`, `intenseTextStyle`, `adjustIndistinguishableColors`,
  `backgroundImageStretchMode`, `backgroundImageAlignment` — `Appearances.xaml`, bare binds.
- `hyperlink.primaryClickModifier`, `.primaryClickGesture`, `.primaryAction`, and the three
  `alternative*` equivalents — `LinkTooltip.xaml`, as `ViewModel.CurrentPrimaryClickModifier`
  and friends.
- `hyperlink.tooltipRules` — the rule editor on `LinkTooltip.xaml`.
- `initialPosition` — `Launch.xaml`, as `LaunchPositionCard` / `UseDefaultLaunchPosition`.
- `environment` — `Profiles_Advanced.xaml`, as `EnvironmentVariables`.
- `copyFormatting`, `disableAnimations`, `newTabMenu` — all have controls.

## Marking

Two provenance marks, and a row carries at most one:

- **◆**, under `aylith.imprint` — this setting does not exist in upstream Windows Terminal.
- **◇**, under `aylith.imprintJsonOnly` — upstream has this setting but exposes it only in
  `settings.json`; the control beside the mark is ours.

`SettingsExpander` forwards `IsForkFeature` to the card that draws its header, so a whole
group can carry the filled mark. It does **not** yet forward `IsJsonOnlyUpstream` — nothing
marks a group that way so far. Mirror `SettingsExpander`'s existing `IsForkFeature`
dependency property and the `TemplateBinding` in `SettingsControlsStyle.xaml` when the first
group needs it.
