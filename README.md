# SlopAuras

SlopAuras is a GWToolbox++ plugin that tracks selected player effects and enemy cooldowns.

Enemy nameplate overlays are enabled by default. They add knockdown status and its remaining timer, icons and estimated timers for configured tracked hexes and conditions, other configured player-effect timers, and configured enemy cooldown timers below the game's selectable enemy names. Untracked hexes, conditions, and enchantments are not displayed. Choose which information to show in **Settings > Plugins > SlopAuras > Open SlopAuras settings**. The overlay does not replace or intercept the game's nameplates.

Configure tracked skills in the SlopAuras settings. PvE durations scale with title rank when available. Condition timers are estimates because the game does not expose exact remaining time. Configure Natural Resistance agents by name.

Commands: `/sa` or `/sa show` or `/sa hide` (toggles the window), `/sa settings` or `/sa s` (toggles the settings window), `/sa print`, `/sa mute` or `/sa unmute` (toggles sound notifications), and `/sa help`. Print omits effects on configured Natural Resistance agents.

Sound alerts are configurable for effect application, near-expiry, and enemy cooldown completion, with an optional custom WAV per individual tracked effect or cooldown that overrides the group default. Choose a WAV per event and set the expiry lead time. Alerts play sequentially through Windows audio; notifications start unmuted.

The first time SlopAuras is loaded (no settings file yet), it prints a welcome chat message pointing to `/sa help` and `/sa s`.

## Building

Build with the GWToolbox++ CMake project: add this plugin directory and register `SlopAuras` with `add_tb_plugin`.
