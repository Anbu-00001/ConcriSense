// Minimal Arduino shim so the pure-math firmware modules (FFT, physics) can be
// compiled and unit-tested on the host. No hardware, no toolchain required.
//
// math.h rather than cmath: the firmware calls sqrtf/cosf/logf unqualified, and
// those are guaranteed at global scope here.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PI
#define PI 3.1415926535897932384626433832795f
#endif
