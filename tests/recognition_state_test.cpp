// Pure C++ contract test for consecutive-hit confirmation, candidate changes,
// invalid scores, expiry, and reset. It has no external runtime dependency.
#include "recognition_state.h"
#include <cassert>
#include <limits>
#include <iostream>

int main() {
    RecognitionState s;
    const auto start = RecognitionState::Clock::now();
    using namespace std::chrono_literals;
    assert(s.observe(6, .83, .14, start));
    assert(!s.confirmed() && s.hits == 1);
    s.observe(6, .80, .14, start + 1s);
    s.observe(6, .79, .13, start + 2s);
    assert(s.confirmed());
    s.observe(7, .82, .12, start + 3s);
    assert(!s.confirmed() && s.id == 7 && s.hits == 1);
    assert(!s.observe(7, .64, .12, start + 4s));
    assert(s.hits == 0 && s.id == -1);
    assert(!s.observe(7, .8, .7, start + 5s));
    assert(!s.observe(7, std::numeric_limits<double>::quiet_NaN(), .1, start));
    s.observe(6, .8, .1, start + 6s);
    s.expire(start + 7500ms);
    assert(s.hits == 0);
    s.observe(6, .8, .1, start + 8s);
    s.observe(6, .8, .1, start + 10s);
    assert(s.hits == 1);
    s.reset(); // 无人、多人、对齐失败、图库变更均调用此路径。
    assert(!s.confirmed() && s.id == -1);
    std::cout << "Recognition state tests passed\n";
}
