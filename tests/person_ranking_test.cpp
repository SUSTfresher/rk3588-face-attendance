// Pure C++ regression test for person-level ranking and its hand-off into the
// confirmation state machine. No Qt, model, image, camera, or database is used.
#include "person_ranking.h"
#include "recognition_state.h"
#include <cassert>
#include <limits>
#include <iostream>

int main() {
    auto ranked = rankPeople({{2, 0, .80}, {2, 1, .85}, {3, 2, .17}, {4, 3, .3}});
    assert(ranked.size() == 3 && ranked[0].personId == 2);
    assert(ranked[0].referenceIndex == 1 && ranked[1].personId == 4);
    assert(rankPeople({{2, 0, .8}, {2, 1, .9}}).size() == 1);
    assert(rankPeople({}).empty());
    assert(rankPeople({{0, 0, .8}, {2, 1, 1.2},
        {3, 2, std::numeric_limits<double>::quiet_NaN()}}).empty());
    ranked = rankPeople({{3, 0, .8}, {2, 1, .8}});
    assert(ranked[0].personId == 2 && ranked[1].personId == 3);
    RecognitionState state;
    const auto now = RecognitionState::Clock::now();
    // 同一人员的获胜样本变化，不打断人员级别的连续确认。
    for (int i = 0; i < 3; ++i) {
        ranked = rankPeople({{2, 0, i == 1 ? .7 : .9},
                             {2, 1, .85}, {3, 2, .15}});
        assert(state.observe(ranked[0].personId, ranked[0].score, ranked[1].score,
                             now + std::chrono::seconds(i)));
    }
    assert(state.confirmed() && state.id == 2);
    state.observe(3, .9, .2, now + std::chrono::seconds(3));
    assert(state.hits == 1 && state.id == 3);
    std::cout << "Person ranking tests passed\n";
}
