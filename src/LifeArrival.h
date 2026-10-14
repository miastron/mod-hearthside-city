/* Hearthside Life shared arrivals and departures; GPL-2.0-or-later, matching the modules. */
#ifndef LIFE_ARRIVAL_H
#define LIFE_ARRIVAL_H

// How a borrowed bot comes and goes where a real player could see it. City Life owns this file;
// mod-hearthside-pvp includes it from ../../mod-hearthside-city/src/.
//
// - FindDropOff: a spot out of every real player's sight, on a plain walkable route to the
//   destination, where a bot can be teleported and then walk (or ride) the rest of the way in.
// - WalkOff: a released bot in sight of a real player walks away and carries on as an ordinary bot
//   there, instead of vanishing in a teleport.

#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

namespace LifeArrival
{
    // A bot this close walks in; farther (or on another map) it is dropped off first. Below
    // mod-playerbots' ReactDistance (150) so its own "return to stay position" keeps walking it.
    constexpr float WalkInRange = 140.0f;
    // Drop-off ring around the destination: past what anyone standing there notices, inside WalkInRange.
    constexpr float DropOffMinDistance = 70.0f;
    constexpr float DropOffMaxDistance = 120.0f;
    constexpr uint32 DropOffAttempts = 24;
    // How long to let a drop-off teleport land before the walk in starts.
    constexpr uint32 LandingMs = 2500;

    // isHuman(Player const*) is the module's human test. False when nobody stands near the
    // destination (heights and paths are only known where a grid is loaded, and nobody is there to
    // see an arrival either) or no spot passes.
    template <class IsHuman>
    bool FindDropOff(Player* bot, uint32 mapId, float tx, float ty, float tz, Position& dropOff, IsHuman isHuman)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(mapId);
        if (!bot || !map || map->Instanceable())
            return false;

        // Measure from someone already standing near the destination.
        Player* anchor = nullptr;
        std::vector<Player*> watchers;
        for (auto const& ref : map->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || p == bot || !p->IsInWorld() || p->IsBeingTeleported())
                continue;
            float d = p->GetExactDist2d(tx, ty);
            if (d > 250.0f)
                continue;
            if (isHuman(p))
                watchers.push_back(p);
            if (d < 60.0f && p->IsAlive() && !p->IsFlying() && (!anchor || d < anchor->GetExactDist2d(tx, ty)))
                anchor = p;
        }
        if (!anchor)
            return false;

        // Hostile creatures that would pull the bot on its way in.
        std::list<Unit*> units;
        float searchRadius = anchor->GetExactDist2d(tx, ty) + DropOffMaxDistance + 30.0f;
        Acore::AnyUnitInObjectRangeCheck check(anchor, searchRadius);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(anchor, units, check);
        Cell::VisitObjects(anchor, searcher, searchRadius);
        std::vector<Position> hostiles;
        for (Unit* unit : units)
        {
            Creature* creature = unit->ToCreature();
            if (!creature || creature->IsCritter() || creature->IsTotem() || creature->IsPet() ||
                creature->GetReactState() != REACT_AGGRESSIVE || !creature->IsHostileTo(bot) ||
                creature->GetLevel() + 10 < bot->GetLevel())
                continue;
            hostiles.push_back(creature->GetPosition());
        }
        auto nearHostile = [&](float x, float y, float radius)
        {
            return std::any_of(hostiles.begin(), hostiles.end(),
                [&](Position const& h) { return h.GetExactDist2d(x, y) < radius; });
        };

        for (uint32 attempt = 0; attempt < DropOffAttempts; ++attempt)
        {
            float angle = frand(0.0f, 2.0f * float(M_PI));
            float distance = frand(DropOffMinDistance, DropOffMaxDistance);
            float cx = tx + std::cos(angle) * distance;
            float cy = ty + std::sin(angle) * distance;
            float cz = map->GetHeight(anchor->GetPhaseMask(), cx, cy, tz + 30.0f, true, 80.0f);
            if (cz <= INVALID_HEIGHT + 1.0f || std::fabs(cz - tz) > 35.0f)
                continue;
            if (map->IsInWater(anchor->GetPhaseMask(), cx, cy, cz, bot->GetCollisionHeight()))
                continue;

            // Out of sight: nobody close, and nobody in range with a clear line to it.
            bool seen = std::any_of(watchers.begin(), watchers.end(), [&](Player* w)
            {
                float d = w->GetExactDist(cx, cy, cz);
                return d < 45.0f || (d < 110.0f && w->IsWithinLOS(cx, cy, cz + 2.0f));
            });
            if (seen || nearHostile(cx, cy, 30.0f))
                continue;

            // A plain walkable route: no partial path, no long detour around a cliff, no ledge drops,
            // no hostile camp beside the road.
            PathGenerator path(anchor);
            if (!path.CalculatePath(cx, cy, cz, tx, ty, tz, false) || path.GetPathType() != PATHFIND_NORMAL)
                continue;
            ::Movement::PointsArray const& points = path.GetPath();
            if (points.size() < 2)
                continue;
            float length = 0.0f;
            bool clear = true;
            for (size_t i = 1; i < points.size() && clear; ++i)
            {
                G3D::Vector3 const& a = points[i - 1];
                G3D::Vector3 const& b = points[i];
                length += (b - a).length();
                if (std::fabs(b.z - a.z) > 4.5f || nearHostile(b.x, b.y, 20.0f))
                    clear = false;
            }
            if (!clear || length > distance * 1.6f + 15.0f)
                continue;

            dropOff.Relocate(cx, cy, cz + 0.5f, std::atan2(ty - cy, tx - cx));
            return true;
        }
        return false;
    }

    // A real player close to (x, y, z), or in range with a clear line to it: something appearing there
    // would be seen. The same test FindDropOff applies to its candidate spots.
    template <class IsHuman>
    bool InSight(uint32 mapId, float x, float y, float z, IsHuman isHuman)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(mapId);
        if (!map || map->Instanceable())
            return false;
        for (auto const& ref : map->GetPlayers())
            if (Player* w = ref.GetSource(); w && w->IsInWorld() && isHuman(w))
            {
                float d = w->GetExactDist(x, y, z);
                if (d < 45.0f || (d < 110.0f && w->IsWithinLOS(x, y, z + 2.0f)))
                    return true;
            }
        return false;
    }

    // Walks a released bot 40-80 yards off and holds mod-playerbots' own random teleport off for
    // 20-40 minutes, so it carries on here as an ordinary bot rather than vanishing in front of a player.
    inline void WalkOff(Player* bot)
    {
        if (!bot || !bot->IsAlive() || !bot->GetMap())
            return;
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
            sRandomPlayerbotMgr.ScheduleTeleport(bot->GetGUID().GetCounter(), urand(1200, 2400));
        float angle = frand(0.0f, 2.0f * float(M_PI));
        float x = bot->GetPositionX() + std::cos(angle) * frand(40.0f, 80.0f);
        float y = bot->GetPositionY() + std::sin(angle) * frand(40.0f, 80.0f);
        float z = bot->GetPositionZ();
        if (bot->GetMap()->CanReachPositionAndGetValidCoords(bot, x, y, z))
            bot->GetMotionMaster()->MovePoint(0, x, y, z);
    }
}

#endif
