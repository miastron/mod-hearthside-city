/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CityLifeMgr.h"
#include "LifeBotReservation.h"

#include "Chat.h"
#include "CommandScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

class CityLifeWorldScript : public WorldScript
{
public:
    CityLifeWorldScript()
        : WorldScript("CityLifeWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sCityLifeMgr.LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sCityLifeMgr.Update(diff);
    }

    void OnShutdown() override
    {
        sCityLifeMgr.Shutdown();
        PlayerbotsLife::UnregisterActiveModule(PlayerbotsLife::ModuleSystemFlags::MODULE_CITY_LIFE);
    }
};

#include "Group.h"
#include "GroupScript.h"

class CityLifePlayerScript : public PlayerScript
{
public:
    CityLifePlayerScript()
        : PlayerScript("CityLifePlayerScript", {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_UPDATE_ZONE,
            PLAYERHOOK_CAN_INIT_TRADE,
            PLAYERHOOK_CAN_SET_TRADE_ITEM,
            PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE,
            PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS,
            PLAYERHOOK_ON_DUEL_END
        })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        sCityLifeMgr.OnPlayerLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        sCityLifeMgr.OnPlayerLogout(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        sCityLifeMgr.OnPlayerZoneUpdate(player, newZone);
    }

    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        if (type == DUEL_WON)
            sCityLifeMgr.QueueDuelEnd(winner, loser);
    }

    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        if (!sCityLifeMgr.IsInteractionsEnabled() || !sCityLifeMgr.IsPlayerTradingEnabled())
        {
            bool targetIsResident = target && sCityLifeMgr.IsResident(target->GetGUID().GetCounter());
            bool playerIsResident = player && sCityLifeMgr.IsResident(player->GetGUID().GetCounter());
            if (targetIsResident || playerIsResident)
                return false; // silently: residents say nothing unless a chat option is on
        }
        return true;
    }

    bool OnPlayerCanSetTradeItem(Player* player, Item* /*tradedItem*/, uint8 tradeSlot) override
    {
        if (tradeSlot == TRADE_SLOT_NONTRADED)
            return true;

        if (!sCityLifeMgr.IsInteractionsEnabled() || !sCityLifeMgr.IsPlayerTradingEnabled())
        {
            if (player && sCityLifeMgr.IsResident(player->GetGUID().GetCounter()))
                return false;
        }
        return true;
    }

    bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* /*achievement*/) override
    {
        if (!player || !sCityLifeMgr.IsBlockAchievementsEnabled())
            return true;

        // Never suppress real human players or human-controlled SelfBots
        if (::IsRealPlayer(player) || ::IsSelfBot(player))
            return true;

        uint32 const guidLow = player->GetGUID().GetCounter();
        if (sCityLifeMgr.IsResidentInSyntheticActivity(guidLow))
        {
            sCityLifeMgr.RecordSuppressedAchievement();
            return false;
        }

        return true;
    }

    bool OnPlayerBeforeCriteriaProgress(Player* player, AchievementCriteriaEntry const* /*criteria*/) override
    {
        if (!player || !sCityLifeMgr.IsBlockAchievementsEnabled())
            return true;

        // Never suppress real human players or human-controlled SelfBots
        if (::IsRealPlayer(player) || ::IsSelfBot(player))
            return true;

        uint32 const guidLow = player->GetGUID().GetCounter();
        if (sCityLifeMgr.IsResidentInSyntheticActivity(guidLow))
        {
            sCityLifeMgr.RecordSuppressedCriteria();
            return false;
        }

        return true;
    }
};

class CityLifeCommandScript : public CommandScript
{
public:
    CityLifeCommandScript() : CommandScript("CityLifeCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "citylife", HandleCityLifeCommand, SEC_GAMEMASTER, Console::Yes }
        };
        return commandTable;
    }

    static bool HandleCityLifeCommand(ChatHandler* handler, char const* args)
    {
        return sCityLifeMgr.HandleCommand(handler, args ? args : "");
    }
};

class CityLifeGroupScript : public GroupScript
{
public:
    CityLifeGroupScript()
        : GroupScript("CityLifeGroupScript", { GROUPHOOK_ON_ADD_MEMBER })
    {
    }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (!group)
            return;

        bool hasRealPlayer = false;
        for (GroupReference const* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && (CityLife::IsHumanPlayer(member) || ::IsSelfBot(member)))
            {
                hasRealPlayer = true;
                break;
            }
        }

        if (hasRealPlayer)
        {
            sCityLifeMgr.QueueDisengage(guid.GetCounter());
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && sCityLifeMgr.IsResident(member->GetGUID().GetCounter()))
                    sCityLifeMgr.QueueDisengage(member->GetGUID().GetCounter());
        }
    }
};

void AddCityLifeScripts()
{
    // A static_assert here could only compare this module's copy of LifeBotReservation.h with
    // itself; CheckProtocolVersion compares it with the copy the first registered Life module used.
    if (!PlayerbotsLife::CheckProtocolVersion())
        LOG_ERROR("server.loading", ">> City Life: LifeBotReservation.h protocol {} does not match {} from another Life "
            "module. Copy one LifeBotReservation.h into every Life module and rebuild.",
            PlayerbotsLife::LIFE_BOT_RESERVATION_PROTOCOL_VERSION, PlayerbotsLife::GetRegisteredProtocolVersion());

    // The active-module flag is set by Manager::LoadConfig, only while City Life is enabled and its tables exist.
    new CityLifeWorldScript();
    new CityLifePlayerScript();
    new CityLifeGroupScript();
    new CityLifeCommandScript();
    LOG_INFO("server.loading", ">> Loaded mod-playerbots-city-life (protocol={}, registry={:p})",
        PlayerbotsLife::GetReservationProtocolVersion(), PlayerbotsLife::GetReservationRegistryAddress());
}
