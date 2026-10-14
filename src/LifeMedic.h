/* Hearthside Life shared duel medics; GPL-2.0-or-later, matching the modules. */
#ifndef LIFE_MEDIC_H
#define LIFE_MEDIC_H

// A healer watching a duel now and then heals the duelists once it is over, the way players at a duel
// ground do. City Life owns this file; mod-hearthside-pvp includes it from ../../mod-hearthside-city/src/.
//
// It is the same healer or two who do it: whether a healer bot is a "medic" at all is fixed by its GUID
// (MedicPercent of healers), only medics within Radius who have the duel in front of them take part, and
// even then only on Chance of the duels. Each module picks candidates among the bots it holds (City its
// residents, PvP its idle duelists), schedules the heals from Plan, and re-checks its lease before Heal.

#include "AiFactory.h"
#include "Config.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"

#include <algorithm>
#include <string>
#include <vector>

namespace LifeMedic
{
    struct Config
    {
        bool Enable = true;
        uint32 MedicPercent = 30; // of healer bots, fixed by GUID
        uint32 Chance = 50;       // % of finished duels a medic in range tends at all
        uint32 MaxPerDuel = 1;    // per module: City's and PvP's medics may both tend one duel
        float Radius = 30.0f;
    };

    // Reads <prefix>.Enable, .MedicPercent, .Chance, .MaxPerDuel and .Radius.
    inline Config LoadConfig(std::string const& prefix)
    {
        Config c;
        c.Enable = sConfigMgr->GetOption<bool>(prefix + ".Enable", c.Enable);
        c.MedicPercent = std::min<uint32>(100, sConfigMgr->GetOption<uint32>(prefix + ".MedicPercent", c.MedicPercent));
        c.Chance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>(prefix + ".Chance", c.Chance));
        c.MaxPerDuel = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(prefix + ".MaxPerDuel", c.MaxPerDuel), 1, 5);
        c.Radius = std::clamp(sConfigMgr->GetOption<float>(prefix + ".Radius", c.Radius), 5.0f, 40.0f);
        return c;
    }

    // A duel that ended with a winner (queued from PlayerScript::OnPlayerDuelEnd, which can run on a
    // map-update thread; the module handles it on the world thread).
    struct DuelEnd
    {
        uint32 WinnerGuid = 0;
        uint32 LoserGuid = 0;
        uint32 MapId = 0;
        float X = 0.0f, Y = 0.0f, Z = 0.0f; // between the two duelists
    };

    inline DuelEnd MakeDuelEnd(Player* winner, Player* loser)
    {
        DuelEnd end;
        end.WinnerGuid = winner->GetGUID().GetCounter();
        end.LoserGuid = loser->GetGUID().GetCounter();
        end.MapId = winner->GetMapId();
        end.X = (winner->GetPositionX() + loser->GetPositionX()) * 0.5f;
        end.Y = (winner->GetPositionY() + loser->GetPositionY()) * 0.5f;
        end.Z = (winner->GetPositionZ() + loser->GetPositionZ()) * 0.5f;
        return end;
    }

    struct PendingHeal
    {
        uint32 HealerGuid = 0;
        uint32 TargetGuid = 0;
        uint32 DueMs = 0;
    };

    // Holy/Discipline priest, Holy paladin, Restoration shaman or druid.
    inline bool IsHealer(Player* bot)
    {
        uint8 tab = AiFactory::GetPlayerSpecTab(bot);
        switch (bot->getClass())
        {
            case CLASS_PRIEST: return tab == 0 || tab == 1;
            case CLASS_PALADIN: return tab == 0;
            case CLASS_SHAMAN: return tab == 2;
            case CLASS_DRUID: return tab == 2;
            default: return false;
        }
    }

    // The same few healers every time: a stable hash of the GUID.
    inline bool IsMedic(uint32 guidLow, uint32 percent)
    {
        return (guidLow * 2654435761u >> 8) % 100 < percent;
    }

    // Free to tend this duel: a medic healer, idle, close and facing it.
    inline bool CanTend(Player* healer, DuelEnd const& end, Config const& c)
    {
        if (!healer || !healer->IsInWorld() || !healer->IsAlive() || healer->GetMapId() != end.MapId ||
            healer->IsInCombat() || healer->duel || healer->isMoving() || healer->IsNonMeleeSpellCast(false) ||
            healer->IsMounted() || healer->GetGUID().GetCounter() == end.WinnerGuid ||
            healer->GetGUID().GetCounter() == end.LoserGuid ||
            !IsMedic(healer->GetGUID().GetCounter(), c.MedicPercent) || !IsHealer(healer))
            return false;
        Position where(end.X, end.Y, end.Z);
        return healer->GetExactDist2d(end.X, end.Y) <= c.Radius && healer->HasInArc(float(M_PI) * 2.0f / 3.0f, &where);
    }

    // Who heals whom, if anyone: rolls Chance once for the duel, then the nearest MaxPerDuel medics
    // among `candidates`, each 1-3.5 s on. The first takes the loser; a second takes the winner.
    inline std::vector<PendingHeal> Plan(std::vector<Player*> candidates, DuelEnd const& end, Config const& c, uint32 now)
    {
        std::vector<PendingHeal> heals;
        if (!c.Enable || urand(1, 100) > c.Chance)
            return heals;
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [&](Player* healer) { return !CanTend(healer, end, c); }), candidates.end());
        std::sort(candidates.begin(), candidates.end(), [&](Player* a, Player* b)
            { return a->GetExactDist2d(end.X, end.Y) < b->GetExactDist2d(end.X, end.Y); });
        for (size_t i = 0; i < candidates.size() && i < c.MaxPerDuel; ++i)
        {
            PendingHeal heal;
            heal.HealerGuid = candidates[i]->GetGUID().GetCounter();
            heal.TargetGuid = i % 2 == 0 ? end.LoserGuid : end.WinnerGuid;
            heal.DueMs = now + urand(1000, 3500);
            heals.push_back(heal);
        }
        return heals;
    }

    // Casts a heal the healer knows on the target if it is hurt and in reach. A GM in GM mode is left alone.
    inline bool Heal(Player* healer, Player* target)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(healer);
        if (!ai || !target || !target->IsInWorld() || !target->IsAlive() || target->IsGameMaster() ||
            target->GetMapId() != healer->GetMapId() || target->duel ||
            target->GetHealth() >= target->GetMaxHealth() || !healer->IsWithinDistInMap(target, 38.0f) ||
            !healer->IsWithinLOSInMap(target) || healer->IsInCombat() || healer->duel ||
            healer->IsNonMeleeSpellCast(false))
            return false;

        static char const* const priest[] = { "flash heal", "renew", "heal", "lesser heal", nullptr };
        static char const* const paladin[] = { "flash of light", "holy light", nullptr };
        static char const* const shaman[] = { "riptide", "lesser healing wave", "healing wave", nullptr };
        static char const* const druid[] = { "rejuvenation", "regrowth", "healing touch", nullptr };
        char const* const* spells = nullptr;
        switch (healer->getClass())
        {
            case CLASS_PRIEST: spells = priest; break;
            case CLASS_PALADIN: spells = paladin; break;
            case CLASS_SHAMAN: spells = shaman; break;
            case CLASS_DRUID: spells = druid; break;
            default: return false;
        }
        // Not always the same spell first: a quick one or a slow one, as the healer likes.
        std::vector<char const*> order;
        for (char const* const* spell = spells; *spell; ++spell)
            order.push_back(*spell);
        if (order.size() > 1 && urand(0, 2) == 0)
            std::swap(order[0], order[1]);
        healer->SetFacingToObject(target);
        for (char const* spell : order)
            if (ai->CanCastSpell(spell, target) && ai->CastSpell(spell, target))
                return true;
        return false;
    }
}

#endif
