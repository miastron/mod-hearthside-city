/* Hearthside Life shared bot identity and bookkeeping; GPL-2.0-or-later, matching the modules. */
#ifndef LIFE_BOTS_H
#define LIFE_BOTS_H

// What a Life module needs to tell a borrowable bot from a person, and to give a bot back the way it
// found it. City Life owns this file; mod-hearthside-pvp includes it from ../../mod-hearthside-city/src/.
//
// - AccountFilter: <prefix>.Bots.AccountPrefix/AccountMin/AccountMax/AccountFilterMode, beside
//   mod-playerbots' own random-bot account list. Read from map-update threads, so it locks.
// - IsBotSession / IsHumanPlayer / IsHumanControlled: this core has no WorldSession::IsBot(), and
//   mod-playerbots attaches its AI after OnPlayerLogin, so ::IsRealPlayer() alone reads every bot
//   logging in as a human.
// - ArchetypeFilter: <prefix>.Bots.Archetypes, using mod-hearthside-chat's GUID-fixed personality when
//   that module is in the build; without it every bot fits.
// - Budget / CountLiveWorldBots: how many bots a module may hold.
// - StrategyJournal: records exactly what a module changed (strategies, "stay" position, PvP flag) so
//   Restore undoes only that and leaves whatever else changed meanwhile.
// - HoldLevels: keeps mod-playerbots' level brackets from re-leveling a held bot.

#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "LifeBotReservation.h"

#if __has_include("hs_archetype.h")
#include "hs_archetype.h"
#define LIFEBOTS_HAS_ARCHETYPES 1
#endif

