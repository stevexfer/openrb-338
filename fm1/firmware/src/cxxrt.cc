// SPDX-License-Identifier: GPL-3.0-only
// The few C++ runtime hooks the engine needs: no heap, no exceptions, no static constructors.
#include <stddef.h>

extern "C" void __cxa_pure_virtual() {
    for (;;) {
    }
}

extern "C" int* __errno() {
    static int e;
    return &e;
}

// virtual destructors reference these; the engine is never deleted
void operator delete(void*) noexcept {}
void operator delete(void*, size_t) noexcept {}
