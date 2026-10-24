#pragma once
// Debug tracing for local analysis; disabled in the submitted bot.
// #define EVO_TRACE 1
#ifdef EVO_TRACE
#include <iostream>
#define TRACE(x) do { std::cout << x << '\n'; } while (0)
#else
#define TRACE(x) do {} while (0)
#endif
