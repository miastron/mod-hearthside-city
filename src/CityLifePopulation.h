/*
 * City Life population allocation
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CITY_LIFE_POPULATION_H
#define CITY_LIFE_POPULATION_H

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace CityLife
{
    struct PopulationWeight
    {
        std::uint32_t HubId = 0;
        std::uint32_t Weight = 0;
        std::uint32_t Minimum = 0;
        std::uint32_t Priority = 0;
    };

    // Inputs describe usable hubs; POI count is not a population ceiling.
    inline std::vector<std::uint32_t> AllocatePopulation(
        std::vector<PopulationWeight> const& hubs, std::uint32_t requested)
    {
        std::vector<std::uint32_t> targets(hubs.size(), 0);
        std::vector<std::size_t> order(hubs.size());
        std::iota(order.begin(), order.end(), 0);
        auto priority = [&](std::size_t a, std::size_t b)
        {
            return hubs[a].Priority != hubs[b].Priority
                ? hubs[a].Priority > hubs[b].Priority : hubs[a].HubId < hubs[b].HubId;
        };
        std::sort(order.begin(), order.end(), priority);
        std::uint32_t remaining = requested;
        std::uint64_t totalWeight = 0;
        for (std::size_t index : order)
        {
            if (!hubs[index].Weight)
                continue;
            targets[index] = std::min(remaining, hubs[index].Minimum);
            remaining -= targets[index];
            totalWeight += hubs[index].Weight;
        }
        if (!remaining || !totalWeight)
            return targets;
        std::vector<std::uint64_t> remainders(hubs.size(), 0);
        std::uint32_t allocated = 0;
        for (std::size_t index : order)
        {
            std::uint64_t product = std::uint64_t(hubs[index].Weight) * remaining;
            std::uint32_t share = static_cast<std::uint32_t>(product / totalWeight);
            targets[index] += share;
            allocated += share;
            remainders[index] = product % totalWeight;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
        {
            return remainders[a] != remainders[b] ? remainders[a] > remainders[b] : priority(a, b);
        });
        std::uint32_t leftover = remaining - allocated;
        for (std::size_t index : order)
            if (leftover && hubs[index].Weight)
            {
                ++targets[index];
                --leftover;
            }
        return targets;
    }
}
#endif
