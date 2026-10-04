# SlopAuras

SlopAuras is a GWToolbox++ plugin that displays remaining durations for selected player effects.

Add effect skill IDs in the plugin settings. The SlopAuras window lists matching effects while they are active.

## Building

This plugin uses GWToolbox++ and GWCA interfaces and is built with the GWToolbox++ CMake project. To include it in that build, add the plugin source directory and register `SlopAuras` with `add_tb_plugin`.
