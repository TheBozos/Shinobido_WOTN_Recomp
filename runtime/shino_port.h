// Shinobido: Way of the Ninja's native port boundary.
#pragma once
#include "bozo/port.h"

namespace shino
{
    // The port's overrides (registerFunction by address) and native instruction sites (runtime/shino_port.cpp).
    void install(Runtime &rt, bozo::port::Registry &registry);
} // namespace shino
