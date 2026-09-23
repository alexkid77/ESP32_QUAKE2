// Debug print helper for the Quake 2 ESP32-P4 port.
//
// Author: Alejandro Villegas Alonso
//         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
//
// All debug printf output is controlled by the "QUAKE2_ESP32P4_DEBUG" Kconfig
// option (menuconfig). When it is OFF (default) all the debug output is
// compiled out and nothing is printed.
#pragma once

#include <stdio.h>

#ifdef CONFIG_QUAKE2_ESP32P4_DEBUG
#define QUAKE2_ESP32P4_DEBUG
#endif

#ifdef QUAKE2_ESP32P4_DEBUG
#define Q2P4_DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define Q2P4_DEBUG(fmt, ...) ((void)0)
#endif