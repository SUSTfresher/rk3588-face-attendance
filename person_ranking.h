#pragma once

/*
 * Person-level reduction for a multi-sample recognition gallery.
 *
 * Matching is scored against samples, but recognition decisions are made per
 * person: the best sample for each person survives, then people are sorted.
 * This prevents a person with multiple enrollment images from occupying both
 * first and second place and corrupting the ambiguity-gap calculation.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

struct PersonScore {
    // referenceIndex identifies the winning gallery sample after reduction.
    std::int64_t personId;
    std::size_t referenceIndex;
    double score;
};

// Keeps the highest score per person, then ranks distinct people. Ties use id
// for deterministic logs/tests. Non-finite or out-of-range scores are ignored;
// callers must require at least two people before using a second-place gap.
inline std::vector<PersonScore> rankPeople(const std::vector<PersonScore>& samples)
{
    std::map<std::int64_t, PersonScore> bestByPerson;
    for (const auto& sample : samples) {
        if (sample.personId <= 0 || !std::isfinite(sample.score) ||
            sample.score < -1 || sample.score > 1) continue;
        const auto found = bestByPerson.find(sample.personId);
        if (found == bestByPerson.end()) bestByPerson.emplace(sample.personId, sample);
        else if (sample.score > found->second.score) found->second = sample;
    }
    std::vector<PersonScore> ranked;
    for (const auto& entry : bestByPerson) ranked.push_back(entry.second);
    std::sort(ranked.begin(), ranked.end(), [](const PersonScore& a, const PersonScore& b) {
        return a.score != b.score ? a.score > b.score : a.personId < b.personId;
    });
    return ranked;
}
