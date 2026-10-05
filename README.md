# SlopAuras

SlopAuras is a GWToolbox++ plugin that displays remaining durations for selected player effects.

Add effect skill IDs in the plugin settings. The SlopAuras window lists matching effects while they are active.

SlopAuras provides client-side chat commands: `/sa hide` hides its window, `/sa show` shows it, and `/sa help` lists the commands in chat. These commands are intercepted locally and are not sent to the game server.

## Building

This plugin uses GWToolbox++ and GWCA interfaces and is built with the GWToolbox++ CMake project. To include it in that build, add the plugin source directory and register `SlopAuras` with `add_tb_plugin`.
