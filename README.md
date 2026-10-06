# SlopAuras

SlopAuras is a GWToolbox++ plugin that tracks selected player effects and enemy cooldowns.

Enemy nameplate overlays are enabled by default. They add knockdown status and its remaining timer, hex/condition/enchantment status, configured player-effect timers, and configured enemy cooldown timers below the game's selectable enemy names. Choose which information to show in **Settings > Plugins > SlopAuras > Open SlopAuras settings**. The overlay does not replace or intercept the game's nameplates.

Configure tracked skills in the SlopAuras settings. PvE durations scale with title rank when available. Condition timers are estimates because the game does not expose exact remaining time. Configure Natural Resistance agents by name.

Commands: `/sa hide`, `/sa show`, `/sa print`, `/sa mute`, `/sa unmute`, and `/sa help`. Print omits effects on configured Natural Resistance agents.

Sound alerts are configurable for effect application, near-expiry, and enemy cooldown completion. Choose a WAV per event and set the expiry lead time. Alerts play sequentially through Windows audio; notifications start unmuted.

## Building

Build with the GWToolbox++ CMake project: add this plugin directory and register `SlopAuras` with `add_tb_plugin`.
