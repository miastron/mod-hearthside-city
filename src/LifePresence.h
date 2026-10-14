/* Hearthside Life shared real-player presence; GPL-2.0-or-later, matching the modules. */
#ifndef LIFE_PRESENCE_H
#define LIFE_PRESENCE_H

// Where the real players are, as the Life modules need it. City Life owns this file;
// mod-hearthside-pvp includes it from ../../mod-hearthside-city/src/.
//
// - Snapshot: the real players online, refreshed at most every three seconds (world thread).
// - Wakes: mod-playerbots' BotActiveAlone wake rule for a place. Bots there are awake, so a module
//   gives such places its budget first; it never sizes anything by it.
// - Watched / AnyoneNear: whether an arrival or a departure would be seen.
//
// The modules each keep their own Snapshot, built with their own human test (LifeBots::IsHumanPlayer
// with the module's account filter).

#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "WorldSession.h"

#include <mutex>
#include <shared_mutex>
#include <vector>

namespace LifePresence
{
    struct RealPlayer
    {
        uint32 GuidLow = 0;
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        uint32 AreaId = 0;
        TeamId Team = TEAM_NEUTRAL;
        uint8 Level = 0;
        float X = 0.0f, Y = 0.0f, Z = 0.0f, O = 0.0f;
        bool IsAlive = true;
        bool IsPvP = false;
        bool InCombat = false;
        bool HasDuel = false;
        bool IsGameMaster = false;
        bool HiddenGm = false; // a GM out of sight wakes no bots in mod-playerbots, so it is no presence here
        uint32 Security = 0;
        bool IsGroupLeader = false;
        uint32 GroupMembersCount = 0;
    };

    class Snapshot
    {
    public:
        // isHuman(Player const*) is the module's human test. Takes the player-map lock.
        template <class IsHuman>
        void Refresh(IsHuman isHuman, bool force = false)
        {
            uint32 now = static_cast<uint32>(GameTime::GetGameTimeMS().count());
            if (!force && _refreshedMs && now - _refreshedMs < RefreshMs)
                return;
            std::vector<RealPlayer> players;
            {
                std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
                for (auto const& entry : ObjectAccessor::GetPlayers())
                {
                    Player* player = entry.second;
                    if (!player || !player->IsInWorld() || !isHuman(player))
                        continue;
                    RealPlayer p;
                    p.GuidLow = player->GetGUID().GetCounter();
                    p.MapId = player->GetMapId();
                    p.ZoneId = player->GetZoneId();
                    p.AreaId = player->GetAreaId();
                    p.Team = player->GetTeamId();
                    p.Level = player->GetLevel();
                    p.X = player->GetPositionX();
                    p.Y = player->GetPositionY();
                    p.Z = player->GetPositionZ();
                    p.O = player->GetOrientation();
                    p.IsAlive = player->IsAlive();
                    p.IsPvP = player->IsPvP();
                    p.InCombat = player->IsInCombat();
                    p.HasDuel = player->duel != nullptr;
                    p.IsGameMaster = player->IsGameMaster();
                    p.HiddenGm = player->IsGameMaster() && !player->isGMVisible();
                    p.Security = player->GetSession()->GetSecurity();
                    Group* group = player->GetGroup();
                    p.IsGroupLeader = group && group->IsLeader(player->GetGUID());
                    p.GroupMembersCount = group ? group->GetMembersCount() : 0;
                    players.push_back(p);
                }
            }
            _refreshedMs = now;
            _players = std::move(players);
        }

        std::vector<RealPlayer> const& Players() const { return _players; }

        // mod-playerbots' BotActiveAlone wake rule for a place on mapId: a real player in zoneId
        // (ForceWhenInZone; 0 = the zone is unknown), anywhere on the map (ForceWhenInMap), or within
        // ForceWhenInRadius of any of `points` (x, y pairs).
        bool Wakes(uint32 mapId, uint32 zoneId, std::vector<std::pair<float, float>> const& points) const
        {
            bool checkMap = sPlayerbotAIConfig.BotActiveAloneForceWhenInMap;
            bool checkZone = sPlayerbotAIConfig.BotActiveAloneForceWhenInZone;
            float range = float(sPlayerbotAIConfig.BotActiveAloneForceWhenInRadius);
            for (RealPlayer const& player : _players)
            {
                if (player.MapId != mapId || player.HiddenGm)
                    continue;
                if (checkMap || (checkZone && zoneId && player.ZoneId == zoneId))
                    return true;
                if (range > 0.0f)
                    for (auto const& [x, y] : points)
                    {
                        float dx = player.X - x;
                        float dy = player.Y - y;
                        if (dx * dx + dy * dy < range * range)
                            return true;
                    }
            }
            return false;
        }

    private:
        static constexpr uint32 RefreshMs = 3000;
        uint32 _refreshedMs = 0;
        std::vector<RealPlayer> _players;
    };

    // A real player on the bot's map within `range` of it: a departure there is seen.
    template <class IsHuman>
    bool Watched(Player* bot, IsHuman isHuman, float range = 150.0f)
    {
        if (!bot || !bot->GetMap())
            return false;
        for (auto const& ref : bot->GetMap()->GetPlayers())
            if (Player* p = ref.GetSource(); p && p != bot && p->IsInWorld() && isHuman(p) &&
                p->GetExactDist2d(bot) <= range)
                return true;
        return false;
    }

    // A real player within `range` of (x, y) on mapId's open-world map: an arrival there could be seen.
    template <class IsHuman>
    bool AnyoneNear(uint32 mapId, float x, float y, IsHuman isHuman, float range = 250.0f)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(mapId);
        if (!map || map->Instanceable())
            return false;
        for (auto const& ref : map->GetPlayers())
            if (Player* p = ref.GetSource(); p && p->IsInWorld() && isHuman(p) && p->GetExactDist2d(x, y) <= range)
                return true;
        return false;
    }
}

#endif
