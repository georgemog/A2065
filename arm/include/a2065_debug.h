#ifndef A2065_DEBUG_H
#define A2065_DEBUG_H

#include <stdio.h>

/* Runtime debug switch. Defined in registers.cpp (shared by all daemon
 * builds); set to 1 by the daemon's --debug / -d command-line flag.
 * DBG(...) is a drop-in for fprintf(stderr, ...) that only emits when
 * debug output is enabled, so the hot paths stay quiet by default. */
extern int a2065_debug;

#define DBG(...) do { if (a2065_debug) fprintf(stderr, __VA_ARGS__); } while (0)

#endif
