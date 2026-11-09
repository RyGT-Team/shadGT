#include "../native/parallel_mode_control.h"
#include <cassert>

int main() {
    using namespace Native::Parallel;
    assert(InitialMode(nullptr, "0"));
    assert(InitialMode("", "0"));
    assert(InitialMode("mode", nullptr));
    assert(InitialMode("mode", "1"));
    assert(InitialMode("mode", "00"));
    assert(!InitialMode("mode", "0"));
    AppliedMode serial(false);
    assert(!serial.Enabled());
    assert(!serial.Apply({1, true}, false));
    assert(!serial.Enabled());
    assert(serial.Apply({1, true}, true));
    assert(serial.Enabled());
    assert(serial.Apply({2, false}, true));
    assert(!serial.Enabled());
}