#include <algorithm>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LifeBots
{
    inline std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    // Fills in what <prefix>.Bots.UsePlayerbotConfig imports from mod-playerbots: its random-bot account
    // prefix for an "auto" AccountPrefix, and its login delay as the least startup delay. Without the
    // import an "auto" prefix matches nothing (random bots still qualify through mod-playerbots' list).
    inline void ImportPlayerbotDefaults(bool usePlayerbotConfig, std::string& accountPrefix, uint32& startupDelaySeconds)
    {
        std::string lowered = Lower(accountPrefix);
        bool autoPrefix = lowered.empty() || lowered == "auto" || lowered == "playerbot" || lowered == "playerbots";
        if (!usePlayerbotConfig)
        {
            if (autoPrefix)
                accountPrefix.clear();
            return;
        }
        std::string configuredPrefix = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAccountPrefix",
            sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot", false), false);
        uint32 loginDelay = sConfigMgr->GetOption<uint32>("Playerbots.DisabledWithoutRealPlayerLoginDelay",
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DisabledWithoutRealPlayerLoginDelay", 30, false), false);
        if (autoPrefix)
            accountPrefix = configuredPrefix;
        startupDelaySeconds = std::max(startupDelaySeconds, loginDelay);
    }

    class AccountFilter
    {
    public:
        // requireBoth (AccountFilterMode = all): with both a prefix and a range set, an account must
        // match both; otherwise either one will do.
        void Configure(std::string prefix, uint32 min, uint32 max, bool requireBoth)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _prefix = Lower(std::move(prefix));
            _min = min;
            _max = max;
            _requireBoth = requireBoth;
        }

        std::string Prefix() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _prefix;
        }

        // The login-database query behind ApplyRefresh; run it async and hand the result over.
        std::string RefreshQuery() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::string prefix = _prefix;
            LoginDatabase.EscapeString(prefix);
            return Acore::StringFormat(
                "SELECT id,username FROM account WHERE ({} > 0 AND id BETWEEN {} AND {}) OR "
                "({} > 0 AND LEFT(LOWER(username),{})='{}')",
                _max, _min, _max, _prefix.size(), _prefix.size(), prefix);
        }

        // Built aside and swapped in, so lookups keep the previous answers until now.
        void ApplyRefresh(QueryResult const& result)
        {
            std::unordered_map<uint32, bool> fresh;
            std::lock_guard<std::mutex> lock(_mutex);
            if (result)
                do
                {
                    Field* fields = result->Fetch();
                    uint32 id = fields[0].Get<uint32>();
                    fresh[id] = Matches(id, Lower(fields[1].Get<std::string>()));
                } while (result->NextRow());
            _cache.swap(fresh);
        }

        bool IsBotAccount(uint32 accountId) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto cached = _cache.find(accountId);
            if (cached != _cache.end())
                return cached->second;
            // Not cached yet: the id range alone, unless the prefix must match too.
            return accountId && InRange(accountId) && !(_requireBoth && !_prefix.empty());
        }

    private:
        bool InRange(uint32 id) const { return _max > 0 && _max >= _min && id >= _min && id <= _max; }

        bool Matches(uint32 id, std::string const& username) const
        {
            bool range = InRange(id);
            bool prefix = !_prefix.empty() && username.rfind(_prefix, 0) == 0;
            if (_requireBoth && _max > 0 && !_prefix.empty())
                return range && prefix;
            return range || prefix;
        }

        mutable std::mutex _mutex;
        std::string _prefix;
        uint32 _min = 0;
        uint32 _max = 0;
        bool _requireBoth = false;
        std::unordered_map<uint32, bool> _cache;
    };

    // A playerbot's session, by account, whether or not mod-playerbots has attached its AI yet.
    inline bool IsBotSession(Player const* player, AccountFilter const& accounts)
    {
        if (!player || !player->GetSession())
            return false;
        uint32 accountId = player->GetSession()->GetAccountId();
        return accountId && (sPlayerbotAIConfig.IsInRandomAccountList(accountId) ||
                             sRandomPlayerbotMgr.IsAddclassBot(player->GetGUID().GetCounter()) ||
                             accounts.IsBotAccount(accountId));
    }

    // A person at a game client: not a bot account and no PlayerbotAI attached. A player-owned alt bot
    // is therefore neither borrowable nor human.
    inline bool IsHumanPlayer(Player const* player, AccountFilter const& accounts)
    {
        return player && player->GetSession() && !IsBotSession(player, accounts) &&
            !GET_PLAYERBOT_AI(const_cast<Player*>(player));
    }

    // Someone other than the bot's own AI is in charge: a person, a player's alt bot, or a group with
    // either in it. A module never touches such a bot's state.
    inline bool IsHumanControlled(Player* player, AccountFilter const& accounts)
    {
        if (!player || !player->GetSession())
            return true;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ::IsRealPlayer(player) || ::IsSelfBot(player) || ai->HasGameClientMaster())
            return true;
        if (Group* group = player->GetGroup())
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && (IsHumanPlayer(member, accounts) || ::IsSelfBot(member)))
                    return true;
        return false;
    }

    class ArchetypeFilter
    {
    public:
        // Reads `key` (comma-separated mod-hearthside-chat archetype names) and logs what is wrong with
        // it, on every load so a bad value is reported every time.
        void Load(std::string const& key, char const* tag, std::string const& fallback = "")
        {
            _list = sConfigMgr->GetOption<std::string>(key, fallback);
            _active = false;
            _mask = 0;
            if (_list.find_first_not_of(" \t,;") == std::string::npos)
                return;
#ifdef LIFEBOTS_HAS_ARCHETYPES
            _active = true;
            std::vector<std::string> unknown;
            _mask = Hs_ArchetypeMaskForNames(_list, unknown);
            for (std::string const& name : unknown)
                LOG_ERROR("module", "[{}] {}: unknown archetype '{}', ignored.", tag, key, name);
            if (!_mask)
                LOG_ERROR("module", "[{}] {} = \"{}\" names no known archetype: no bot will be borrowed until it is fixed or emptied.",
                    tag, key, _list);
#else
            LOG_WARN("module", "[{}] {} is set but mod-hearthside-chat (hs_archetype.h) is not in this build: "
                "the archetype filter is ignored and every bot may be borrowed.", tag, key);
#endif
        }

        // True if the list is empty, the chat module is not in the build, or the bot's archetype is listed.
        bool Fits(uint32 guidLow) const
        {
#ifdef LIFEBOTS_HAS_ARCHETYPES
            if (!_active)
                return true;
            uint64 raw = ObjectGuid::Create<HighGuid::Player>(guidLow).GetRawValue();
            uint32 archetype = static_cast<uint32>(Hs_ArchetypeForBot(raw));
            return archetype < 32 && (_mask & (1u << archetype)) != 0;
#else
            (void)guidLow;
            return true;
#endif
        }

        std::string const& List() const { return _list; }

    private:
        std::string _list;
        bool _active = false;
        uint32 _mask = 0; // with _active, 0 = no bot fits
    };

    // <prefix>.Population.BudgetMode: "percentage" of the live bots, or a "count" ("fixed") of them;
    // never more than maxTotal (0 = no cap) or the pool, and at least one while there is a pool.
    inline bool ParsePercentageBudget(std::string const& mode)
    {
        std::string lowered = Lower(mode);
        return lowered != "count" && lowered != "fixed";
    }

    inline uint32 Budget(bool percentage, uint32 fixedCount, uint32 borrowPercent, uint32 maxTotal, uint32 pool)
    {
        if (!pool)
            return 0;
        uint32 requested = percentage ? static_cast<uint32>(uint64(pool) * borrowPercent / 100) : fixedCount;
        uint32 upper = maxTotal ? std::min(maxTotal, pool) : pool;
        return std::clamp<uint32>(requested, 1, upper);
    }

    // Bots out in the open world that a Life module could borrow: random or configured bot accounts,
    // alive, nobody's alt, not in a battleground, its queue or an instance. Takes the player-map lock.
    inline uint32 CountLiveWorldBots(AccountFilter const& accounts)
    {
        uint32 count = 0;
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* player = entry.second;
            if (!player || !player->GetSession() || !player->IsInWorld())
                continue;
            if (IsHumanControlled(player, accounts) || !player->IsAlive() || player->InBattleground() ||
                player->InBattlegroundQueue() || !player->GetMap() || player->GetMap()->Instanceable())
                continue;
            if (sRandomPlayerbotMgr.IsRandomBot(player) || accounts.IsBotAccount(player->GetSession()->GetAccountId()))
                ++count;
        }
        return count;
    }

    // ---------------------------------------------------------------------------------------------
    // Strategy journal

    struct StrategyDelta
    {
        bool Captured = false;
        std::vector<std::string> Original; // before the module's first change
        std::vector<std::string> Added;    // by the module (siblings mod-playerbots swapped in included)
        std::vector<std::string> Removed;
    };

    struct StrategyJournal
    {
        StrategyDelta Combat;
        StrategyDelta NonCombat;
        bool StayCaptured = false;
        bool HadStay = false;
        float StayX = 0.0f, StayY = 0.0f, StayZ = 0.0f;
        uint32 StayMapId = 0;
        bool PvPCaptured = false;
        bool OriginalPvP = false;
        bool AppliedPvP = false;

        bool Captured() const { return Combat.Captured || NonCombat.Captured || StayCaptured || PvPCaptured; }
    };

    namespace Detail
    {
        inline bool Has(std::vector<std::string> const& list, std::string const& name)
        {
            return std::find(list.begin(), list.end(), name) != list.end();
        }

        inline void AddOnce(std::vector<std::string>& list, std::string const& name)
        {
            if (!Has(list, name))
                list.push_back(name);
        }
    }

    // Applies `changes` ("+a,-b,...") to one state and records what actually changed.
    inline void ChangeStrategies(PlayerbotAI* ai, StrategyJournal& journal, std::string const& changes, BotState state)
    {
        if (!ai || changes.empty())
            return;
        StrategyDelta& delta = state == BOT_STATE_COMBAT ? journal.Combat : journal.NonCombat;
        std::vector<std::string> before = ai->GetStrategies(state);
        if (!delta.Captured)
        {
            delta.Captured = true;
            delta.Original = before;
        }
        ai->ChangeStrategy(changes, state);
        std::vector<std::string> after = ai->GetStrategies(state);
        for (std::string const& name : after)
            if (!Detail::Has(before, name))
                Detail::AddOnce(delta.Added, name);
        for (std::string const& name : before)
            if (!Detail::Has(after, name))
                Detail::AddOnce(delta.Removed, name);
    }

    // Remembers the bot's own "stay" position once, before the module first moves it.
    inline void CaptureStay(PlayerbotAI* ai, StrategyJournal& journal)
    {
        if (!ai || journal.StayCaptured)
            return;
        PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
        auto stay = positions.find("stay");
        journal.StayCaptured = true;
        journal.HadStay = stay != positions.end() && stay->second.isSet();
        if (journal.HadStay)
        {
            journal.StayX = stay->second.x;
            journal.StayY = stay->second.y;
            journal.StayZ = stay->second.z;
            journal.StayMapId = stay->second.mapId;
        }
    }

    // Points mod-playerbots' "stay" position at (x, y, z) on mapId, journaled for the restore.
    inline void SetStay(PlayerbotAI* ai, StrategyJournal& journal, float x, float y, float z, uint32 mapId)
    {
        if (!ai)
            return;
        CaptureStay(ai, journal);
        ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get()["stay"].Set(x, y, z, mapId);
    }

    inline void SetPvP(Player* player, StrategyJournal& journal, bool on)
    {
        if (!player)
            return;
        if (!journal.PvPCaptured)
        {
            journal.PvPCaptured = true;
            journal.OriginalPvP = player->IsPvP();
        }
        player->SetPvP(on);
        journal.AppliedPvP = on;
    }

    namespace Detail
    {
        inline void RestoreState(PlayerbotAI* ai, StrategyDelta const& delta, BotState state)
        {
            if (!delta.Captured)
                return;
            std::vector<std::string> current = ai->GetStrategies(state);
            // Ours, still on, and not the bot's own to begin with: off again.
            for (std::string const& name : delta.Added)
                if (!Has(delta.Original, name) && Has(current, name))
                    ai->ChangeStrategy("-" + name, state);
            // Taken off by us and its own: back on, unless a sibling has taken its place since.
            for (std::string const& name : delta.Removed)
            {
                if (!Has(delta.Original, name) || Has(current, name))
                    continue;
                auto siblings = ai->GetAiObjectContext()->GetSiblingStrategy(name);
                std::vector<std::string> live = ai->GetStrategies(state);
                if (std::none_of(live.begin(), live.end(), [&](std::string const& s) { return siblings.count(s) != 0; }))
                    ai->ChangeStrategy("+" + name, state);
            }
        }
    }

    // Undoes what the journal recorded. The caller decides whether the bot is still its to restore
    // (not human-controlled, not taken over by a group or a battleground).
    inline void Restore(Player* player, StrategyJournal const& journal)
    {
        if (!player)
            return;
        if (journal.PvPCaptured && player->IsPvP() == journal.AppliedPvP && player->IsPvP() != journal.OriginalPvP)
            player->SetPvP(journal.OriginalPvP);
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai)
            return;
        Detail::RestoreState(ai, journal.Combat, BOT_STATE_COMBAT);
        Detail::RestoreState(ai, journal.NonCombat, BOT_STATE_NON_COMBAT);
        if (journal.StayCaptured)
        {
            PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
            if (journal.HadStay)
                positions["stay"].Set(journal.StayX, journal.StayY, journal.StayZ, journal.StayMapId);
            else if (auto stay = positions.find("stay"); stay != positions.end())
                stay->second.Reset();
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Level freeze

    // mod-playerbots' level brackets re-level a random bot in place (realm 2026-10-12: a leased level 9
    // dinged to 63 mid-duel-event). Each Life module hands over the names of the bots it holds now, every
    // management tick and an empty set at shutdown; their union is kept on mod-playerbots' exclude
    // lists. Those lists are by name and mod-hearthside-chat writes them too, so only names a Life
    // module put there are ever taken back off. One registry for the process (an inline function's
    // static is shared by every module that includes this header). World thread only.
    inline void HoldLevels(PlayerbotsLife::ReservationOwner owner, std::unordered_set<std::string> const& names)
    {
        struct Registry
        {
            std::unordered_map<uint8, std::unordered_set<std::string>> Held;
            std::unordered_set<std::string> Added;
        };
        static Registry registry;
        registry.Held[static_cast<uint8>(owner)] = names;

        std::unordered_set<std::string> held;
        for (auto const& [holder, set] : registry.Held)
            held.insert(set.begin(), set.end());

        auto& brackets = sPlayerbotAIConfig.LevelBracketsExcludeNames;
        auto& resets = sPlayerbotAIConfig.ResetBotLevelExcludeNames;
        for (std::string const& name : held)
        {
            bool added = false;
            for (auto* list : { &brackets, &resets })
                if (!Detail::Has(*list, name))
                {
                    list->push_back(name);
                    added = true;
                }
            if (added)
                registry.Added.insert(name);
        }
        for (auto it = registry.Added.begin(); it != registry.Added.end();)
        {
            if (held.count(*it))
            {
                ++it;
                continue;
            }
            for (auto* list : { &brackets, &resets })
                list->erase(std::remove(list->begin(), list->end(), *it), list->end());
            it = registry.Added.erase(it);
        }
    }
}

#endif
