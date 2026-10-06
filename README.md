# SlopAuras

SlopAuras is a GWToolbox++ plugin that displays remaining durations for selected player effects.

To configure tracked effects, open the SlopAuras section under Toolbox's **Settings > Plugins** and select **Open SlopAuras settings**. The settings window is drawn while the main SlopAuras window is visible. The SlopAuras window lists matching effects while they are active. PvE skill durations use the associated title rank when available, including Asura and Sunspear rank scaling. Configured skills that apply conditions, such as "You Move Like a Dwarf!", can also be tracked; their timers are estimated from the cast and expire early when the target no longer reports a condition or hex. The game does not expose exact per-condition remaining time, so another active condition or hex may keep an estimate visible after the tracked one ends. Natural Resistance agents are configured by name, so the setting remains valid if an agent's model ID changes.

SlopAuras provides client-side chat commands: `/sa hide` hides its window, `/sa show` shows it, `/sa print` prints active tracked effects and enemy cooldowns in chat, and `/sa help` lists the commands in chat. Effects on configured Natural Resistance enemies are omitted from the printed output.

## Building

This plugin uses GWToolbox++ and GWCA interfaces and is built with the GWToolbox++ CMake project. To include it in that build, add the plugin source directory and register `SlopAuras` with `add_tb_plugin`.
