#include <stdint.h>

__declspec(dllexport) __declspec(noinline) double probe_four_doubles(double a, double b, double c, double d) {
    volatile double fourth = d;
    return a + 2 * b + 3 * c + 4 * fourth;
}

__declspec(dllexport) __declspec(noinline) int64_t probe_integer(int64_t value) {
    volatile int64_t copy = value;
    return copy + 7;
}
