#pragma once

// Firmware update over plain HTTP from admin-panel nginx: version.txt, then the binary's
// SHA-256, then the binary, installed only if its hash matches.

// Checks once per 24 h and right after power-on/RESET; reboots after a successful install.
void checkForUpdatesIfDue();
