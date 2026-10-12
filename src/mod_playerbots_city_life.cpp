/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CityLifeMgr.h"
#include "LifeBotReservation.h"
#include "LifeTelemetryBridge.h"

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
            PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS
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

    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        if (!sCityLifeMgr.IsInteractionsEnabled() || !sCityLifeMgr.IsPlayerTradingEnabled())
        {
            bool targetIsResident = target && sCityLifeMgr.IsResident(target->GetGUID().GetCounter());
            bool playerIsResident = player && sCityLifeMgr.IsResident(player->GetGUID().GetCounter());
            if (targetIsResident || playerIsResident)
            {
                if (targetIsResident)
                    target->Whisper("Trading is disabled.", LANG_UNIVERSAL, player);
                return false;
            }
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
        if (::IsRealPlayer(player))
            return true;

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            if (ai->IsSelfBot())
                return true;
        }

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
        if (::IsRealPlayer(player))
            return true;

        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            if (ai->IsSelfBot())
                return true;
        }

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
            Player const* member = ref->GetSource();
            if (member && member->GetSession() && !member->GetSession()->IsBot())
            {
                hasRealPlayer = true;
                break;
            }
        }

        if (hasRealPlayer)
        {
            sCityLifeMgr.DisengageResident(guid.GetCounter());
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && sCityLifeMgr.IsResident(member->GetGUID().GetCounter()))
                    sCityLifeMgr.DisengageResident(member->GetGUID().GetCounter());
        }
    }
};

void AddCityLifeScripts()
{
    static_assert(PlayerbotsLife::LIFE_BOT_RESERVATION_PROTOCOL_VERSION == 2026101104,
        "City Life requires LifeBotReservation protocol version 2026101104");

    PlayerbotsLife::RegisterActiveModule(PlayerbotsLife::ModuleSystemFlags::MODULE_CITY_LIFE);
    PlayerbotsLife::RegisterTelemetryBridge();
    new CityLifeWorldScript();
    new CityLifePlayerScript();
    new CityLifeGroupScript();
    new CityLifeCommandScript();
    LOG_INFO("server.loading", ">> Loaded mod-playerbots-city-life (protocol={}, registry={:p})",
        PlayerbotsLife::GetReservationProtocolVersion(), PlayerbotsLife::GetReservationRegistryAddress());
}
