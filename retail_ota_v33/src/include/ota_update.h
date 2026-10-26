#pragma once

// Firmware update from the GitHub repository (version.txt, then the binary).

// Checks once per 24 h and right after power-on/RESET; reboots after a successful install.
void checkForUpdatesIfDue();
