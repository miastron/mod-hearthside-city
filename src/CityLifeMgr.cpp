/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "CityLifeMgr.h"
#include "CityLifeActivities.h"
#include "CityLifeDialogues.h"
#include "CityLifeMovement.h"
#include "CityLifePopulation.h"
#include "Item.h"
#include "StringFormat.h"
#include "LifeBotReservation.h"

#include "AreaDefines.h"
#include "Battlefield.h"
#include "BattlefieldMgr.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "WorldSession.h"


// Archetype filter: mod-hearthside-chat's GUID-fixed personality decides which
// bots this module may borrow (CityLife.Bots.Archetypes). Without that module in the
// build every archetype fits.
#if __has_include("hs_archetype.h")
#include "hs_archetype.h"
#define CITYLIFE_HAS_ARCHETYPES 1
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iterator>
#include <limits>
#include <shared_mutex>
#include <sstream>

namespace
{
    constexpr float TwoPi = 6.28318530718f;
    constexpr uint32 ReservationSeconds = 20 * MINUTE;
    constexpr uint32 ReservationRefreshSeconds = 10 * MINUTE;

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    std::vector<std::string> Tokenize(std::string const& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> out;
        std::string token;
        while (stream >> token)
            out.push_back(token);
        return out;
    }

    uint32 GameTimeSeconds()
    {
        return static_cast<uint32>(GameTime::GetGameTime().count());
    }

    uint32 GameTimeMS()
    {
        return static_cast<uint32>(GameTime::GetGameTimeMS().count());
    }

    char const* TeamName(CityLife::TeamSide team)
    {
        switch (team)
        {
            case CityLife::TeamSide::Alliance:
                return "Alliance";
            case CityLife::TeamSide::Horde:
                return "Horde";
            default:
                return "Any";
        }
    }

    bool ParseTeam(std::string token, CityLife::TeamSide& out)
    {
        token = Lower(token);
        if (token == "a" || token == "ally" || token == "alliance")
        {
            out = CityLife::TeamSide::Alliance;
            return true;
        }
        if (token == "h" || token == "horde")
        {
            out = CityLife::TeamSide::Horde;
            return true;
        }
        if (token == "any" || token == "both" || token == "neutral")
        {
            out = CityLife::TeamSide::Any;
            return true;
        }
        return false;
    }

    bool ParseUInt(std::string const& text, uint32& out)
    {
        try
        {
            size_t used = 0;
            unsigned long value = std::stoul(text, &used);
            if (used != text.size() || value > std::numeric_limits<uint32>::max())
                return false;
            out = static_cast<uint32>(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseFloat(std::string const& text, float& out)
    {
        try
        {
            size_t used = 0;
            out = std::stof(text, &used);
            return used == text.size();
        }
        catch (...)
        {
            return false;
        }
    }

    CityLife::CityPoiType CategoryToPoiType(std::string const& category)
    {
        std::string cat = Lower(category);
        if (cat == "inn")
            return CityLife::CityPoiType::Inn;
        if (cat == "auction" || cat == "ah")
            return CityLife::CityPoiType::AuctionHouse;
        if (cat == "mailbox" || cat == "mail")
            return CityLife::CityPoiType::Mailbox;
        if (cat == "tavern")
            return CityLife::CityPoiType::Tavern;
        if (cat == "trainer")
            return CityLife::CityPoiType::Trainer;
        if (cat == "dock" || cat == "harbor" || cat == "fish")
            return CityLife::CityPoiType::Dock;
        if (cat == "wall" || cat == "patrol")
            return CityLife::CityPoiType::WallPatrol;
        if (cat == "vendor" || cat == "food")
            return CityLife::CityPoiType::Vendor;
        if (cat == "duel" || cat == "ring")
            return CityLife::CityPoiType::DuelRing;
        if (cat == "bed" || cat == "innbed")
            return CityLife::CityPoiType::InnBed;
        if (cat == "bank")
            return CityLife::CityPoiType::Bank;
        return CityLife::CityPoiType::Plaza;
    }

    // The kind of spot a resident heads for when it picks this activity next.
    CityLife::CityPoiType PoiForActivity(CityLife::CitizenActivity activity)
    {
        using CityLife::CitizenActivity;
        using CityLife::CityPoiType;
        switch (activity)
        {
            case CitizenActivity::VisitAuctionHouse: return CityPoiType::AuctionHouse;
            case CitizenActivity::VisitMailbox:      return CityPoiType::Mailbox;
            case CitizenActivity::VisitTrainer:      return CityPoiType::Trainer;
            case CitizenActivity::BuyFood:           return CityPoiType::Vendor;
            case CitizenActivity::WalkCityWalls:     return CityPoiType::WallPatrol;
            case CitizenActivity::FishAtDocks:       return CityPoiType::Dock;
            case CitizenActivity::SitInTavern:
            case CitizenActivity::DrinkInTavern:     return CityPoiType::Tavern;
            case CitizenActivity::ReturnToInn:       return CityPoiType::Inn;
            case CitizenActivity::WatchDuels:        return CityPoiType::DuelRing;
            default:                                 return CityPoiType::Plaza;
        }
    }

    // True when the spot is one PoiForActivity(activity) asks for (a tavern visit also fits an inn).
    bool SpotFitsActivity(CityLife::Spot const& spot, CityLife::CitizenActivity activity)
    {
        CityLife::CityPoiType const wanted = PoiForActivity(activity);
        return spot.PoiType == wanted ||
            (wanted == CityLife::CityPoiType::Tavern && spot.PoiType == CityLife::CityPoiType::Inn);
    }
}

namespace CityLife
{
    bool IsBotSession(Player const* player)
    {
        if (!player || !player->GetSession())
            return false;
        uint32 accountId = player->GetSession()->GetAccountId();
        return accountId && (sPlayerbotAIConfig.IsInRandomAccountList(accountId) ||
                             sRandomPlayerbotMgr.IsAddclassBot(player->GetGUID().GetCounter()) ||
                             sCityLifeMgr.IsConfiguredBotAccount(accountId));
    }

    bool IsHumanPlayer(Player* player)
    {
        // ::IsRealPlayer is "no PlayerbotAI attached", which is also true of a bot during login.
        return player && player->GetSession() && !IsBotSession(player) && ::IsRealPlayer(player);
    }

    Manager& Manager::Instance()
    {
        static Manager instance;
        return instance;
    }

    void Manager::LoadConfig()
    {
        CancelAdmissions();
        ++_dataGeneration;
        _enable = sConfigMgr->GetOption<bool>("CityLife.Enable", true);
        _debug = sConfigMgr->GetOption<bool>("CityLife.Debug", false);
        _startupDelaySeconds = sConfigMgr->GetOption<uint32>("CityLife.StartupDelaySeconds", 90);
        _tickSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.TickSeconds", 10), 5, 60);

        // Population Budget & Demand
        std::string budgetModeStr = Lower(sConfigMgr->GetOption<std::string>("CityLife.Population.BudgetMode", "percentage"));
        _budgetMode = (budgetModeStr == "count") ? BudgetMode::Count : BudgetMode::Percentage;
        _fixedCount = sConfigMgr->GetOption<uint32>("CityLife.Population.FixedCount", 100);
        _borrowPercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Population.BorrowPercent", 10), 1, 100);
        _maxTotalPopulation = sConfigMgr->GetOption<uint32>("CityLife.Population.MaxTotal", 120);
        if (_maxTotalPopulation > 2000)
            _maxTotalPopulation = 2000;

        std::string demandModeStr = Lower(sConfigMgr->GetOption<std::string>("CityLife.Population.DemandMode", "hybrid"));
        if (demandModeStr == "fixed")
            _demandMode = DemandMode::Fixed;
        else if (demandModeStr == "dynamic")
            _demandMode = DemandMode::Dynamic;
        else
            _demandMode = DemandMode::Hybrid;

        std::string distStr = Lower(sConfigMgr->GetOption<std::string>("CityLife.Population.Distribution", "scaled"));
        _distributionMode = (distStr == "legacy") ? DistributionMode::Legacy : DistributionMode::Scaled;
        _baselinePercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Population.BaselinePercent", 20), 0, 100);
        _burstGraceSeconds = sConfigMgr->GetOption<uint32>("CityLife.Population.BurstGraceSeconds", 180);
        _maxChangesPerTick = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Population.MaxChangesPerTick", 3), 1, 50);

        // Candidate Filters & Safety
        _usePlayerbotConfig = sConfigMgr->GetOption<bool>("CityLife.Bots.UsePlayerbotConfig", true);
        _botAccountPrefix = sConfigMgr->GetOption<std::string>("CityLife.Bots.AccountPrefix", "auto");
        _botAccountMin = sConfigMgr->GetOption<uint32>("CityLife.Bots.AccountMin", 0);
        _botAccountMax = sConfigMgr->GetOption<uint32>("CityLife.Bots.AccountMax", 0);
        uint32 rawQueryLimit = sConfigMgr->GetOption<uint32>("CityLife.Bots.QueryLimit", 500);
        _botQueryLimit = (rawQueryLimit == 0) ? 0 : std::max<uint32>(50, rawQueryLimit);
        _archetypes = sConfigMgr->GetOption<std::string>("CityLife.Bots.Archetypes", "");
        _archetypeFilter = false;
        _archetypeMask = 0;
        if (_archetypes.find_first_not_of(" \t,;") != std::string::npos)
        {
#ifdef CITYLIFE_HAS_ARCHETYPES
            _archetypeFilter = true;
            std::vector<std::string> unknown;
            _archetypeMask = Hs_ArchetypeMaskForNames(_archetypes, unknown);
            for (std::string const& name : unknown)
                LOG_ERROR("module", "[CityLife] CityLife.Bots.Archetypes: unknown archetype '{}', ignored.", name);
            if (!_archetypeMask)
                LOG_ERROR("module", "[CityLife] CityLife.Bots.Archetypes = \"{}\" names no known archetype: no bot will be borrowed.",
                    _archetypes);
#else
            LOG_WARN("module", "[CityLife] CityLife.Bots.Archetypes is set but mod-hearthside-chat is not in this build: "
                "the archetype filter is ignored and every bot may be borrowed.");
#endif
        }
        _skipGroupedBots = sConfigMgr->GetOption<bool>("CityLife.Bots.SkipGrouped", true);
        _respectPlayerbotActivity = sConfigMgr->GetOption<bool>("CityLife.Bots.RespectPlayerbotActivity", false);
        _returnBots = sConfigMgr->GetOption<bool>("CityLife.Bots.ReturnWhenReleased", true);

        // Time Scaling
        _timeScaling = sConfigMgr->GetOption<bool>("CityLife.Time.Enable", true);
        _morningMultiplier = std::min<uint32>(200, sConfigMgr->GetOption<uint32>("CityLife.Time.MorningMultiplierPct", 60));
        _dayMultiplier = std::min<uint32>(200, sConfigMgr->GetOption<uint32>("CityLife.Time.DayMultiplierPct", 80));
        _eveningMultiplier = std::min<uint32>(200, sConfigMgr->GetOption<uint32>("CityLife.Time.EveningMultiplierPct", 100));
        _nightMultiplier = std::min<uint32>(200, sConfigMgr->GetOption<uint32>("CityLife.Time.NightMultiplierPct", 25));

        // Relocation & Movement
        _roaming = sConfigMgr->GetOption<bool>("CityLife.Behaviour.Relocate", true);
        _relocateMinSeconds = std::max<uint32>(30, sConfigMgr->GetOption<uint32>("CityLife.Behaviour.RelocateMinSeconds", 60));
        _relocateMaxSeconds = std::max(_relocateMinSeconds, sConfigMgr->GetOption<uint32>("CityLife.Behaviour.RelocateMaxSeconds", 180));
        _leashRadius = std::max<uint32>(10, sConfigMgr->GetOption<uint32>("CityLife.Behaviour.LeashRadius", 35));
        // Second values used as milliseconds are capped at a day so "* 1000" stays well inside 2^31.
        _emoteMinSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Behaviour.EmoteMinSeconds", 120), 30, DAY);
        _emoteMaxSeconds = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Behaviour.EmoteMaxSeconds", 360), _emoteMinSeconds, DAY);
        _serviceSpotWeightMultiplier = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Behaviour.ServiceSpotWeightMultiplierPct", 100), 100, 1000);
        _staticServiceChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("CityLife.Behaviour.StaticServiceChancePct", 20));

        _staticServiceHubs.clear();
        std::string staticHubs = sConfigMgr->GetOption<std::string>("CityLife.Behaviour.StaticServiceHubs", "Stormwind,Orgrimmar");
        std::replace(staticHubs.begin(), staticHubs.end(), ',', ' ');
        std::replace(staticHubs.begin(), staticHubs.end(), ';', ' ');
        for (std::string const& hub : Tokenize(staticHubs))
            _staticServiceHubs.insert(Lower(hub));

        _staticServiceCategories.clear();
        std::string staticCategories = sConfigMgr->GetOption<std::string>("CityLife.Behaviour.StaticServiceCategories", "auction,bank");
        std::replace(staticCategories.begin(), staticCategories.end(), ',', ' ');
        std::replace(staticCategories.begin(), staticCategories.end(), ';', ' ');
        for (std::string const& category : Tokenize(staticCategories))
            _staticServiceCategories.insert(Lower(category));

        _nonCombatStrategies = sConfigMgr->GetOption<std::string>("CityLife.Strategies.NonCombat",
            "+stay,-follow,-passive,-grind,-rpg,-new rpg,-travel,-duel,-pvp");

        // Activity Switches
        _actLeaveInn = sConfigMgr->GetOption<bool>("CityLife.Activities.LeaveInn.Enable", true);
        _actReturnToInn = sConfigMgr->GetOption<bool>("CityLife.Activities.ReturnToInn.Enable", true);
        _actRoaming = sConfigMgr->GetOption<bool>("CityLife.Activities.Roaming.Enable", true);
        _actWallWalking = sConfigMgr->GetOption<bool>("CityLife.Activities.WallWalking.Enable", true);
        _actTavernSitting = sConfigMgr->GetOption<bool>("CityLife.Activities.TavernSitting.Enable", true);
        _actTavernDrinking = sConfigMgr->GetOption<bool>("CityLife.Activities.TavernDrinking.Enable", true);
        _actRandomEmotes = sConfigMgr->GetOption<bool>("CityLife.Activities.RandomEmotes.Enable", true);
        _antics = LifeMotion::LoadAnticConfig("CityLife.Antics");
        _actAuctionHouse = sConfigMgr->GetOption<bool>("CityLife.Activities.AuctionHouse.Enable", true);
        _actMailbox = sConfigMgr->GetOption<bool>("CityLife.Activities.Mailbox.Enable", true);
        _actTrainer = sConfigMgr->GetOption<bool>("CityLife.Activities.Trainer.Enable", true);
        _actFoodVendor = sConfigMgr->GetOption<bool>("CityLife.Activities.FoodVendor.Enable", true);
        _actDuelWatching = sConfigMgr->GetOption<bool>("CityLife.Activities.DuelWatching.Enable", true);
        _actFishing = sConfigMgr->GetOption<bool>("CityLife.Activities.Fishing.Enable", true);
        _actInnDancing = sConfigMgr->GetOption<bool>("CityLife.Activities.InnDancing.Enable", true);
        _actPlazaSocial = sConfigMgr->GetOption<bool>("CityLife.Activities.PlazaSocial.Enable", true);
        _actCrowdScenes = sConfigMgr->GetOption<bool>("CityLife.Activities.CrowdScenes.Enable", true);
        _actMountedTravel = sConfigMgr->GetOption<bool>("CityLife.Activities.MountedTravel.Enable", true);
        _activitiesAfkEnable = sConfigMgr->GetOption<bool>("CityLife.Activities.AFK.Enable", true);

        // Living Crowd Scenes
        _crowdEnable = sConfigMgr->GetOption<bool>("CityLife.Crowd.Enable", true);
        _crowdPlayerRadius = sConfigMgr->GetOption<float>("CityLife.Crowd.PlayerRadius", 100.0f);
        _crowdChatCooldownSec = std::min<uint32>(DAY, sConfigMgr->GetOption<uint32>("CityLife.Crowd.ChatCooldownSec", 90));
        _crowdAfkMinSec = std::min<uint32>(DAY, sConfigMgr->GetOption<uint32>("CityLife.Crowd.AfkMinSec", 60));
        _crowdAfkMaxSec = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("CityLife.Crowd.AfkMaxSec", 180), _crowdAfkMinSec, DAY);

        // Duel spectators (the duels themselves are mod-hearthside-pvp's)
        _spectatorWatchRadius = std::clamp<float>(sConfigMgr->GetOption<float>("CityLife.Spectators.WatchRadius", 40.0f), 10.0f, 100.0f);

        // Movement & Sink Protection
        _mountBetweenPoiChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("CityLife.Movement.MountBetweenPoiChance", 90));
        _sinkForensics = sConfigMgr->GetOption<bool>("CityLife.Movement.SinkForensics", true);

        // Chat & Interactions
        _chatScriptedConversations = sConfigMgr->GetOption<bool>("CityLife.Chat.ScriptedConversations", false);
        _chatGreetings = sConfigMgr->GetOption<bool>("CityLife.Chat.Greetings", false);
        _chatUnrestricted = sConfigMgr->GetOption<bool>("CityLife.Chat.Unrestricted", false);
        _chatRandomSayChance = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("CityLife.Chat.RandomSayChance", 0));
        _chatSayCooldownSec = std::min<uint32>(DAY, sConfigMgr->GetOption<uint32>("CityLife.Chat.SayCooldownSec", 30));
        _enableInteractions = sConfigMgr->GetOption<bool>("CityLife.Interactions.Enable", false);
        _enablePlayerTrading = sConfigMgr->GetOption<bool>("CityLife.Interactions.PlayerTrading", false);
        _enableBuyFood = sConfigMgr->GetOption<bool>("CityLife.Interactions.BuyFood", false);
        _blockAchievements = sConfigMgr->GetOption<bool>("CityLife.Achievements.BlockAchievements", false);

        // Advanced Micro-Behaviors (Ported from mod-city-bots)
        _alternatingDistricts = sConfigMgr->GetOption<bool>("CityLife.AlternatingDistricts.Enable", true);
        _waterAvoidance = sConfigMgr->GetOption<bool>("CityLife.Movement.WaterAvoidance.Enable", true);

        // Configurable Activity Weights (Ported from mod-city-bots)
        _weightWanderDistrict = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.WanderDistrict", 30));
        _weightSitInTavern = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.SitInTavern", 20));
        _weightRandomEmote = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.RandomEmote", 15));
        _weightVisitAuctionHouse = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.VisitAuctionHouse", 10));
        _weightVisitMailbox = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.VisitMailbox", 10));
        _weightBuyFood = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.BuyFood", 10));
        _weightVisitTrainer = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.VisitTrainer", 8));
        _weightDrinkInTavern = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.DrinkInTavern", 10));
        _weightWalkCityWalls = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.WalkCityWalls", 10));
        _weightFishAtDocks = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.FishAtDocks", 5));
        _weightReturnToInn = std::min<uint32>(1000, sConfigMgr->GetOption<uint32>("CityLife.Weight.ReturnToInn", 15));

        ImportPlayerbotDefaults();
        CheckCoexistence();

        RefreshAccountCache(); // keeps the previous cache until the new result arrives
        // Only the first load waits out the startup delay: a later .reload config must not freeze
        // the residents that are already running.
        if (!_configLoaded)
        {
            _startupElapsedMs = 0;
            _timerMs = 0;
        }
        _configLoaded = true;

        bool tablesOk = VerifyDatabase();
        _databaseReady = tablesOk && _enable && LoadData();

        // Final enabled state first, then drain whatever is still borrowed if the module is not running.
        if (!(_enable && _databaseReady) && !_residents.empty())
            ReleaseAll(true);

        if (_enable && tablesOk)
            PlayerbotsLife::RegisterActiveModule(PlayerbotsLife::ModuleSystemFlags::MODULE_CITY_LIFE);
        else
            PlayerbotsLife::UnregisterActiveModule(PlayerbotsLife::ModuleSystemFlags::MODULE_CITY_LIFE);

        LOG_INFO("module", "[CityLife] Initialized: enable={} DB={} hubs={} residents={} budgetMode={} demandMode={}",
            _enable ? 1 : 0, !tablesOk ? "missing" : (_databaseReady ? "ready" : "no hubs"), _hubs.size(), _residents.size(),
            budgetModeStr, demandModeStr);
    }

    void Manager::ImportPlayerbotDefaults()
    {
        if (!_usePlayerbotConfig)
            return;

        std::string configuredPrefix = sConfigMgr->GetOption<std::string>(
            "Playerbots.RandomBotAccountPrefix",
            sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot", false),
            false);
        uint32 loginDelay = sConfigMgr->GetOption<uint32>(
            "Playerbots.DisabledWithoutRealPlayerLoginDelay",
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DisabledWithoutRealPlayerLoginDelay", 30, false),
            false);
        std::string prefix = Lower(_botAccountPrefix);
        if (prefix.empty() || prefix == "auto" || prefix == "playerbot" || prefix == "playerbots")
            _botAccountPrefix = configuredPrefix;
        _startupDelaySeconds = std::max(_startupDelaySeconds, loginDelay);
    }

    bool Manager::CheckCoexistence()
    {
        bool cbActive = sConfigMgr->GetOption<bool>("CitizenBots.Enable", false, false);
        if (cbActive)
        {
            LOG_WARN("module", "[CityLife] CONFLICT: mod-city-bots appears enabled (CitizenBots.Enable=1). "
                "Disable mod-city-bots to prevent duplicate pathing commands and bot conflicts.");
        }
        return !cbActive;
    }

    bool Manager::VerifyDatabase()
    {
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'city_life_hub'"))
        {
            LOG_ERROR("module", "[CityLife] Missing WORLD table city_life_hub. "
                "AzerothCore's updater applies data/sql/db-world/base/city_life.sql: check that "
                "Updates.EnableDatabases includes the world database, then restart.");
            return false;
        }
        if (!WorldDatabase.Query("SHOW TABLES LIKE 'city_life_spot'"))
        {
            LOG_ERROR("module", "[CityLife] Missing WORLD table city_life_spot. "
                "AzerothCore's updater applies data/sql/db-world/base/city_life.sql: check that "
                "Updates.EnableDatabases includes the world database, then restart.");
            return false;
        }
        return true;
    }

    bool Manager::LoadData()
    {
        CancelAdmissions();
        std::vector<Hub> hubs;

        bool hasZoneId = WorldDatabase.Query("SHOW COLUMNS FROM `city_life_hub` LIKE 'zone_id'") != nullptr;
        std::string hubQuery = hasZoneId
            ? "SELECT id,name,enabled,team,min_level,max_level,map_id,default_population,priority,zone_id FROM city_life_hub ORDER BY priority DESC,id ASC"
            : "SELECT id,name,enabled,team,min_level,max_level,map_id,default_population,priority FROM city_life_hub ORDER BY priority DESC,id ASC";

        QueryResult hubResult = WorldDatabase.Query(hubQuery);
        if (hubResult)
        {
            do
            {
                Field* fields = hubResult->Fetch();
                Hub hub;
                hub.Id = fields[0].Get<uint32>();
                hub.Name = fields[1].Get<std::string>();
                hub.Enabled = fields[2].Get<uint8>() != 0;
                hub.Team = static_cast<TeamSide>(fields[3].Get<uint8>());
                hub.MinLevel = fields[4].Get<uint8>();
                hub.MaxLevel = fields[5].Get<uint8>();
                hub.MapId = fields[6].Get<uint32>();
                hub.DefaultPopulation = fields[7].Get<uint32>();
                hub.Priority = fields[8].Get<uint32>();

                if (hasZoneId)
                    hub.ZoneId = fields[9].Get<uint32>();
                else
                    hub.ZoneId = 0;

                // Known Zone mapping fallback if zone_id was not explicitly configured
                std::string lowerName = Lower(hub.Name);
                if (hub.ZoneId == 0)
                {
                    if (lowerName == "stormwind" || lowerName == "stormwindgate")
                        hub.ZoneId = 1519;
                    else if (lowerName == "orgrimmar" || lowerName == "orgrimmargate")
                        hub.ZoneId = 1637;
                    else if (lowerName == "ironforge")
                        hub.ZoneId = 1537;
                    else if (lowerName == "undercity")
                        hub.ZoneId = 1497;
                    else if (lowerName == "darnassus")
                        hub.ZoneId = 1657;
                    else if (lowerName == "thunderbluff")
                        hub.ZoneId = 1638;
                    else if (lowerName == "exodar")
                        hub.ZoneId = 3557;
                    else if (lowerName == "silvermoon")
                        hub.ZoneId = 3487;
                    else if (lowerName == "shattrath")
                        hub.ZoneId = 3703;
                    else if (lowerName == "dalaran")
                        hub.ZoneId = 4395;
                    else if (lowerName.find("goldshire") != std::string::npos)
                        hub.ZoneId = 12;
                }

                // Gate and Goldshire duel grounds: mod-hearthside-pvp runs the duels, residents here watch.
                hub.DuelHub = (lowerName.find("gate") != std::string::npos || lowerName.find("duel") != std::string::npos);
                hub.SocialHub = (lowerName == "goldshire");

                std::string configPrefix = "CityLife.Hub." + hub.Name;
                bool isWintergrasp = (lowerName.find("wintergrasp") != std::string::npos);
                uint32 defaultPop = isWintergrasp ? 0 : hub.DefaultPopulation;
                bool defaultEnable = isWintergrasp ? false : true;

                hub.ConfiguredPopulation = sConfigMgr->GetOption<uint32>(
                    configPrefix + ".Population", defaultPop, false);
                bool configEnabled = sConfigMgr->GetOption<bool>(
                    configPrefix + ".Enable", defaultEnable, false);
                hub.Enabled = hub.Enabled && configEnabled;
                hub.UseTimeScaling = sConfigMgr->GetOption<bool>(configPrefix + ".TimeScaling", !isWintergrasp, false);
                hub.ResidenceSeconds = sConfigMgr->GetOption<uint32>(
                    configPrefix + ".ResidenceSeconds", 0, false);
                hub.MinCount = sConfigMgr->GetOption<uint32>(
                    configPrefix + ".Min", 0, false);

                hubs.push_back(hub);
            } while (hubResult->NextRow());
        }

        // Check if extended columns exist in city_life_spot
        bool hasPoiType = WorldDatabase.Query("SHOW COLUMNS FROM `city_life_spot` LIKE 'poi_type'") != nullptr;
        bool hasDistrict = WorldDatabase.Query("SHOW COLUMNS FROM `city_life_spot` LIKE 'district'") != nullptr;
        bool hasPartner = WorldDatabase.Query("SHOW COLUMNS FROM `city_life_spot` LIKE 'partner_spot_id'") != nullptr;
        bool hasExtendedColumns = hasPoiType && hasDistrict && hasPartner;

        std::string spotQuery = hasExtendedColumns
            ? "SELECT id,hub_id,name,category,x,y,z,o,radius,weight,poi_type,district,partner_spot_id FROM city_life_spot WHERE enabled=1 ORDER BY hub_id,id"
            : "SELECT id,hub_id,name,category,x,y,z,o,radius,weight FROM city_life_spot WHERE enabled=1 ORDER BY hub_id,id";

        QueryResult spotResult = WorldDatabase.Query(spotQuery);
        if (spotResult)
        {
            do
            {
                Field* fields = spotResult->Fetch();
                Spot spot;
                spot.Id = fields[0].Get<uint32>();
                spot.HubId = fields[1].Get<uint32>();
                spot.Name = fields[2].Get<std::string>();
                spot.Category = fields[3].Get<std::string>();
                spot.X = fields[4].Get<float>();
                spot.Y = fields[5].Get<float>();
                spot.Z = fields[6].Get<float>();
                spot.O = fields[7].Get<float>();
                spot.Radius = std::max(0.0f, fields[8].Get<float>());
                spot.Weight = std::max<uint32>(1, fields[9].Get<uint32>());

                if (hasExtendedColumns)
                {
                    spot.PoiType = static_cast<CityPoiType>(fields[10].Get<uint8>());
                    spot.District = fields[11].Get<std::string>();
                    spot.PartnerSpotId = fields[12].Get<uint32>();
                }
                else
                {
                    spot.PoiType = CategoryToPoiType(spot.Category);
                    spot.District = spot.Category;
                    spot.PartnerSpotId = 0;
                }

                auto hub = std::find_if(hubs.begin(), hubs.end(),
                    [&](Hub const& candidate) { return candidate.Id == spot.HubId; });
                if (hub != hubs.end())
                    hub->Spots.push_back(spot);
            } while (spotResult->NextRow());
        }

        {
            std::lock_guard<std::mutex> lock(_hubRealPlayerMutex);
            _zoneToHubIds.clear();
            for (Hub const& h : hubs)
            {
                if (h.ZoneId != 0)
                    _zoneToHubIds[h.ZoneId].push_back(h.Id);
            }
        }

        _hubs.swap(hubs);

        // Release residents belonging to hubs that were disabled or deleted during reload
        for (auto resident = _residents.begin(); resident != _residents.end();)
        {
            Hub* hub = FindHub(resident->HubId);
            if (!hub || !hub->Enabled)
            {
                ReleaseResident(*resident, _returnBots);
                resident = _residents.erase(resident);
            }
            else
                ++resident;
        }

        // Recompute per-hub resident counts
        _hubResidentCounts.clear();
        for (Resident const& r : _residents)
            ++_hubResidentCounts[r.HubId];

        LOG_INFO("module", "[CityLife] Loaded {} hubs. {} active residents.", _hubs.size(), _residents.size());
        return !_hubs.empty();
    }

    TeamSide Manager::TeamForRace(uint8 race) const
    {
        switch (race)
        {
            case RACE_HUMAN:
            case RACE_DWARF:
            case RACE_NIGHTELF:
            case RACE_GNOME:
            case RACE_DRAENEI:
                return TeamSide::Alliance;
            case RACE_ORC:
            case RACE_UNDEAD_PLAYER:
            case RACE_TAUREN:
            case RACE_TROLL:
            case RACE_BLOODELF:
                return TeamSide::Horde;
            default:
                return TeamSide::Any;
        }
    }

    Player* Manager::FindPlayer(uint32 guidLow) const
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* player = ObjectAccessor::FindConnectedPlayer(guid))
            return player;
        return ObjectAccessor::FindPlayer(guid);
    }

    void Manager::RefreshAccountCache()
    {
        std::string prefix = Lower(_botAccountPrefix);
        LoginDatabase.EscapeString(prefix);
        std::string sql = Acore::StringFormat(
            "SELECT id,username FROM account WHERE ({} > 0 AND id BETWEEN {} AND {}) OR "
            "({} > 0 AND LEFT(LOWER(username),{})='{}')",
            _botAccountMax, _botAccountMin, _botAccountMax, _botAccountPrefix.size(), _botAccountPrefix.size(), prefix);
        uint64 generation = _dataGeneration;
        _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(sql).WithCallback([this, generation](QueryResult result)
        {
            if (generation != _dataGeneration)
                return;
            // Built aside and swapped in, so lookups keep the previous answers until now.
            std::unordered_map<uint32, bool> fresh;
            if (result)
            {
                do
                {
                    uint32 id = result->Fetch()[0].Get<uint32>();
                    bool range = _botAccountMax > 0 && id >= _botAccountMin && id <= _botAccountMax;
                    bool prefixMatch = !_botAccountPrefix.empty() &&
                        Lower(result->Fetch()[1].Get<std::string>()).rfind(Lower(_botAccountPrefix), 0) == 0;
                    fresh[id] = range || prefixMatch;
                } while (result->NextRow());
            }
            std::lock_guard<std::mutex> lock(_botAccountMutex);
            _botAccountCache.swap(fresh);
        }));
    }

    bool Manager::IsConfiguredBotAccount(uint32 accountId) const
    {
        {
            std::lock_guard<std::mutex> lock(_botAccountMutex);
            auto cached = _botAccountCache.find(accountId);
            if (cached != _botAccountCache.end())
                return cached->second;
        }
        bool range = accountId && _botAccountMax > 0 && _botAccountMax >= _botAccountMin &&
            accountId >= _botAccountMin && accountId <= _botAccountMax;
        return range;
    }

    bool Manager::IsHumanControlled(Player* player) const
    {
        if (!player || !player->GetSession())
            return true;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ::IsRealPlayer(player) || ::IsSelfBot(player) || ai->HasGameClientMaster())
            return true;
        if (Group* group = player->GetGroup())
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && (IsHumanPlayer(member) || ::IsSelfBot(member)))
                    return true;
        return false;
    }

    bool Manager::IsCandidate(Player* player)
    {
        if (!player || !player->GetSession() || !player->IsInWorld() || player->IsBeingTeleported())
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(player) &&
            !IsConfiguredBotAccount(player->GetSession()->GetAccountId()))
            return false;
        if (PlayerbotsLife::IsReserved(player->GetGUID().GetCounter()))
            return false;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (IsHumanControlled(player))
            return false;
        if (_respectPlayerbotActivity && !ai->AllowActivity(ALL_ACTIVITY))
            return false;
        if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->duel)
            return false;
        if (player->InBattleground() || player->InBattlegroundQueue())
            return false;
        if (player->GetMap() && player->GetMap()->Instanceable())
            return false;
        if (_skipGroupedBots && player->GetGroup())
            return false;
        return !IsResident(player->GetGUID().GetCounter());
    }

    bool Manager::IsResidentUsable(Player* player) const
    {
        if (!player || !player->GetSession() || !player->IsInWorld())
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(player) &&
            !IsConfiguredBotAccount(player->GetSession()->GetAccountId()))
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || ::IsRealPlayer(player) || ::IsSelfBot(player) || ai->HasGameClientMaster())
            return false;
        if (!player->IsAlive() || player->IsInFlight() || player->duel)
            return false;
        if (player->InBattleground() || player->InBattlegroundQueue())
            return false;
        if (player->GetMap() && player->GetMap()->Instanceable())
            return false;
        if (Group* group = player->GetGroup())
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource(); member && (IsHumanPlayer(member) || ::IsSelfBot(member)))
                    return false;
        if (_skipGroupedBots && player->GetGroup())
            return false;
        return true;
    }

    uint32 Manager::GetLiveWorldBotCount(bool forceRefresh) const
    {
        uint32 nowMs = GameTimeMS();
        if (!forceRefresh && _lastCensusTimeMs != 0 && (nowMs - _lastCensusTimeMs < 5000))
            return _cachedLiveBotCount;

        uint32 count = 0;
        std::vector<uint32> accountIdsToCheck;
        std::unordered_set<uint32> activeRealPlayerZones;

        {
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            accountIdsToCheck.reserve(players.size());
            for (auto const& pair : players)
            {
                Player* p = pair.second;
                if (!p || !p->GetSession() || !p->IsInWorld())
                    continue;

                if (IsHumanPlayer(p))
                {
                    if (!p->IsGameMaster())
                        activeRealPlayerZones.insert(p->GetZoneId());
                    continue;
                }

                if (IsHumanControlled(p) || !p->IsAlive() || p->InBattleground() || p->InBattlegroundQueue() ||
                    (p->GetMap() && p->GetMap()->Instanceable()))
                    continue;

                if (sRandomPlayerbotMgr.IsRandomBot(p))
                    ++count;
                else
                    accountIdsToCheck.push_back(p->GetSession()->GetAccountId());
            }
        }

        if (!activeRealPlayerZones.empty())
        {
            uint32 now = GameTimeSeconds();
            std::lock_guard<std::mutex> lock(_hubRealPlayerMutex);
            for (uint32 zoneId : activeRealPlayerZones)
            {
                auto it = _zoneToHubIds.find(zoneId);
                if (it != _zoneToHubIds.end())
                {
                    for (uint32 hubId : it->second)
                        _hubLastRealPlayerTime[hubId] = now;
                }
            }
        }

        for (uint32 accountId : accountIdsToCheck)
        {
            if (IsConfiguredBotAccount(accountId))
                ++count;
        }

        _cachedLiveBotCount = count;
        _lastCensusTimeMs = nowMs;
        return count;
    }

    bool Manager::IsResident(uint32 guidLow) const
    {
        return std::any_of(_residents.begin(), _residents.end(),
            [&](Resident const& resident) { return resident.Bot.GuidLow == guidLow; });
    }

    void Manager::DisengageResident(uint32 guidLow)
    {
        auto it = std::find_if(_residents.begin(), _residents.end(),
            [&](Resident const& r) { return r.Bot.GuidLow == guidLow; });

        if (it != _residents.end())
        {
            auto countIt = _hubResidentCounts.find(it->HubId);
            if (countIt != _hubResidentCounts.end() && countIt->second > 0)
                --countIt->second;

            ReleaseResident(*it, false); // Do not return-teleport away from group or master
            _residents.erase(it);

            if (_debug)
                LOG_INFO("module", "[CityLife] Disengaged resident {} on invite or control takeover.", guidLow);
        }
    }

    bool Manager::FitsArchetype(uint32 guidLow)
    {
#ifdef CITYLIFE_HAS_ARCHETYPES
        if (!_archetypeFilter)
            return true;
        uint64 raw = ObjectGuid::Create<HighGuid::Player>(guidLow).GetRawValue();
        uint32 archetype = static_cast<uint32>(Hs_ArchetypeForBot(raw));
        return archetype < 32 && (_archetypeMask & (1u << archetype)) != 0;
#else
        (void)guidLow;
        return true;
#endif
    }

    std::vector<BotCandidate> Manager::LoadCandidates(Hub const& hub, uint32 poolCount)
    {
        std::vector<BotCandidate> candidates;
        uint32 nowMs = GameTimeMS();
        if (_cachedOnlineGuids.empty() || (nowMs - _lastCandidateScanMs > 5000))
        {
            _cachedOnlineGuids.clear();
            std::shared_lock<std::shared_mutex> playerLock(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            _cachedOnlineGuids.reserve(players.size());
            for (auto const& pair : players)
                if (pair.second)
                    _cachedOnlineGuids.push_back(pair.second->GetGUID().GetCounter());
            _lastCandidateScanMs = nowMs;
        }

        if (_cachedOnlineGuids.empty())
            return candidates;

        // Smart Hybrid Dynamic QueryLimit:
        // 1. Calculate dynamic census ceiling
        uint32 dynamicCeiling = _botQueryLimit;
        if (dynamicCeiling == 0)
        {
            uint32 liveBots = (poolCount > 0) ? poolCount : GetLiveWorldBotCount();
            dynamicCeiling = std::clamp<uint32>(liveBots / 2, 100, 1500);
        }

        // 2. Demand-driven early exit target
        uint32 currentCount = CountResidents(hub.Id) + static_cast<uint32>(std::count_if(
            _pendingAdmissions.begin(), _pendingAdmissions.end(),
            [&](PendingAdmission const& pending) { return pending.HubId == hub.Id; }));
        auto target = _hubTargets.find(hub.Id);
        uint32 targetPop = target == _hubTargets.end() ? TargetPopulation(hub) : target->second;
        uint32 deficit = (targetPop > currentCount) ? (targetPop - currentCount) : 1;
        uint32 candidateTarget = std::min<uint32>(dynamicCeiling, std::max<uint32>(20, deficit * 5));

        // 3. Rotational sampling across online bots: prioritize local zone and map bots
        std::vector<BotCandidate> localZoneCandidates;
        std::vector<BotCandidate> localMapCandidates;
        std::vector<BotCandidate> foreignCandidates;

        size_t n = _cachedOnlineGuids.size();
        size_t offset = urand(0, static_cast<uint32>(n - 1));

        for (size_t i = 0; i < n; ++i)
        {
            uint32 guidLow = _cachedOnlineGuids[(offset + i) % n];
            Player* player = FindPlayer(guidLow);
            if (!player || player->GetLevel() < hub.MinLevel || player->GetLevel() > hub.MaxLevel)
                continue;
            if (!IsCandidate(player) || !FitsArchetype(guidLow))
                continue;
            if (player->GetZoneId() == AREA_WINTERGRASP)
                continue;
            // Already asked for by another hub this pass: the registry would grant only one of them.
            if (std::any_of(_pendingAdmissions.begin(), _pendingAdmissions.end(),
                [guidLow](PendingAdmission const& pending) { return pending.Bot.GuidLow == guidLow; }))
                continue;

            BotCandidate candidate;
            candidate.GuidLow = guidLow;
            candidate.AccountId = player->GetSession()->GetAccountId();
            candidate.Name = player->GetName();
            candidate.Level = player->GetLevel();
            candidate.Race = player->getRace();
            candidate.Team = TeamForRace(candidate.Race);
            if (hub.Team != TeamSide::Any && candidate.Team != hub.Team)
                continue;

            if (player->GetMapId() == hub.MapId && hub.ZoneId != 0 && player->GetZoneId() == hub.ZoneId)
                localZoneCandidates.push_back(candidate);
            else if (player->GetMapId() == hub.MapId)
                localMapCandidates.push_back(candidate);
            else
                foreignCandidates.push_back(candidate);

            if (localZoneCandidates.size() >= candidateTarget)
                break;
        }

        candidates = std::move(localZoneCandidates);
        if (candidates.size() < candidateTarget)
        {
            for (auto const& c : localMapCandidates)
            {
                candidates.push_back(c);
                if (candidates.size() >= candidateTarget)
                    break;
            }
        }
        if (candidates.size() < candidateTarget)
        {
            for (auto const& c : foreignCandidates)
            {
                candidates.push_back(c);
                if (candidates.size() >= candidateTarget)
                    break;
            }
        }

        for (size_t i = candidates.size(); i > 1; --i)
            std::swap(candidates[i - 1], candidates[urand(0, static_cast<uint32>(i - 1))]);

        return candidates;
    }

    Hub* Manager::FindHub(uint32 hubId)
    {
        auto found = std::find_if(_hubs.begin(), _hubs.end(),
            [&](Hub const& hub) { return hub.Id == hubId; });
        return found == _hubs.end() ? nullptr : &*found;
    }

    Hub const* Manager::FindHub(uint32 hubId) const
    {
        auto found = std::find_if(_hubs.begin(), _hubs.end(),
            [&](Hub const& hub) { return hub.Id == hubId; });
        return found == _hubs.end() ? nullptr : &*found;
    }

    Hub* Manager::FindHub(std::string const& name)
    {
        std::string lowered = Lower(name);
        auto found = std::find_if(_hubs.begin(), _hubs.end(),
            [&](Hub const& hub) { return Lower(hub.Name) == lowered; });
        return found == _hubs.end() ? nullptr : &*found;
    }

    Hub const* Manager::FindHubByZone(uint32 zoneId) const
    {
        for (Hub const& hub : _hubs)
            if (hub.ZoneId != 0 && hub.ZoneId == zoneId)
                return &hub;
        return nullptr;
    }

    Spot const* Manager::FindSpot(uint32 hubId, uint32 spotId) const
    {
        for (Hub const& hub : _hubs)
        {
            if (hub.Id == hubId)
            {
                for (Spot const& spot : hub.Spots)
                    if (spot.Id == spotId)
                        return &spot;
            }
        }
        return nullptr;
    }

    Spot const* Manager::FindInnBedSpot(Hub const& hub) const
    {
        // A bed if the hub has one, else the inn itself.
        Spot const* inn = nullptr;
        for (Spot const& spot : hub.Spots)
        {
            if (spot.PoiType == CityPoiType::InnBed || spot.Category == "bed")
                return &spot;
            if (!inn && (spot.PoiType == CityPoiType::Inn || spot.Category == "inn"))
                inn = &spot;
        }
        return inn;
    }

    bool Manager::IsStaticServiceSpot(Hub const& hub, Spot const& spot) const
    {
        return _staticServiceHubs.find(Lower(hub.Name)) != _staticServiceHubs.end() &&
            _staticServiceCategories.find(Lower(spot.Category)) != _staticServiceCategories.end();
    }

    bool Manager::UsesAlternatingHubRoute(std::string const& hubName) const
    {
        std::string lower = Lower(hubName);
        return lower == "stormwind" ||
               lower == "orgrimmar" ||
               lower == "ironforge" ||
               lower == "undercity" ||
               lower == "darnassus" ||
               lower == "exodar" ||
               lower == "thunderbluff" ||
               lower == "silvermoon" ||
               lower == "shattrath" ||
               lower == "dalaran";
    }

    bool Manager::IsHubDistrict(std::string const& hubName, std::string const& district) const
    {
        std::string h = Lower(hubName);
        std::string d = Lower(district);

        if (h == "stormwind")
            return d == "trade" || d == "tradedistrict";
        if (h == "orgrimmar")
            return d == "valley" || d == "valleyofstrength" || d == "valleyinn" || d == "valleymarket";
        if (h == "ironforge")
            return d == "greatforge" || d == "commons";
        if (h == "undercity")
            return d == "magicquarter" || d == "apothecarium";
        if (h == "darnassus")
            return d == "craftsmen" || d == "tradesmensterrace";
        if (h == "exodar")
            return d == "seat" || d == "seatofnaaru";
        if (h == "thunderbluff")
            return d == "lowerrise" || d == "central";
        if (h == "silvermoon")
            return d == "bazaar" || d == "sunfuryspire" || d == "sunfury";
        if (h == "shattrath")
            return d == "terrace" || d == "centralterrace";
        if (h == "dalaran")
            return d == "maguscommerce" || d == "centralloop";

        return false;
    }

    bool Manager::DistrictMatchesClass(std::string const& hubName, std::string const& district, uint8 botClass)
    {
        std::string h = Lower(hubName);
        std::string d = Lower(district);

        if (h == "stormwind")
        {
            switch (botClass)
            {
                case CLASS_MAGE:
                case CLASS_WARLOCK:
                    return d.find("mage") != std::string::npos;
                case CLASS_ROGUE:
                case CLASS_WARRIOR:
                case CLASS_DEATH_KNIGHT:
                    return d.find("old") != std::string::npos;
                case CLASS_PALADIN:
                case CLASS_PRIEST:
                    return d.find("cathedral") != std::string::npos;
                case CLASS_HUNTER:
                case CLASS_SHAMAN:
                    return d.find("dwarf") != std::string::npos;
                case CLASS_DRUID:
                    return d.find("park") != std::string::npos || d.find("cathedral") != std::string::npos;
                default:
                    return true;
            }
        }
        else if (h == "orgrimmar")
        {
            switch (botClass)
            {
                case CLASS_WARRIOR:
                case CLASS_ROGUE:
                case CLASS_DEATH_KNIGHT:
                    return d.find("strength") != std::string::npos;
                case CLASS_HUNTER:
                case CLASS_SHAMAN:
                case CLASS_PRIEST:
                case CLASS_MAGE:
                    return d.find("spirit") != std::string::npos;
                case CLASS_DRUID:
                    return d.find("wisdom") != std::string::npos;
                case CLASS_WARLOCK:
                    return d.find("cleft") != std::string::npos || d.find("shadow") != std::string::npos;
                default:
                    return true;
            }
        }
        else if (h == "ironforge")
        {
            switch (botClass)
            {
                case CLASS_WARRIOR:
                case CLASS_PALADIN:
                case CLASS_HUNTER:
                case CLASS_ROGUE:
                case CLASS_DEATH_KNIGHT:
                    return d.find("military") != std::string::npos || d.find("commons") != std::string::npos;
                case CLASS_MAGE:
                case CLASS_PRIEST:
                case CLASS_WARLOCK:
                    return d.find("mystic") != std::string::npos;
                default:
                    return true;
            }
        }
        else if (h == "undercity")
        {
            switch (botClass)
            {
                case CLASS_WARRIOR:
                case CLASS_ROGUE:
                case CLASS_DEATH_KNIGHT:
                    return d.find("war") != std::string::npos || d.find("center") != std::string::npos;
                case CLASS_MAGE:
                case CLASS_WARLOCK:
                case CLASS_PRIEST:
                    return d.find("magic") != std::string::npos || d.find("apothecary") != std::string::npos;
                default:
                    return true;
            }
        }
        else if (h == "darnassus")
        {
            switch (botClass)
            {
                case CLASS_WARRIOR:
                case CLASS_ROGUE:
                    return d.find("warrior") != std::string::npos;
                case CLASS_PRIEST:
                case CLASS_PALADIN:
                    return d.find("temple") != std::string::npos;
                case CLASS_DRUID:
                case CLASS_HUNTER:
                    return d.find("cenarion") != std::string::npos;
                default:
                    return true;
            }
        }
        return true;
    }

    Spot const* Manager::ChooseSpot(Hub const& hub, uint32 ignoredSpotId, bool filterByDistrict, bool targetHubDistrict, Player* botForPathCheck) const
    {
        Spot const* best = nullptr;
        uint64 bestScore = std::numeric_limits<uint64>::max();
        bool hasAlternative = hub.Spots.size() > 1;

        Spot const* currentSpot = nullptr;
        if (ignoredSpotId != 0)
        {
            for (Spot const& s : hub.Spots)
            {
                if (s.Id == ignoredSpotId)
                {
                    currentSpot = &s;
                    break;
                }
            }
        }

        auto passesInnFilter = [&](Spot const& spot)
        {
            // Beds are for the end-of-residence sleep (FindInnBedSpot), not for roaming.
            if (spot.PoiType == CityPoiType::InnBed)
                return false;
            if (!currentSpot)
                return true;
            bool currentIsInInn = (currentSpot->Category == "inn" || currentSpot->PoiType == CityPoiType::Tavern);
            bool candIsInInn = (spot.Category == "inn" || spot.PoiType == CityPoiType::Tavern);
            if (currentIsInInn && !candIsInInn && !_actLeaveInn)
                return false;
            if (!currentIsInInn && candIsInInn && !_actReturnToInn)
                return false;
            return true;
        };

        std::vector<Spot const*> candidateList;
        if (filterByDistrict)
        {
            for (Spot const& spot : hub.Spots)
            {
                if (hasAlternative && spot.Id == ignoredSpotId)
                    continue;

                if (!passesInnFilter(spot))
                    continue;

                bool isHub = IsHubDistrict(hub.Name, spot.District);
                if (targetHubDistrict ? isHub : !isHub)
                    candidateList.push_back(&spot);
            }
        }

        if (candidateList.empty())
        {
            for (Spot const& spot : hub.Spots)
            {
                if (hasAlternative && spot.Id == ignoredSpotId)
                    continue;

                if (!passesInnFilter(spot))
                    continue;

                candidateList.push_back(&spot);
            }
        }

        for (Spot const* spotPtr : candidateList)
        {
            Spot const& spot = *spotPtr;

            // Water avoidance sensitivity check
            if (_waterAvoidance && botForPathCheck && spot.PoiType != CityPoiType::Dock)
            {
                if (!Movement::HasCompleteWalkablePath(botForPathCheck, spot.X, spot.Y, spot.Z, false))
                    continue;
            }

            uint32 occupants = static_cast<uint32>(std::count_if(_residents.begin(), _residents.end(),
                [&](Resident const& resident)
                {
                    return resident.HubId == hub.Id && resident.SpotId == spot.Id;
                }));
            uint64 weight = spot.Weight;
            if (IsStaticServiceSpot(hub, spot))
                weight = weight * _serviceSpotWeightMultiplier / 100;
            uint64 score = (static_cast<uint64>(occupants) + 1) * 100000 /
                std::max<uint64>(1, weight) + urand(0, 100);
            if (score < bestScore)
            {
                bestScore = score;
                best = &spot;
            }
        }

        // Fallback if all candidates rejected by path check
        if (!best && !candidateList.empty())
        {
            for (Spot const* spotPtr : candidateList)
            {
                Spot const& spot = *spotPtr;
                uint32 occupants = static_cast<uint32>(std::count_if(_residents.begin(), _residents.end(),
                    [&](Resident const& resident)
                    {
                        return resident.HubId == hub.Id && resident.SpotId == spot.Id;
                    }));
                uint64 weight = spot.Weight;
                if (IsStaticServiceSpot(hub, spot))
                    weight = weight * _serviceSpotWeightMultiplier / 100;
                uint64 score = (static_cast<uint64>(occupants) + 1) * 100000 /
                    std::max<uint64>(1, weight) + urand(0, 100);
                if (score < bestScore)
                {
                    bestScore = score;
                    best = &spot;
                }
            }
        }

        return best;
    }

    CitizenRole Manager::DetermineRole(Hub const& hub, Spot const& spot, BotCandidate const& /*candidate*/) const
    {
        // Everyone on a duel ground watches; the duels there are mod-hearthside-pvp's.
        if ((hub.DuelHub || spot.PoiType == CityPoiType::DuelRing) && _actDuelWatching)
            return CitizenRole::Spectator;

        // Every dock resident fishes: with a pole and the skill it casts Fishing, otherwise it plays
        // the animation (ExecuteFishing), so nothing is learned, equipped or looted for it.
        if (spot.PoiType == CityPoiType::Dock && _actFishing)
            return CitizenRole::Fisher;

        if (Lower(hub.Name) == "goldshire" && (spot.District == "LionsPrideUp" || spot.Name.find("Dance") != std::string::npos || spot.Category == "inn") && _actInnDancing)
            return CitizenRole::InnDancer;

        if (spot.PoiType == CityPoiType::Plaza && _actPlazaSocial)
            return CitizenRole::PlazaTalker;

        if (spot.PoiType == CityPoiType::Bank && _actCrowdScenes && _crowdEnable)
            return CitizenRole::Crowd;

        return CitizenRole::Flex;
    }

    CitizenActivity Manager::ChooseInitialActivity(CitizenRole role, Spot const& spot) const
    {
        switch (role)
        {
            case CitizenRole::Fisher:
                return CitizenActivity::FishAtDocks;
            case CitizenRole::InnDancer:
                return CitizenActivity::InnDance;
            case CitizenRole::PlazaTalker:
                return CitizenActivity::PlazaSocial;
            case CitizenRole::Crowd:
                return CitizenActivity::CrowdSocial;
            case CitizenRole::Spectator:
                return CitizenActivity::WatchDuels;
            default:
                break;
        }

        switch (spot.PoiType)
        {
            case CityPoiType::Inn:
            case CityPoiType::Tavern:
                if (_actTavernSitting && _actTavernDrinking)
                    return roll_chance_i(50) ? CitizenActivity::DrinkInTavern : CitizenActivity::SitInTavern;
                if (_actTavernDrinking)
                    return CitizenActivity::DrinkInTavern;
                return _actTavernSitting ? CitizenActivity::SitInTavern : CitizenActivity::Idle;
            case CityPoiType::InnBed:
                return CitizenActivity::ReleaseCycle;
            case CityPoiType::AuctionHouse:
                return _actAuctionHouse ? CitizenActivity::VisitAuctionHouse : CitizenActivity::Idle;
            case CityPoiType::Mailbox:
                return _actMailbox ? CitizenActivity::VisitMailbox : CitizenActivity::Idle;
            case CityPoiType::Trainer:
                return _actTrainer ? CitizenActivity::VisitTrainer : CitizenActivity::Idle;
            case CityPoiType::Vendor:
                return _actFoodVendor ? CitizenActivity::BuyFood : CitizenActivity::Idle;
            case CityPoiType::WallPatrol:
                return _actWallWalking ? CitizenActivity::WalkCityWalls : CitizenActivity::Idle;
            case CityPoiType::Bank:
                if (_actCrowdScenes && _crowdEnable)
                    return CitizenActivity::CrowdSocial;
                return _actAuctionHouse ? CitizenActivity::VisitAuctionHouse : CitizenActivity::Idle;
            case CityPoiType::Plaza:
                return _actPlazaSocial ? CitizenActivity::PlazaSocial : CitizenActivity::Idle;
            case CityPoiType::Dock:
                return _actFishing ? CitizenActivity::FishAtDocks : (_actRoaming ? CitizenActivity::WanderDistrict : CitizenActivity::Idle);
            case CityPoiType::DuelRing:
                return _actDuelWatching ? CitizenActivity::WatchDuels : CitizenActivity::Idle;
            default:
                return _actRoaming ? CitizenActivity::WanderDistrict : CitizenActivity::RandomEmote;
        }
    }

    CitizenActivity Manager::PickNextActivity(Resident const& resident, Hub const& hub) const
    {
        // Spectators stay on the duel ground; fishers and dancers never relocate. Plaza talkers and
        // crowd members get a role from the spot they stand on and move on like anyone else.
        if (resident.Role == CitizenRole::Spectator)
            return CitizenActivity::WatchDuels;

        struct ActivityWeightOption
        {
            CitizenActivity Activity;
            uint32 Weight;
            bool Enabled;
            CityPoiType RequiredPoi;
        };

        ActivityWeightOption pool[] = {
            { CitizenActivity::WanderDistrict,     _weightWanderDistrict,     _actRoaming,        CityPoiType::Plaza },
            { CitizenActivity::SitInTavern,         _weightSitInTavern,         _actTavernSitting,  CityPoiType::Tavern },
            { CitizenActivity::DrinkInTavern,       _weightDrinkInTavern,       _actTavernDrinking, CityPoiType::Tavern },
            { CitizenActivity::RandomEmote,         _weightRandomEmote,         _actRandomEmotes,   CityPoiType::Plaza },
            { CitizenActivity::VisitAuctionHouse,   _weightVisitAuctionHouse,   _actAuctionHouse,   CityPoiType::AuctionHouse },
            { CitizenActivity::VisitMailbox,        _weightVisitMailbox,        _actMailbox,        CityPoiType::Mailbox },
            { CitizenActivity::BuyFood,             _weightBuyFood,             _actFoodVendor,     CityPoiType::Vendor },
            { CitizenActivity::VisitTrainer,        _weightVisitTrainer,        _actTrainer,        CityPoiType::Trainer },
            { CitizenActivity::WalkCityWalls,       _weightWalkCityWalls,       _actWallWalking,    CityPoiType::WallPatrol },
            { CitizenActivity::FishAtDocks,         _weightFishAtDocks,         _actFishing,        CityPoiType::Dock },
            { CitizenActivity::ReturnToInn,         _weightReturnToInn,         _actReturnToInn,    CityPoiType::Inn }
        };

        // Weights are capped at 1000 in LoadConfig, so the running total cannot overflow.
        uint32 totalWeight = 0;
        for (auto& item : pool)
        {
            if (!item.Enabled || item.Weight == 0)
            {
                item.Weight = 0;
                continue;
            }

            // Don't repeat the exact same activity immediately if other options exist
            if (item.Activity == resident.Activity)
            {
                item.Weight = 0;
                continue;
            }

            bool hasPoi = std::any_of(hub.Spots.begin(), hub.Spots.end(),
                [&](Spot const& s) {
                    return s.PoiType == item.RequiredPoi || (item.RequiredPoi == CityPoiType::Tavern && s.PoiType == CityPoiType::Inn);
                });

            if (!hasPoi && item.RequiredPoi != CityPoiType::Plaza)
            {
                item.Weight = 0;
                continue;
            }

            totalWeight += item.Weight;
        }

        if (totalWeight == 0)
            return CitizenActivity::WanderDistrict;

        uint32 roll = urand(1, totalWeight);
        for (auto const& item : pool)
        {
            if (roll <= item.Weight)
                return item.Activity;
            roll -= item.Weight;
        }
        return CitizenActivity::WanderDistrict;
    }

    Spot const* Manager::ChooseSpotForActivity(Hub const& hub, CitizenActivity activity, uint32 ignoredSpotId, Player* botForPathCheck) const
    {
        CityPoiType const targetType = PoiForActivity(activity);

        std::vector<Spot const*> matchingSpots;
        for (Spot const& spot : hub.Spots)
        {
            if (spot.Id == ignoredSpotId)
                continue;

            if (SpotFitsActivity(spot, activity))
                matchingSpots.push_back(&spot);
        }

        if (targetType == CityPoiType::Trainer && botForPathCheck)
        {
            std::vector<Spot const*> classSpots;
            uint8 bClass = botForPathCheck->getClass();
            for (Spot const* sp : matchingSpots)
            {
                if (DistrictMatchesClass(hub.Name, sp->District, bClass) ||
                    DistrictMatchesClass(hub.Name, sp->Name, bClass))
                {
                    classSpots.push_back(sp);
                }
            }
            if (!classSpots.empty())
                matchingSpots = std::move(classSpots);
        }

        if (matchingSpots.empty() && targetType == CityPoiType::Trainer)
        {
            for (Spot const& spot : hub.Spots)
            {
                if (spot.PoiType == CityPoiType::Trainer)
                    matchingSpots.push_back(&spot);
            }
        }

        if (matchingSpots.empty())
            return ChooseSpot(hub, ignoredSpotId, false, false, botForPathCheck);

        Spot const* best = nullptr;
        uint64 bestScore = std::numeric_limits<uint64>::max();

        for (Spot const* spotPtr : matchingSpots)
        {
            Spot const& spot = *spotPtr;
            if (_waterAvoidance && botForPathCheck && spot.PoiType != CityPoiType::Dock)
            {
                if (!Movement::HasCompleteWalkablePath(botForPathCheck, spot.X, spot.Y, spot.Z, false))
                    continue;
            }

            uint32 occupants = static_cast<uint32>(std::count_if(_residents.begin(), _residents.end(),
                [&](Resident const& r) { return r.HubId == hub.Id && r.SpotId == spot.Id; }));

            uint64 score = (static_cast<uint64>(occupants) + 1) * 10000 / std::max<uint32>(1, spot.Weight) + urand(0, 50);
            if (score < bestScore)
            {
                bestScore = score;
                best = &spot;
            }
        }

        if (!best && !matchingSpots.empty())
            best = matchingSpots[urand(0, static_cast<uint32>(matchingSpots.size()) - 1)];

        return best ? best : ChooseSpot(hub, ignoredSpotId, false, false, botForPathCheck);
    }

    uint32 Manager::CountResidents(uint32 hubId) const
    {
        auto it = _hubResidentCounts.find(hubId);
        return it != _hubResidentCounts.end() ? it->second : 0;
    }

    uint32 Manager::CurrentTimeMultiplier() const
    {
        if (!_timeScaling)
            return 100;

        // Server wall-clock hour, through the core's thread-safe breakdown (not std::localtime).
        std::tm localTime = Acore::Time::TimeBreakdown(std::time(nullptr));
        if (localTime.tm_hour >= 6 && localTime.tm_hour < 12)
            return _morningMultiplier;
        if (localTime.tm_hour >= 12 && localTime.tm_hour < 18)
            return _dayMultiplier;
        if (localTime.tm_hour >= 18)
            return _eveningMultiplier;
        return _nightMultiplier;
    }

    uint32 Manager::CalculateTotalBudget(uint32 eligiblePoolCount) const
    {
        uint32 requested = (_budgetMode == BudgetMode::Count)
            ? _fixedCount
            : static_cast<uint32>(std::floor(eligiblePoolCount * _borrowPercent / 100.0f));

        if (_maxTotalPopulation > 0)
            return std::min({ requested, _maxTotalPopulation, eligiblePoolCount });
        return std::min(requested, eligiblePoolCount);
    }

    bool Manager::IsWintergraspWarHub(Hub const& hub) const
    {
        if (hub.ZoneId != AREA_WINTERGRASP)
            return false;
        Battlefield* wintergrasp = sBattlefieldMgr->GetBattlefieldByBattleId(BATTLEFIELD_BATTLEID_WG);
        return wintergrasp && wintergrasp->IsWarTime();
    }

    uint32 Manager::TargetPopulation(Hub const& hub) const
    {
        if (!hub.Enabled || hub.Spots.empty() || IsWintergraspWarHub(hub))
            return 0;

        uint32 nominal = hub.ConfiguredPopulation;
        if (_timeScaling && hub.UseTimeScaling)
        {
            nominal = static_cast<uint32>(std::lround(
                static_cast<double>(nominal) * CurrentTimeMultiplier() / 100.0));
        }

        if (_demandMode == DemandMode::Fixed)
            return nominal;

        // Dynamic or Hybrid demand evaluation
        uint32 now = GameTimeSeconds();
        bool playerPresent = false;

        {
            std::lock_guard<std::mutex> lock(_hubRealPlayerMutex);
            auto lastSeenIt = _hubLastRealPlayerTime.find(hub.Id);
            if (lastSeenIt != _hubLastRealPlayerTime.end())
            {
                if (now >= lastSeenIt->second && (now - lastSeenIt->second) <= _burstGraceSeconds)
                    playerPresent = true;
            }
        }

        if (playerPresent)
            return nominal;

        // When unoccupied:
        if (_demandMode == DemandMode::Dynamic)
            return hub.MinCount;

        // Hybrid mode baseline
        uint32 baseline = static_cast<uint32>(std::lround(nominal * _baselinePercent / 100.0));
        return std::max(baseline, hub.MinCount);
    }

    void Manager::OnPlayerZoneUpdate(Player* player, uint32 zoneId)
    {
        if (!IsHumanPlayer(player) || player->IsGameMaster())
            return;

        std::lock_guard<std::mutex> lock(_hubRealPlayerMutex);
        auto it = _zoneToHubIds.find(zoneId);
        if (it != _zoneToHubIds.end())
        {
            uint32 now = GameTimeSeconds();
            for (uint32 hubId : it->second)
                _hubLastRealPlayerTime[hubId] = now;
        }
    }

    void Manager::OnPlayerLogin(Player* player)
    {
        if (!IsHumanPlayer(player) || player->IsGameMaster())
            return;

        std::lock_guard<std::mutex> lock(_hubRealPlayerMutex);
        auto it = _zoneToHubIds.find(player->GetZoneId());
        if (it != _zoneToHubIds.end())
        {
            uint32 now = GameTimeSeconds();
            for (uint32 hubId : it->second)
                _hubLastRealPlayerTime[hubId] = now;
        }
    }

    void Manager::OnPlayerLogout(Player* player)
    {
        // mod-playerbots rebuilds a bot's strategies and positions at its next login
        // (PlayerbotHolder::OnBotLogin: ResetStrategies + repository load), so nothing to restore here.
        if (player && player->GetSession() && !IsHumanPlayer(player))
        {
            uint32 guid = player->GetGUID().GetCounter();
            if (IsResident(guid))
                QueueDisengage(guid);
        }
    }

    void Manager::QueueDisengage(uint32 guidLow)
    {
        std::lock_guard<std::mutex> lock(_queuedDisengageMutex);
        _queuedDisengages.push_back(guidLow);
    }

    void Manager::ProcessQueuedDisengages()
    {
        std::vector<uint32> queued;
        {
            std::lock_guard<std::mutex> lock(_queuedDisengageMutex);
            queued.swap(_queuedDisengages);
        }
        for (uint32 guidLow : queued)
            DisengageResident(guidLow);
    }

    Player* Manager::FindCrowdChatPartner(uint32 hubId, Player* bot)
    {
        if (!bot)
            return nullptr;

        for (Resident const& r : _residents)
        {
            if (r.HubId != hubId || r.Bot.GuidLow == bot->GetGUID().GetCounter())
                continue;

            if (r.Role == CitizenRole::Crowd && r.CrowdPhaseState == CrowdPhase::Linger &&
                r.ChatPartnerGuid == 0 && r.Activity == CitizenActivity::CrowdSocial)
            {
                Player* p = FindPlayer(r.Bot.GuidLow);
                if (p && p->IsInWorld() && bot->GetDistance(p) <= 15.0f)
                    return p;
            }
        }
        return nullptr;
    }

    void Manager::BeginCrowdChat(Player* bot, Player* partner, Resident& resident, uint32 now)
    {
        if (!bot || !partner)
            return;

        uint32 partnerGuidLow = partner->GetGUID().GetCounter();
        Resident* partnerResident = nullptr;
        for (Resident& r : _residents)
        {
            if (r.Bot.GuidLow == partnerGuidLow)
            {
                partnerResident = &r;
                break;
            }
        }

        if (!partnerResident)
            return;

        // Determine dialogue pool
        CityPoiType pool = CityPoiType::Plaza;
        if (Spot const* spot = FindSpot(resident.HubId, resident.SpotId))
        {
            if (spot->PoiType == CityPoiType::Bank || spot->PoiType == CityPoiType::AuctionHouse)
                pool = spot->PoiType;
        }

        std::vector<int16> candidates;
        for (int16 i = 0; i < Dialogues::kCrowdDialogueCount; ++i)
        {
            if (Dialogues::kCrowdDialogues[i].Pool == pool || Dialogues::kCrowdDialogues[i].Pool == CityPoiType::Plaza)
                candidates.push_back(i);
        }

        if (candidates.empty())
            return;

        int16 dialogueId = candidates[urand(0, static_cast<uint32>(candidates.size() - 1))];

        resident.ChatDialogueId = dialogueId;
        resident.ChatPartnerGuid = partnerGuidLow;
        resident.ChatInitiator = true;
        resident.ChatLineIdx = 0;
        resident.ChatDeadlineMs = now + 45 * 1000;

        partnerResident->ChatPartnerGuid = bot->GetGUID().GetCounter();
        partnerResident->ChatInitiator = false;
        partnerResident->ChatDeadlineMs = resident.ChatDeadlineMs;

        bot->SetFacingToObject(partner);
        partner->SetFacingToObject(bot);
    }

    void Manager::ClearResidentChatPartner(uint32 partnerGuidLow, uint32 expectedGuidLow)
    {
        for (Resident& r : _residents)
        {
            if (r.Bot.GuidLow == partnerGuidLow && r.ChatPartnerGuid == expectedGuidLow)
            {
                r.ChatPartnerGuid = 0;
                r.ChatInitiator = false;
                r.ChatDialogueId = -1;
                r.ChatLineIdx = 0;
                r.ChatDeadlineMs = 0;
                r.LastCrowdChatMs = GameTimeMS();
                break;
            }
        }
    }

    Resident* Manager::FindResident(uint32 guidLow)
    {
        for (Resident& r : _residents)
        {
            if (r.Bot.GuidLow == guidLow)
                return &r;
        }
        for (ReleasingResident& pending : _releasingResidents)
            if (pending.Actor.Bot.GuidLow == guidLow)
                return &pending.Actor;
        return nullptr;
    }

    Resident const* Manager::FindResident(uint32 guidLow) const
    {
        for (Resident const& r : _residents)
        {
            if (r.Bot.GuidLow == guidLow)
                return &r;
        }
        for (ReleasingResident const& pending : _releasingResidents)
            if (pending.Actor.Bot.GuidLow == guidLow)
                return &pending.Actor;
        return nullptr;
    }

    void Manager::AssignSpot(Resident& resident, Hub const& hub, Spot const& spot, bool allowStationary)
    {
        float angle = frand(0.0f, TwoPi);
        float distance = spot.Radius > 0.0f ? frand(0.0f, spot.Radius) : 0.0f;
        resident.HubId = hub.Id;
        resident.SpotId = spot.Id;
        resident.TargetMap = hub.MapId;
        resident.TargetX = spot.X + std::cos(angle) * distance;
        resident.TargetY = spot.Y + std::sin(angle) * distance;
        resident.TargetZ = spot.Z;
        resident.TargetO = spot.O;

        resident.Role = DetermineRole(hub, spot, resident.Bot);
        if (resident.Activity != CitizenActivity::ReleaseCycle)
        {
            resident.Activity = ChooseInitialActivity(resident.Role, spot);
        }

        // Dedicated venue performers (inn dancers, fishers, bed sleepers) remain stationary at their venue
        bool isDedicatedPerformer = (resident.Role == CitizenRole::InnDancer ||
                                     resident.Role == CitizenRole::Fisher ||
                                     resident.Activity == CitizenActivity::ReleaseCycle ||
                                     spot.PoiType == CityPoiType::InnBed);

        resident.Stationary = isDedicatedPerformer || (allowStationary && IsStaticServiceSpot(hub, spot) &&
            urand(1, 100) <= _staticServiceChance);
        resident.ActivityStartMs = GameTimeMS();
        resident.ActivityDurationMs = urand(45, 180) * 1000;
        resident.LastRoamWasHub = IsHubDistrict(hub.Name, spot.District);
        resident.CrowdHomeSpotId = spot.Id;
    }

    void Manager::SnapTargetToGround(Player* player, Resident& resident, Hub const& hub, Spot const& spot) const
    {
        // AssignSpot offsets X/Y inside the spot radius but keeps the spot's Z, which on a slope,
        // bridge or quay can be under or above the floor. Re-height the point from the bot's map
        // and fall back to the spot centre when it fails the destination checks.
        if (!player || !player->IsInWorld() || player->GetMapId() != hub.MapId ||
            (resident.TargetX == spot.X && resident.TargetY == spot.Y))
            return;
        float z = spot.Z;
        player->UpdateAllowedPositionZ(resident.TargetX, resident.TargetY, z);
        if (std::fabs(z - spot.Z) <= std::max(5.0f, spot.Radius * 0.5f) &&
            Movement::ValidateCityDestination(player, hub.MapId, resident.TargetX, resident.TargetY, z,
                resident.TargetO, spot.PoiType == CityPoiType::Dock))
        {
            resident.TargetZ = z;
            return;
        }
        resident.TargetX = spot.X;
        resident.TargetY = spot.Y;
        resident.TargetZ = spot.Z;
    }

    bool Manager::AddResident(Hub const& hub, uint32 poolCount)
    {
        if (std::any_of(_pendingAdmissions.begin(), _pendingAdmissions.end(),
            [&](PendingAdmission const& pending) { return pending.HubId == hub.Id; }))
            return false;
        if (IsWintergraspWarHub(hub))
            return false;
        std::vector<BotCandidate> candidates = LoadCandidates(hub, poolCount);
        if (candidates.empty())
            return false;
        uint32 now = GameTimeSeconds();
        PendingAdmission pending;
        pending.HubId = hub.Id;
        pending.Bot = candidates.front();
        pending.Deadline = now + std::max<uint32>(30, _tickSeconds * 3);
        pending.RequestId = PlayerbotsLife::SubmitAdmissionRequest(PlayerbotsLife::ReservationOwner::CityLife,
            ++_nextAssignmentId, { pending.Bot.GuidLow }, CalculateTotalBudget(poolCount), now, pending.Deadline);
        if (!pending.RequestId)
            return false;
        _pendingAdmissions.push_back(pending);
        return true;
    }

    bool Manager::AdmitResident(Hub const& hub, BotCandidate const& bot, PlayerbotsLife::LeaseToken token)
    {
        Player* player = FindPlayer(bot.GuidLow);
        Spot const* spot = ChooseSpot(hub);
        if (!hub.Enabled || !spot || !IsResidentUsable(player) || player->IsInCombat() ||
            player->IsBeingTeleported() || token.GuidLow != bot.GuidLow ||
            token.Owner != PlayerbotsLife::ReservationOwner::CityLife ||
            !PlayerbotsLife::ValidateLeaseState(token, PlayerbotsLife::LeaseState::Provisional) ||
            !PlayerbotsLife::PromoteLease(token))
            return false;
        Resident resident;
        resident.Bot = bot;
        resident.Token = token;
        resident.OriginalMap = player->GetMapId();
        resident.OriginalX = player->GetPositionX();
        resident.OriginalY = player->GetPositionY();
        resident.OriginalZ = player->GetPositionZ();
        resident.OriginalO = player->GetOrientation();
        AssignSpot(resident, hub, *spot, true);
        SnapTargetToGround(player, resident, hub, *spot);
        if (!Movement::ValidateCityDestination(player, hub.MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, spot->PoiType == CityPoiType::Dock))
        {
            if (_debug)
                LOG_WARN("module", "[CityLife] AdmitResident: Spot '{}/{}' rejected by ValidateCityDestination.", hub.Name, spot->Name);
            return false;
        }

        uint32 now = GameTimeSeconds();
        resident.NextMoveAt = now + urand(_relocateMinSeconds, _relocateMaxSeconds);
        resident.NextEmoteAtMs = GameTimeMS() + urand(_emoteMinSeconds, _emoteMaxSeconds) * 1000;
        resident.NextReservationRefreshAt = now + ReservationRefreshSeconds;
        resident.ReleaseAt = hub.ResidenceSeconds == 0 ? 0 : now + hub.ResidenceSeconds;
        resident.LastSweepZ = resident.TargetZ;
        ApplyCityStrategies(player, resident);
        SetStayPosition(player, resident);
        if (sRandomPlayerbotMgr.IsRandomBot(player))
            sRandomPlayerbotMgr.ScheduleTeleport(bot.GuidLow, std::max(ReservationSeconds, hub.ResidenceSeconds + MINUTE));
        Movement::ClearMountAndFlight(player);
        Movement::MoveToPosition(player, hub.MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, true);
        _residents.push_back(resident);
        ++_hubResidentCounts[resident.HubId];
        ++_changesThisTick;
        return true;
    }

    void Manager::CancelAdmissions()
    {
        PlayerbotsLife::CancelAdmissionRequest(0, PlayerbotsLife::ReservationOwner::CityLife);
        for (PendingAdmission const& pending : _pendingAdmissions)
            PlayerbotsLife::ReleaseLeaseExact(pending.Token);
        _pendingAdmissions.clear();
    }

    void Manager::ProcessAdmissionGrants()
    {
        uint32 now = GameTimeSeconds();
        auto pending = _pendingAdmissions.begin();
        while (pending != _pendingAdmissions.end())
        {
            Hub* hub = FindHub(pending->HubId);
            auto target = _hubTargets.find(pending->HubId);
            if (!hub || !hub->Enabled || pending->Deadline <= now || IsWintergraspWarHub(*hub) ||
                target == _hubTargets.end() || CountResidents(pending->HubId) >= target->second)
            {
                PlayerbotsLife::CancelAdmissionRequest(pending->RequestId, PlayerbotsLife::ReservationOwner::CityLife);
                PlayerbotsLife::ReleaseLeaseExact(pending->Token);
                pending = _pendingAdmissions.erase(pending);
                continue;
            }
            if (!pending->Token.Generation)
            {
                std::vector<PlayerbotsLife::LeaseToken> tokens;
                if (!PlayerbotsLife::TakeAdmissionGrant(pending->RequestId, PlayerbotsLife::ReservationOwner::CityLife, tokens))
                {
                    ++pending;
                    continue;
                }
                // The registry hands back only tokens whose lease survived, so this may be empty.
                if (tokens.size() != 1)
                {
                    for (auto const& token : tokens)
                        PlayerbotsLife::ReleaseLeaseExact(token);
                    pending = _pendingAdmissions.erase(pending);
                    continue;
                }
                pending->Token = tokens.front();
            }
            if (_changesThisTick >= _maxChangesPerTick)
            {
                ++pending;
                continue;
            }
            uint32 budget = CalculateTotalBudget(GetLiveWorldBotCount());
            if (PlayerbotsLife::GetCooperationSnapshot().CityLifeCount > budget ||
                !AdmitResident(*hub, pending->Bot, pending->Token))
                PlayerbotsLife::ReleaseLeaseExact(pending->Token);
            pending = _pendingAdmissions.erase(pending);
        }
    }

    void Manager::ReleaseResident(Resident const& resident, bool returnHome)
    {
        // A lease that is already gone (scavenged, or never promoted) still gets the restore pass:
        // ProcessReleasingResidents restores the bot unless another module now holds it.
        if (LifeMotion::Running(resident.Antic))
            LifeMotion::Halt(FindPlayer(resident.Bot.GuidLow), resident.Antic);
        PlayerbotsLife::BeginRelease(resident.Token);
        if (std::none_of(_releasingResidents.begin(), _releasingResidents.end(),
            [&](ReleasingResident const& pending) { return pending.Actor.Token.Generation == resident.Token.Generation; }))
        {
            _releasingResidents.push_back({ resident, returnHome, GameTimeSeconds() });
            _releasingDirty = true;
        }
    }

    void Manager::ProcessReleasingResidents(bool isShutdown)
    {
        // Wall-clock deadline for the throttled path, kept well under the registry's
        // RELEASING_LEASE_TTL_SEC so the scavenger never frees a bot still being restored.
        constexpr uint32 ReleaseDeadlineSeconds = 30;
        uint32 now = GameTimeSeconds();
        auto pending = _releasingResidents.begin();
        while (pending != _releasingResidents.end())
        {
            Resident& resident = pending->Actor;
            bool owned = PlayerbotsLife::ValidateLeaseState(resident.Token, PlayerbotsLife::LeaseState::Releasing);
            // Without our lease the bot is ours to restore only while no other module holds it.
            bool foreign = !owned && PlayerbotsLife::IsReserved(resident.Bot.GuidLow);
            Player* player = foreign ? nullptr : FindPlayer(resident.Bot.GuidLow);
            bool isHuman = player && IsHumanControlled(player);
            // A clock that stepped back (now < start) counts as not timed out.
            bool timedOut = now >= pending->ReleaseStartedAt && now - pending->ReleaseStartedAt >= ReleaseDeadlineSeconds;
            bool relinquish = !owned || !player || isHuman || isShutdown || timedOut;

            if (!relinquish)
            {
                if (!player->IsInWorld() || player->IsBeingTeleported() || _changesThisTick >= _maxChangesPerTick)
                {
                    ++pending;
                    continue;
                }
                if (player->IsInCombat())
                {
                    ++pending;
                    continue;
                }
                Activities::CrowdClearAfk(player);
                Activities::CrowdReleaseChat(player, resident);
                Activities::StopDance(player, resident);
                RestoreResidentState(player, resident);
                if (sRandomPlayerbotMgr.IsRandomBot(player))
                    sRandomPlayerbotMgr.ScheduleTeleport(resident.Bot.GuidLow);
                if (pending->ReturnHome && player->IsAlive())
                    Movement::MoveToPosition(player, resident.OriginalMap, resident.OriginalX,
                        resident.OriginalY, resident.OriginalZ, resident.OriginalO, true);
                ++_changesThisTick;
            }
            else
            {
                // Terminal fallback / takeover / timeout / shutdown path
                if (player && !isHuman)
                {
                    Activities::CrowdClearAfk(player);
                    Activities::CrowdReleaseChat(player, resident);
                    Activities::StopDance(player, resident);
                    RestoreResidentState(player, resident);
                    if (player->IsInWorld() && !player->IsInCombat() && !player->IsBeingTeleported() &&
                        pending->ReturnHome && player->IsAlive())
                        Movement::MoveToPosition(player, resident.OriginalMap, resident.OriginalX,
                            resident.OriginalY, resident.OriginalZ, resident.OriginalO, true);
                }
                // If human-controlled: DO NOT touch player strategies, state, or teleports! A real
                // player's group takeover resets the bot's strategies itself (mod-playerbots
                // AcceptInvitationAction / UpdateAIGroupMaster call ResetStrategies).
            }

            if (owned)
                PlayerbotsLife::ReleaseLeaseExact(resident.Token);
            pending = _releasingResidents.erase(pending);
        }
    }

    void Manager::ReleaseAll(bool returnHome)
    {
        CancelAdmissions();
        for (Resident const& resident : _residents)
            ReleaseResident(resident, returnHome && _returnBots);
        _residents.clear();
        _hubResidentCounts.clear();
    }

    void Manager::Shutdown()
    {
        ++_dataGeneration;
        ReleaseAll(true);
        ProcessReleasingResidents(true);
        _releasingResidents.clear();
        // Backstop: anything the restore pass could not reach still leaves the shared registry.
        PlayerbotsLife::ReleaseAllForOwner(PlayerbotsLife::ReservationOwner::CityLife);
    }

    void Manager::RemoveUnavailableResidents()
    {
        uint32 now = GameTimeSeconds();
        auto resident = _residents.begin();
        while (resident != _residents.end())
        {
            Player* player = FindPlayer(resident->Bot.GuidLow);
            Hub* hub = FindHub(resident->HubId);
            bool residenceExpired = resident->ReleaseAt != 0 && resident->ReleaseAt <= now && player &&
                !player->IsInCombat() && !player->IsBeingTeleported();
            // Wintergrasp battle started: leave the zone through the normal (throttled, restoring) release.
            bool warHub = hub && IsWintergraspWarHub(*hub);
            bool leaseLost = !PlayerbotsLife::ValidateActiveLease(resident->Token);
            bool usable = IsResidentUsable(player);

            // Residence over: walk to an inn bed and sleep a little before the final release.
            if (residenceExpired && hub && usable && !leaseLost && !warHub && _actReturnToInn &&
                resident->Activity != CitizenActivity::ReleaseCycle)
            {
                if (Spot const* restSpot = FindInnBedSpot(*hub))
                {
                    Activities::StopDance(player, *resident);
                    resident->Activity = CitizenActivity::ReleaseCycle;
                    AssignSpot(*resident, *hub, *restSpot, true);
                    resident->ReleaseAt = now + 20; // 20s in bed before the final release
                    SetStayPosition(player, *resident);
                    Movement::MoveToPosition(player, hub->MapId, resident->TargetX, resident->TargetY,
                        resident->TargetZ, resident->TargetO, false);
                    ++resident;
                    continue;
                }
            }

            if (!hub || leaseLost || !usable || residenceExpired || warHub)
            {
                if (player && player->getStandState() == UNIT_STAND_STATE_SLEEP)
                    player->SetStandState(UNIT_STAND_STATE_STAND);
                if (player)
                    player->HandleEmoteCommand(EMOTE_ONESHOT_NONE);

                auto countIt = _hubResidentCounts.find(resident->HubId);
                if (countIt != _hubResidentCounts.end() && countIt->second > 0)
                    --countIt->second;

                ReleaseResident(*resident, (residenceExpired || warHub) && _returnBots);
                resident = _residents.erase(resident);
            }
            else
                ++resident;
        }
    }

    void Manager::MaintainPopulation()
    {
        RemoveUnavailableResidents();

        // Live World Bot Sensor: measure exact active random bots in the world
        uint32 eligiblePoolCount = GetLiveWorldBotCount();
        uint32 totalBudget = CalculateTotalBudget(eligiblePoolCount);

        std::unordered_map<uint32, uint32> targets;
        if (_distributionMode == DistributionMode::Scaled)
        {
            std::vector<PopulationWeight> weights;
            uint64 totalDemand = 0;
            for (Hub const& hub : _hubs)
            {
                targets[hub.Id] = 0;
                if (!hub.Enabled || hub.Spots.empty())
                    continue;
                uint32 demand = TargetPopulation(hub);
                weights.push_back({ hub.Id, demand, hub.MinCount, hub.Priority });
                totalDemand += demand;
            }
            // Every hub gets its full demand while the budget covers it; only an over-budget demand
            // is shared out (minimums first, then by weight). Upstream shrank the budget itself by
            // demand/configured, so one occupied city got a sliver of a budget eaten by minimums.
            if (totalDemand <= totalBudget)
            {
                for (PopulationWeight const& weight : weights)
                    targets[weight.HubId] = weight.Weight;
            }
            else
            {
                auto allocation = AllocatePopulation(weights, totalBudget);
                for (size_t index = 0; index < weights.size(); ++index)
                    targets[weights[index].HubId] = allocation[index];
            }
        }
        else // Legacy sequential allocation
        {
            uint32 remaining = totalBudget;
            for (Hub const& hub : _hubs)
            {
                uint32 target = std::min(TargetPopulation(hub), remaining);
                targets[hub.Id] = target;
                remaining -= target;
            }
        }

        _hubTargets = targets;
        uint32 changes = _changesThisTick;
        for (Hub const& hub : _hubs)
        {
            uint32 target = targets[hub.Id];
            while (CountResidents(hub.Id) > target && changes < _maxChangesPerTick)
            {
                auto found = std::find_if(_residents.rbegin(), _residents.rend(),
                    [&](Resident const& resident)
                    {
                        Player* player = FindPlayer(resident.Bot.GuidLow);
                        return resident.HubId == hub.Id && player && !player->IsInCombat() &&
                            !player->IsBeingTeleported() && !player->duel;
                    });
                if (found == _residents.rend())
                    break;
                auto eraseAt = std::next(found).base();
                auto countIt = _hubResidentCounts.find(eraseAt->HubId);
                if (countIt != _hubResidentCounts.end() && countIt->second > 0)
                    --countIt->second;
                ReleaseResident(*eraseAt, _returnBots);
                _residents.erase(eraseAt);
                ++changes;
                ++_changesThisTick;
            }
        }

        std::unordered_set<uint32> blockedHubs;
        while (changes < _maxChangesPerTick)
        {
            Hub const* selectedHub = nullptr;
            uint32 selectedCurrent = 0;
            uint32 selectedTarget = 0;
            for (Hub const& hub : _hubs)
            {
                uint32 target = targets[hub.Id];
                uint32 current = CountResidents(hub.Id) + static_cast<uint32>(std::count_if(
                    _pendingAdmissions.begin(), _pendingAdmissions.end(),
                    [&](PendingAdmission const& pending) { return pending.HubId == hub.Id; }));
                if (current >= target || blockedHubs.find(hub.Id) != blockedHubs.end())
                    continue;

                bool lowerFillRatio = selectedHub &&
                    static_cast<uint64>(current) * selectedTarget <
                    static_cast<uint64>(selectedCurrent) * target;
                if (!selectedHub || lowerFillRatio)
                {
                    selectedHub = &hub;
                    selectedCurrent = current;
                    selectedTarget = target;
                }
            }

            if (!selectedHub)
                break;
            if (AddResident(*selectedHub, eligiblePoolCount))
            {
                ++changes;
                continue;
            }

            blockedHubs.insert(selectedHub->Id);
        }
    }

    void Manager::RelocateResident(Resident& resident, Hub const& hub)
    {
        if (resident.Stationary ||
            resident.Activity == CitizenActivity::ReleaseCycle ||
            resident.Activity == CitizenActivity::InnDance ||
            resident.Activity == CitizenActivity::FishAtDocks ||
            resident.Role == CitizenRole::InnDancer ||
            resident.Role == CitizenRole::Fisher)
        {
            return;
        }

        Player* player = FindPlayer(resident.Bot.GuidLow);
        if (!player)
            return;

        CitizenActivity nextActivity = PickNextActivity(resident, hub);
        Spot const* spot = ChooseSpotForActivity(hub, nextActivity, resident.SpotId, player);

        if (!spot && _alternatingDistricts && UsesAlternatingHubRoute(hub.Name))
        {
            bool targetHub = !resident.LastRoamWasHub;
            spot = ChooseSpot(hub, resident.SpotId, true, targetHub, player);
            if (spot)
            {
                resident.LastRoamWasHub = IsHubDistrict(hub.Name, spot->District);
            }
        }

        if (!spot)
        {
            spot = ChooseSpot(hub, resident.SpotId, false, false, player);
            if (spot)
            {
                resident.LastRoamWasHub = IsHubDistrict(hub.Name, spot->District);
            }
        }

        if (!spot)
            return;

        Activities::StopDance(player, resident);
        if (resident.Sitting || player->getStandState() != UNIT_STAND_STATE_STAND)
        {
            player->SetStandState(UNIT_STAND_STATE_STAND);
            resident.Sitting = false;
        }

        AssignSpot(resident, hub, *spot, false);
        SnapTargetToGround(player, resident, hub, *spot);
        // AssignSpot derived the activity from the spot; keep the picked one when the spot fits it
        // (a fallback spot of another kind keeps the activity that suits it).
        if (resident.Role == CitizenRole::Flex && SpotFitsActivity(*spot, nextActivity))
            resident.Activity = nextActivity;
        uint32 nowSec = GameTimeSeconds();
        resident.NextMoveAt = nowSec + urand(_relocateMinSeconds, _relocateMaxSeconds);
        resident.HopDismountRolled = false;

        SetStayPosition(player, resident);

        // Mounting decision for outdoor distance
        float distance = player->GetDistance2d(resident.TargetX, resident.TargetY);
        // The walk to the new spot may outlast several ticks: hold the leash teleport off for a
        // generous walking-pace estimate (4 yd/s plus slack), capped at two minutes.
        resident.LeashGraceUntil = nowSec + std::min<uint32>(120, 15 + static_cast<uint32>(distance / 4.0f));
        if (_actMountedTravel && distance > 30.0f && urand(1, 100) <= _mountBetweenPoiChance)
        {
            Movement::TryMount(player);
        }

        Movement::MoveToPosition(player, hub.MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, false);
    }

    void Manager::MaintainResidents()
    {
        uint32 nowSec = GameTimeSeconds();
        uint32 nowMs = GameTimeMS();

        for (Resident& resident : _residents)
        {
            Hub* hub = FindHub(resident.HubId);
            Player* player = FindPlayer(resident.Bot.GuidLow);
            // mod-playerbots' PlayerbotHolder::UpdateSessions acks a bot's teleport every update.
            if (!hub || !player || player->IsBeingTeleported() ||
                !PlayerbotsLife::ValidateActiveLease(resident.Token) ||
                !IsResidentUsable(player))
                continue;

            if ((!_activitiesAfkEnable || !_crowdEnable || !_actCrowdScenes) && resident.AfkSetByModule)
                Activities::CrowdClearAfk(player);
            // Reservation refresh
            if (resident.NextReservationRefreshAt <= nowSec && sRandomPlayerbotMgr.IsRandomBot(player))
            {
                sRandomPlayerbotMgr.ScheduleTeleport(resident.Bot.GuidLow,
                    std::max(ReservationSeconds, hub->ResidenceSeconds + MINUTE));
                resident.NextReservationRefreshAt = nowSec + ReservationRefreshSeconds;
            }

            if (player->IsInCombat() || player->duel || player->IsInFlight())
                continue;
            // An antic under way moves the bot itself (UpdateAntics, every ~100 ms); leave it be.
            if (LifeMotion::Running(resident.Antic))
                continue;
            // Sink Forensics Check
            Movement::CheckSinkForensics(player, resident.LastSweepZ, _sinkForensics);
            resident.LastSweepZ = player->GetPositionZ();

            // Multi-level floor protection: if way below lowest POI, recover
            float lowestZ = 0.0f;
            if (Movement::LowestPoiZWithin(*hub, player->GetPositionX(), player->GetPositionY(), 20.0f, &lowestZ))
            {
                if (player->GetPositionZ() < lowestZ - 15.0f && !player->IsBeingTeleported())
                {
                    Movement::MoveToPosition(player, hub->MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, true);
                    continue;
                }
            }

            // Leash check: skipped while a relocation walk or a crowd wander is still under way.
            // Fishers and dancers belong at their venue and get a tighter leash.
            bool travelling = (resident.LeashGraceUntil > nowSec && resident.LeashGraceUntil - nowSec <= 120) ||
                resident.CrowdPhaseState == CrowdPhase::WanderOut || resident.CrowdPhaseState == CrowdPhase::WanderBack;
            bool const isDedicatedPerformer = resident.Role == CitizenRole::Fisher ||
                resident.Role == CitizenRole::InnDancer;
            float const distToTarget = player->GetDistance2d(resident.TargetX, resident.TargetY);
            float const leash = isDedicatedPerformer ? std::min<float>(20.0f, _leashRadius) : _leashRadius;

            // Off the hub's map, or settled in another zone than its spot's (a stray path, a knockback
            // off a ledge): a resident that just took up this activity is put back; one that wanders
            // off again is released. The spot's own zone is the reference, so a hub whose zone_id is
            // wrong or whose spots straddle a border does not lose its residents.
            bool const inForeignZone = player->GetMapId() != hub->MapId ||
                (!travelling && player->GetZoneId() != player->GetMap()->GetZoneId(player->GetPhaseMask(),
                    resident.TargetX, resident.TargetY, resident.TargetZ));
            if (inForeignZone && player->GetMapId() == hub->MapId && nowMs - resident.ActivityStartMs > 30000)
            {
                ReleaseResident(resident, false);
                continue;
            }

            if (inForeignZone || (!travelling && distToTarget > leash))
            {
                ApplyCityStrategies(player, resident);
                SetStayPosition(player, resident);
                Movement::MoveToPosition(player, hub->MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, true);
                continue;
            }

            // Arrival check: dismount when close to target
            if (distToTarget <= 4.0f)
            {
                if (!resident.AtSpot)
                {
                    resident.AtSpot = true;
                    Movement::DismountIfMounted(player);
                    SetStayPosition(player, resident);
                }
            }
            else
            {
                resident.AtSpot = false;
            }

            // Follow mod-playerbots' BotActiveAlone: a resident its rules have paused (no real player
            // in range or in the zone, not in the rotating share, SmartScale) keeps its spot and its
            // lease but starts nothing new -- no activity change or roaming -- until it wakes,
            // so a player walking up finds the residents already there and watches them come to life.
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player); !ai || !ai->AllowActivity())
                continue;

            // Stalled short of the spot after the walk's grace ran out (a blocked path, a bot that
            // stopped to fight): send it on again; the leash above handles one that drifted far.
            if (!resident.AtSpot && !travelling && !player->isMoving() &&
                resident.CrowdPhaseState == CrowdPhase::Linger)
            {
                Movement::MoveToPosition(player, hub->MapId, resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetO, false);
                resident.LeashGraceUntil = nowSec + std::min<uint32>(120, 15 + static_cast<uint32>(distToTarget / 4.0f));
                continue;
            }

            bool const venueBound = resident.Stationary ||
                resident.Activity == CitizenActivity::ReleaseCycle ||
                resident.Activity == CitizenActivity::InnDance ||
                resident.Activity == CitizenActivity::FishAtDocks ||
                isDedicatedPerformer;

            if (resident.AtSpot && resident.ActivityDurationMs &&
                resident.Activity != CitizenActivity::ReleaseCycle &&
                nowMs - resident.ActivityStartMs >= resident.ActivityDurationMs &&
                !player->IsNonMeleeSpellCast(false) && !resident.ChatPartnerGuid &&
                resident.CrowdPhaseState == CrowdPhase::Linger)
            {
                if (Spot const* spot = FindSpot(resident.HubId, resident.SpotId))
                {
                    if (resident.Activity == CitizenActivity::InnDance)
                    {
                        if (!resident.Sitting)
                        {
                            // A set danced: bow or applaud and sit down for a rest.
                            Activities::StopDance(player, resident);
                            player->HandleEmoteCommand(roll_chance_i(50) ? EMOTE_ONESHOT_BOW : EMOTE_ONESHOT_APPLAUD);
                            player->SetStandState(UNIT_STAND_STATE_SIT);
                            resident.Sitting = true;
                            resident.ActivityDurationMs = urand(60, 150) * 1000;
                        }
                        else
                        {
                            // Rested: stand up; ExecuteInnDance starts the next set.
                            player->SetStandState(UNIT_STAND_STATE_STAND);
                            resident.Sitting = false;
                            resident.ActivityDurationMs = urand(60, 180) * 1000;
                        }
                        resident.ActivityStartMs = nowMs;
                    }
                    else if (resident.Activity == CitizenActivity::FishAtDocks)
                    {
                        if (!resident.Sitting)
                        {
                            // Fished a while: sit down by the water for a rest.
                            player->HandleEmoteCommand(EMOTE_ONESHOT_NONE);
                            player->SetStandState(UNIT_STAND_STATE_SIT);
                            resident.Sitting = true;
                            resident.ActivityDurationMs = urand(45, 120) * 1000;
                        }
                        else
                        {
                            player->SetStandState(UNIT_STAND_STATE_STAND);
                            resident.Sitting = false;
                            resident.ActivityDurationMs = urand(60, 180) * 1000;
                        }
                        resident.ActivityStartMs = nowMs;
                    }
                    else if ((resident.Activity == CitizenActivity::SitInTavern ||
                              resident.Activity == CitizenActivity::DrinkInTavern) &&
                             (resident.Stationary || !_roaming || !_actRoaming))
                    {
                        // Tavern regulars stay seated and alternate between sitting and drinking.
                        resident.Activity = resident.Activity == CitizenActivity::SitInTavern ?
                            CitizenActivity::DrinkInTavern : CitizenActivity::SitInTavern;
                        resident.ActivityStartMs = nowMs;
                        resident.ActivityDurationMs = urand(60, 180) * 1000;
                    }
                    else
                    {
                        if (resident.Sitting)
                        {
                            player->SetStandState(UNIT_STAND_STATE_STAND);
                            resident.Sitting = false;
                        }
                        Activities::StopDance(player, resident);
                        player->HandleEmoteCommand(EMOTE_ONESHOT_NONE);

                        // Done here: a roaming resident moves on to its next activity at once.
                        if (!venueBound && _roaming && _actRoaming)
                        {
                            RelocateResident(resident, *hub);
                            continue;
                        }

                        resident.Role = DetermineRole(*hub, *spot, resident.Bot);
                        resident.Activity = ChooseInitialActivity(resident.Role, *spot);
                        resident.ActivityStartMs = nowMs;
                        resident.ActivityDurationMs = urand(45, 180) * 1000;
                    }
                }
            }
            // Roaming relocation
            if (_roaming && _actRoaming && !venueBound && resident.NextMoveAt <= nowSec)
            {
                RelocateResident(resident, *hub);
                continue;
            }

            // Activity Execution Dispatch
            switch (resident.Activity)
            {
                case CitizenActivity::CrowdSocial:
                    if (_actCrowdScenes && _crowdEnable)
                        Activities::ExecuteLivingCrowd(resident, player, *hub, nowMs, _chatScriptedConversations,
                            _chatSayCooldownSec, _crowdPlayerRadius, _crowdChatCooldownSec, _crowdAfkMinSec, _crowdAfkMaxSec, _activitiesAfkEnable);
                    break;
                case CitizenActivity::WatchDuels:
                    if (_actDuelWatching)
                        Activities::ExecuteDuelSpectator(resident, player, *hub, nowMs, _spectatorWatchRadius);
                    break;
                case CitizenActivity::FishAtDocks:
                    if (_actFishing)
                        Activities::ExecuteFishing(resident, player, *hub, nowMs);
                    break;
                case CitizenActivity::InnDance:
                    if (_actInnDancing)
                        Activities::ExecuteInnDance(resident, player, *hub, nowMs, _chatGreetings, _chatSayCooldownSec);
                    break;
                case CitizenActivity::PlazaSocial:
                    if (_actPlazaSocial)
                        Activities::ExecutePlazaSocial(resident, player, *hub, nowMs, _chatUnrestricted, _chatRandomSayChance, _chatSayCooldownSec);
                    break;
                case CitizenActivity::SitInTavern:
                    if (_actTavernSitting)
                        Activities::ExecuteTavernSitting(resident, player, *hub, nowMs);
                    break;
                case CitizenActivity::DrinkInTavern:
                    if (_actTavernDrinking)
                        Activities::ExecuteTavernDrinking(resident, player, *hub, nowMs);
                    break;
                case CitizenActivity::WalkCityWalls:
                    if (_actWallWalking)
                        Activities::ExecuteWallPatrol(resident, player, *hub, nowMs);
                    break;
                case CitizenActivity::VisitAuctionHouse:
                    if (_actAuctionHouse)
                        Activities::ExecuteVisitNpc(resident, player, *hub, nowMs, UNIT_NPC_FLAG_AUCTIONEER);
                    break;
                case CitizenActivity::VisitTrainer:
                    if (_actTrainer)
                        Activities::ExecuteVisitNpc(resident, player, *hub, nowMs, UNIT_NPC_FLAG_TRAINER);
                    break;
                case CitizenActivity::VisitMailbox:
                    if (_actMailbox)
                        Activities::ExecuteVisitMailbox(resident, player, *hub, nowMs);
                    break;
                case CitizenActivity::BuyFood:
                    if (_actFoodVendor)
                        Activities::ExecuteBuyFood(resident, player, *hub, nowMs, _enableInteractions && _enableBuyFood);
                    break;
                case CitizenActivity::WanderDistrict:
                    if (_actRoaming && TryStartAntic(resident, player, nowMs))
                        break;
                    if (_actRoaming && MsReached(nowMs, resident.NextEmoteAtMs))
                    {
                        Activities::ExecuteRandomEmote(resident, player, *hub, nowMs);
                        resident.NextEmoteAtMs = nowMs + urand(_emoteMinSeconds, _emoteMaxSeconds) * 1000;
                    }
                    break;
                case CitizenActivity::ReleaseCycle:
                    Activities::ExecuteReleaseCycle(resident, player, *hub, nowMs);
                    break;
                case CitizenActivity::RandomEmote:
                default:
                    if (_actRandomEmotes && resident.Activity == CitizenActivity::RandomEmote &&
                        TryStartAntic(resident, player, nowMs))
                        break;
                    if (_actRandomEmotes && MsReached(nowMs, resident.NextEmoteAtMs))
                    {
                        Activities::ExecuteRandomEmote(resident, player, *hub, nowMs);
                        resident.NextEmoteAtMs = nowMs + urand(_emoteMinSeconds, _emoteMaxSeconds) * 1000;
                    }
                    break;
            }
        }
    }

    bool Manager::TryStartAntic(Resident& resident, Player* player, uint32 nowMs, LifeMotion::Antic kind)
    {
        if (kind == LifeMotion::Antic::None)
        {
            // Only a resident standing about at its spot, not sitting, chatting or on its way somewhere.
            if (!resident.AtSpot || resident.Sitting || resident.ChatPartnerGuid || player->isMoving() ||
                !LifeMotion::Ready(resident.Antic, nowMs) || !LifeMotion::RollAntic(_antics, _tickSeconds * IN_MILLISECONDS))
                return false;
            kind = LifeMotion::PickAntic(_antics);
        }
        else if (LifeMotion::Running(resident.Antic))
            LifeMotion::End(player, resident.Antic, nowMs, _antics);
        if (!LifeMotion::Begin(player, resident.Antic, kind, nowMs, _antics))
            return false;
        if (_debug)
            LOG_INFO("module", "[CityLife] {} starts antic {}.", player->GetName(), LifeMotion::AnticName(kind));
        return true;
    }

    void Manager::UpdateAntics()
    {
        if (!_antics.Enable)
        {
            // Switched off by a config reload: finish whatever is under way.
            for (Resident& resident : _residents)
                if (LifeMotion::Running(resident.Antic))
                    LifeMotion::End(FindPlayer(resident.Bot.GuidLow), resident.Antic, GameTimeMS(), _antics);
            return;
        }

        uint32 nowMs = GameTimeMS();
        for (Resident& resident : _residents)
        {
            bool running = LifeMotion::Running(resident.Antic);
            // A resident riding in to its spot: some hop and get off in mid-air instead of stopping first.
            bool arriving = !running && !resident.AtSpot && !resident.HopDismountRolled && _antics.HopDismountChance;
            if (!running && !arriving)
                continue;

            Player* player = FindPlayer(resident.Bot.GuidLow);
            if (arriving)
            {
                if (!player || !player->IsMounted() || !player->isMoving() ||
                    player->GetDistance2d(resident.TargetX, resident.TargetY) > 9.0f ||
                    !ValidateResidentAction(player, resident))
                    continue;
                resident.HopDismountRolled = true;
                if (urand(1, 100) > _antics.HopDismountChance ||
                    !LifeMotion::BeginHopDismount(player, resident.Antic, resident.TargetX, resident.TargetY, nowMs))
                    continue;
            }

            bool going = player && ValidateResidentAction(player, resident);
            if (going && resident.Antic.Kind != LifeMotion::Antic::HopDismount)
            {
                // A resident mod-playerbots has paused stops, as it starts nothing new either.
                PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
                going = ai && ai->AllowActivity();
            }
            if (going)
            {
                // Within the leash, so the maintenance pass never pulls it back mid-antic.
                LifeMotion::Bound home;
                home.HomeX = resident.TargetX;
                home.HomeY = resident.TargetY;
                home.HomeRadius = std::min<float>(12.0f, std::max<float>(6.0f, float(_leashRadius) - 5.0f));
                going = LifeMotion::Advance(player, resident.Antic, nowMs, _antics, home);
            }
            if (!going)
            {
                LifeMotion::End(player, resident.Antic, nowMs, _antics);
                // Hand mod-playerbots' "stay" back to the spot; the maintenance pass walks it home.
                if (player && ValidateResidentAction(player, resident))
                    SetStayPosition(player, resident);
            }
        }
    }

    void Manager::ApplyCityStrategies(Player* player, Resident& resident) const
    {
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            std::vector<std::string> beforeStrategies = ai->GetStrategies(BOT_STATE_NON_COMBAT);
            if (!resident.Journal.CapturedOriginalStrategies)
            {
                resident.Journal.CapturedOriginalStrategies = true;
                resident.Journal.OriginalNonCombatStrategies = beforeStrategies;
            }

            std::string strategiesToApply = _nonCombatStrategies;
            if (!_chatScriptedConversations && !_chatGreetings && !_chatUnrestricted)
                strategiesToApply += ",-chat,-emote";

            ai->ChangeStrategy(strategiesToApply, BOT_STATE_NON_COMBAT);

            std::vector<std::string> afterStrategies = ai->GetStrategies(BOT_STATE_NON_COMBAT);

            // Record actual additions (including native sibling side effects):
            for (std::string const& s : afterStrategies)
            {
                if (std::find(beforeStrategies.begin(), beforeStrategies.end(), s) == beforeStrategies.end())
                {
                    if (std::find(resident.Journal.AppliedStrategies.begin(), resident.Journal.AppliedStrategies.end(), s) == resident.Journal.AppliedStrategies.end())
                        resident.Journal.AppliedStrategies.push_back(s);
                }
            }

            // Record actual removals (including native sibling side effects):
            for (std::string const& s : beforeStrategies)
            {
                if (std::find(afterStrategies.begin(), afterStrategies.end(), s) == afterStrategies.end())
                {
                    if (std::find(resident.Journal.RemovedStrategies.begin(), resident.Journal.RemovedStrategies.end(), s) == resident.Journal.RemovedStrategies.end())
                        resident.Journal.RemovedStrategies.push_back(s);
                }
            }
        }
    }

    void Manager::SetStayPosition(Player* player, Resident& resident) const
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai)
            return;
        PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
        PositionInfo stay = positions["stay"];
        if (!resident.Journal.ModifiedStayPosition)
        {
            resident.Journal.ModifiedStayPosition = true;
            resident.Journal.HadStayPosition = stay.isSet();
            resident.Journal.PrevStayX = stay.x;
            resident.Journal.PrevStayY = stay.y;
            resident.Journal.PrevStayZ = stay.z;
            resident.Journal.PrevStayMap = stay.mapId;
        }
        resident.Journal.AppliedStayX = resident.TargetX;
        resident.Journal.AppliedStayY = resident.TargetY;
        resident.Journal.AppliedStayZ = resident.TargetZ;
        resident.Journal.AppliedStayMap = resident.TargetMap;
        stay.Set(resident.TargetX, resident.TargetY, resident.TargetZ, resident.TargetMap);
        positions["stay"] = stay;
    }

    void Manager::RestoreResidentState(Player* player, Resident const& resident) const
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai)
            return;

        PositionMap& positions = ai->GetAiObjectContext()->GetValue<PositionMap&>("position")->Get();
        PositionInfo stay = positions["stay"];
        // Put back the stay position captured before the first change, whatever the bot's own AI
        // did to it while it was a resident.
        if (resident.Journal.ModifiedStayPosition)
        {
            if (resident.Journal.HadStayPosition)
                stay.Set(resident.Journal.PrevStayX, resident.Journal.PrevStayY, resident.Journal.PrevStayZ, resident.Journal.PrevStayMap);
            else
                stay.Reset();
            positions["stay"] = stay;
        }

        // True delta restoration: revert only mutations made by City Life if current state still matches
        if (resident.Journal.CapturedOriginalStrategies)
        {
            std::vector<std::string> currentStrategies = ai->GetStrategies(BOT_STATE_NON_COMBAT);

            // Revert strategies added by City Life if still active and was NOT originally present
            for (std::string const& s : resident.Journal.AppliedStrategies)
            {
                bool wasOriginal = std::find(resident.Journal.OriginalNonCombatStrategies.begin(),
                                             resident.Journal.OriginalNonCombatStrategies.end(), s) != resident.Journal.OriginalNonCombatStrategies.end();
                if (!wasOriginal && std::find(currentStrategies.begin(), currentStrategies.end(), s) != currentStrategies.end())
                    ai->ChangeStrategy("-" + s, BOT_STATE_NON_COMBAT);
            }

            // Restore strategies removed by City Life if originally present and not currently active
            for (std::string const& s : resident.Journal.RemovedStrategies)
            {
                bool wasOriginal = std::find(resident.Journal.OriginalNonCombatStrategies.begin(),
                                             resident.Journal.OriginalNonCombatStrategies.end(), s) != resident.Journal.OriginalNonCombatStrategies.end();
                if (!wasOriginal || std::find(currentStrategies.begin(), currentStrategies.end(), s) != currentStrategies.end())
                    continue;
                auto siblings = ai->GetAiObjectContext()->GetSiblingStrategy(s);
                auto live = ai->GetStrategies(BOT_STATE_NON_COMBAT);
                bool newerSibling = std::any_of(live.begin(), live.end(),
                    [&](std::string const& current) { return siblings.count(current) != 0; });
                if (!newerSibling)
                    ai->ChangeStrategy("+" + s, BOT_STATE_NON_COMBAT);
            }
        }
    }

    void Manager::Update(uint32 diff)
    {
        ProcessQueuedDisengages();
        _queryProcessor.ProcessReadyCallbacks();
        _transactionProcessor.ProcessReadyCallbacks();
        PlayerbotsLife::ResolveAdmissionRequests(GameTimeSeconds());
        bool managementTick = _timerMs <= diff;
        if (managementTick)
        {
            _timerMs = _tickSeconds * IN_MILLISECONDS;
            _changesThisTick = 0;
        }
        else
            _timerMs -= diff;

        _releasingTimerMs += diff;
        if (_releasingDirty || _releasingTimerMs >= 250)
        {
            _releasingTimerMs = 0;
            _releasingDirty = false;
            ProcessReleasingResidents(false);
        }

        if (!_enable || !_databaseReady)
            return;
        if (_startupElapsedMs < _startupDelaySeconds * IN_MILLISECONDS)
        {
            _startupElapsedMs += diff;
            return;
        }
        if (managementTick)
        {
            uint32 nowSec = GameTimeSeconds();
            PlayerbotsLife::ScavengeStaleLeases(nowSec);
            MaintainPopulation();
            MaintainResidents();
        }
        _anticTimerMs += diff;
        if (_anticTimerMs >= 100)
        {
            _anticTimerMs = 0;
            UpdateAntics();
        }
        ProcessAdmissionGrants(); // Acknowledge five-second grants independently of management ticks.
    }

    void Manager::PrintStatus(ChatHandler* handler) const
    {
        handler->PSendSysMessage("Lifecycle: pendingAdmissions={} releasing={} changesThisTick={}/{}",
            _pendingAdmissions.size(), _releasingResidents.size(), _changesThisTick, _maxChangesPerTick);
        uint32 startupTarget = _startupDelaySeconds * IN_MILLISECONDS;
        uint32 startupLeft = _startupElapsedMs < startupTarget ?
            (startupTarget - _startupElapsedMs + 999) / 1000 : 0;
        uint32 liveBots = GetLiveWorldBotCount();
        handler->PSendSysMessage(
            "City Life: enabled={} DB={} startup={}s residents={}/{} (live bots={}) time={}% hubs={} crowd={}",
            _enable ? 1 : 0, _databaseReady ? "ready" : "missing", startupLeft,
            _residents.size(), _maxTotalPopulation == 0 ? "uncapped" : std::to_string(_maxTotalPopulation).c_str(),
            liveBots, CurrentTimeMultiplier(), _hubs.size(),
            _crowdEnable ? 1 : 0);
        for (Hub const& hub : _hubs)
        {
            // demand: what the hub asks for now; target: what the last tick allocated it.
            auto allocated = _hubTargets.find(hub.Id);
            handler->PSendSysMessage("{} [{}] lvl {}-{} cfg={} demand={} target={} current={} spots={}",
                hub.Name, TeamName(hub.Team), hub.MinLevel, hub.MaxLevel, hub.ConfiguredPopulation,
                TargetPopulation(hub), allocated == _hubTargets.end() ? 0 : allocated->second,
                CountResidents(hub.Id), hub.Spots.size());
        }
        uint32 anticsRunning = uint32(std::count_if(_residents.begin(), _residents.end(),
            [](Resident const& resident) { return LifeMotion::Running(resident.Antic); }));
        handler->PSendSysMessage("Antics: enabled={} running={} chance={}%/min cooldown={}s hopDismount={}%",
            _antics.Enable ? 1 : 0, anticsRunning, _antics.ChancePerMinute, _antics.CooldownSeconds, _antics.HopDismountChance);
        auto snapshot = PlayerbotsLife::GetCooperationSnapshot();
        handler->PSendSysMessage("Admissions pending={} shared waiting={} unacknowledged={} ownership={} work used={}/{}",
            _pendingAdmissions.size(), snapshot.WaitingAdmissions, snapshot.UnacknowledgedGrants,
            snapshot.CityLifeCount, _changesThisTick, _maxChangesPerTick);
    }

    void Manager::PrintDebugStatus(ChatHandler* handler) const
    {
        handler->PSendSysMessage("City Life Diagnostics:");
        handler->PSendSysMessage("BudgetMode: {} | DemandMode: {} | Baseline: {}% | Grace: {}s",
            _budgetMode == BudgetMode::Percentage ? "Percentage" : "Count",
            _demandMode == DemandMode::Hybrid ? "Hybrid" : (_demandMode == DemandMode::Dynamic ? "Dynamic" : "Fixed"),
            _baselinePercent, _burstGraceSeconds);
        handler->PSendSysMessage("LivingCrowd: AwakeRadius={}yd ChatCooldown={}s AFK={}-{}s",
            _crowdPlayerRadius, _crowdChatCooldownSec, _crowdAfkMinSec, _crowdAfkMaxSec);
        handler->PSendSysMessage("Active Residents by Role:");

        uint32 crowdCount = 0, plazaCount = 0, spectatorCount = 0, fisherCount = 0, dancerCount = 0, flexCount = 0;
        for (Resident const& r : _residents)
        {
            switch (r.Role)
            {
                case CitizenRole::Crowd: ++crowdCount; break;
                case CitizenRole::PlazaTalker: ++plazaCount; break;
                case CitizenRole::Spectator: ++spectatorCount; break;
                case CitizenRole::Fisher: ++fisherCount; break;
                case CitizenRole::InnDancer: ++dancerCount; break;
                default: ++flexCount; break;
            }
        }
        handler->PSendSysMessage("  Crowd={} Plaza={} Spectators={} Fishers={} Dancers={} Citizens={}",
            crowdCount, plazaCount, spectatorCount, fisherCount, dancerCount, flexCount);
        // Duel slots are mod-hearthside-pvp's; the registry is shared.
        PlayerbotsLife::CooperationSnapshot snap = PlayerbotsLife::GetCooperationSnapshot();
        handler->PSendSysMessage("Cooperation: Reserved Total={} (City={}, PvP={}) | Leases: Prov={} Active={} Rel={} | DuelSlots={}",
            snap.TotalReserved, snap.CityLifeCount, snap.PvPLifeCount,
            snap.ProvisionalCount, snap.ActiveCount, snap.ReleasingCount, snap.ActiveDuelSlots);
    }

    void Manager::PrintHelp(ChatHandler* handler) const
    {
        handler->PSendSysMessage("City Life commands:");
        handler->PSendSysMessage(".citylife status");
        handler->PSendSysMessage(".citylife compat");
        handler->PSendSysMessage(".citylife reload [config|data|all]");
        handler->PSendSysMessage(".citylife debug status");
        handler->PSendSysMessage(".citylife debug antic <name> <Dance|Strafe|HopBackForth|HopSideToSide|BunnyHop>");
        handler->PSendSysMessage(".citylife hub list");
        handler->PSendSysMessage(".citylife hub add <name> <alliance|horde|any> <minLevel> <maxLevel> <population> [priority]");
        handler->PSendSysMessage(".citylife hub population <name> <count>");
        handler->PSendSysMessage(".citylife hub enable|disable <name>");
        handler->PSendSysMessage(".citylife spot list <hub>");
        handler->PSendSysMessage(".citylife spot add <hub> <name> <category> [weight] [radius]");
        handler->PSendSysMessage(".citylife spot delete <hub> <name>");
    }

    void Manager::PrintCompat(ChatHandler* handler) const
    {
        if (!handler)
            return;

        uint32 activeModules = PlayerbotsLife::GetActiveModules();
        bool clActive = (activeModules & PlayerbotsLife::ModuleSystemFlags::MODULE_CITY_LIFE) != 0;
        bool pvpActive = (activeModules & PlayerbotsLife::ModuleSystemFlags::MODULE_PVP_LIFE) != 0;
        bool artActive = (activeModules & PlayerbotsLife::ModuleSystemFlags::MODULE_ARTISANS) != 0;

        auto snapshot = PlayerbotsLife::GetCooperationSnapshot(GameTimeSeconds());

        handler->PSendSysMessage("=== Playerbots Life Coexistence Status ===");
        handler->PSendSysMessage("Active Modules: CityLife={} | PvPLife={} | Artisans={}",
            clActive ? "ACTIVE" : "INACTIVE",
            pvpActive ? "ACTIVE" : "INACTIVE",
            artActive ? "ACTIVE" : "INACTIVE");
        handler->PSendSysMessage("Protocol Version: {} | Registry Address: {:p}",
            PlayerbotsLife::GetReservationProtocolVersion(),
            PlayerbotsLife::GetReservationRegistryAddress());
        handler->PSendSysMessage("Active Reservations: CityLife={} | PvPLife={} | Artisans={} (Total: {})",
            snapshot.CityLifeCount, snapshot.PvPLifeCount, snapshot.ArtisansCount, snapshot.TotalReserved);
        handler->PSendSysMessage("Lease States: Active={} | Provisional={} | Releasing={} (Current Gen: {})",
            snapshot.ActiveCount, snapshot.ProvisionalCount, snapshot.ReleasingCount, snapshot.CurrentGeneration);
        handler->PSendSysMessage("Stale Leases: Stale Provisional={} (oldest={}s) | Stale Releasing={} (oldest={}s)",
            snapshot.StaleProvisionalCount, snapshot.OldestProvisionalAgeSec,
            snapshot.StaleReleasingCount, snapshot.OldestReleasingAgeSec);
        handler->PSendSysMessage("Arbitration: Waiting Admissions={} | Unacked Grants={} | Active Duel Slots={}",
            snapshot.WaitingAdmissions, snapshot.UnacknowledgedGrants, snapshot.ActiveDuelSlots);
    }

    bool Manager::HandleDebugCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
            return false;

        std::string sub = Lower(args[1]);
        if (sub == "status")
        {
            PrintDebugStatus(handler);
            return true;
        }
        if (sub == "antic" && args.size() >= 4)
        {
            // Plays an antic on a resident right away, to see what it looks like.
            Player* player = ObjectAccessor::FindPlayerByName(args[2]);
            Resident* resident = player ? FindResident(player->GetGUID().GetCounter()) : nullptr;
            LifeMotion::Antic kind = LifeMotion::AnticFromName(args[3]);
            if (!resident || !ValidateResidentAction(player, *resident))
                handler->PSendSysMessage("'{}' is not a City Life resident right now.", args[2]);
            else if (kind == LifeMotion::Antic::None || kind == LifeMotion::Antic::HopDismount)
                handler->PSendSysMessage("Antics: Dance, Strafe, HopBackForth, HopSideToSide, BunnyHop.");
            else if (player->IsInCombat() || player->duel || !TryStartAntic(*resident, player, GameTimeMS(), kind))
                handler->PSendSysMessage("{} is busy.", player->GetName());
            else
                handler->PSendSysMessage("{} starts {}.", player->GetName(), LifeMotion::AnticName(kind));
            return true;
        }

        return false;
    }

    void Manager::QueueMutation(std::string const& sql, uint32 gmGuidLow, std::string const& successMsg)
    {
        if (_pendingDbWrites >= 32)
        {
            LOG_WARN("module", "[CityLife] Database write queue full; GM mutation skipped.");
            if (gmGuidLow)
            {
                if (Player* gm = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(gmGuidLow)))
                    ChatHandler(gm->GetSession()).PSendSysMessage("City Life: Edit queue full; try again shortly.");
            }
            return;
        }

        WorldDatabaseTransaction transaction = WorldDatabase.BeginTransaction();
        transaction->Append(sql);
        ++_pendingDbWrites;

        _transactionProcessor.AddCallback(WorldDatabase.AsyncCommitTransaction(transaction)).AfterComplete(
            [this, gmGuidLow, successMsg](bool success)
            {
                --_pendingDbWrites;
                if (!success)
                {
                    LOG_ERROR("module", "[CityLife] GM database mutation transaction failed.");
                    if (gmGuidLow)
                    {
                        if (Player* gm = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(gmGuidLow)))
                            ChatHandler(gm->GetSession()).PSendSysMessage("City Life: Database edit failed.");
                    }
                    return;
                }

                // Reload once the last queued edit lands, even if a config reload ran in between.
                if (_pendingDbWrites == 0 && _enable)
                    _databaseReady = VerifyDatabase() && LoadData();

                if (gmGuidLow && !successMsg.empty())
                {
                    if (Player* gm = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(gmGuidLow)))
                        ChatHandler(gm->GetSession()).PSendSysMessage("{}", successMsg);
                }
            });
    }

    bool Manager::ValidateResidentAction(Player* bot, Resident const& resident) const
    {
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            return false;

        if (resident.Token.Generation == 0)
            return false;

        if (!PlayerbotsLife::ValidateActiveLease(resident.Token))
            return false;

        if (IsHumanControlled(bot))
            return false;

        return true;
    }

    bool Manager::IsResidentInSyntheticActivity(uint32 guidLow) const
    {
        for (Resident const& resident : _residents)
        {
            if (resident.Bot.GuidLow == guidLow)
            {
                // The scripted scenes: crowd and plaza chat, inn dancing, tavern drinking, cheering duels.
                return resident.Activity == CitizenActivity::CrowdSocial ||
                       resident.Activity == CitizenActivity::PlazaSocial ||
                       resident.Activity == CitizenActivity::InnDance ||
                       resident.Activity == CitizenActivity::DrinkInTavern ||
                       resident.Activity == CitizenActivity::WatchDuels;
            }
        }
        return false;
    }

    bool Manager::HandleHubCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 2)
            return false;
        std::string operation = Lower(args[1]);
        if (operation == "list")
        {
            PrintStatus(handler);
            return true;
        }

        if (operation == "add")
        {
            if (args.size() < 7 || !handler->GetSession() || !handler->GetSession()->GetPlayer())
                return false;
            TeamSide team;
            uint32 minLevel, maxLevel, population;
            uint32 priority = 100;
            if (!ParseTeam(args[3], team) || !ParseUInt(args[4], minLevel) ||
                !ParseUInt(args[5], maxLevel) || !ParseUInt(args[6], population) ||
                (args.size() > 7 && !ParseUInt(args[7], priority)) || minLevel > maxLevel || maxLevel > 80)
            {
                handler->PSendSysMessage("City Life: Invalid faction, level, population, or priority.");
                return true;
            }

            Player* gm = handler->GetSession()->GetPlayer();
            uint32 zoneId = gm->GetZoneId();
            uint32 gmGuidLow = gm->GetGUID().GetCounter();
            std::string name = args[2];
            WorldDatabase.EscapeString(name);

            bool hasZoneId = WorldDatabase.Query("SHOW COLUMNS FROM `city_life_hub` LIKE 'zone_id'") != nullptr;
            std::string sql = hasZoneId
                ? Acore::StringFormat(
                    "INSERT INTO city_life_hub "
                    "(name,enabled,team,min_level,max_level,map_id,zone_id,default_population,priority) "
                    "VALUES ('{}',1,{},{},{},{},{},{},{}) ON DUPLICATE KEY UPDATE enabled=1,team=VALUES(team),"
                    "min_level=VALUES(min_level),max_level=VALUES(max_level),map_id=VALUES(map_id),zone_id=VALUES(zone_id),"
                    "default_population=VALUES(default_population),priority=VALUES(priority)",
                    name, static_cast<uint32>(team), minLevel, maxLevel, gm->GetMapId(), zoneId, population, priority)
                : Acore::StringFormat(
                    "INSERT INTO city_life_hub "
                    "(name,enabled,team,min_level,max_level,map_id,default_population,priority) "
                    "VALUES ('{}',1,{},{},{},{},{},{}) ON DUPLICATE KEY UPDATE enabled=1,team=VALUES(team),"
                    "min_level=VALUES(min_level),max_level=VALUES(max_level),map_id=VALUES(map_id),"
                    "default_population=VALUES(default_population),priority=VALUES(priority)",
                    name, static_cast<uint32>(team), minLevel, maxLevel, gm->GetMapId(), population, priority);

            std::string successMsg = Acore::StringFormat(
                "City Life: Saved hub '{}' (zone {}). Add spots with '.citylife spot add {} <name> <category>' before population can appear.",
                name, zoneId, name);
            QueueMutation(sql, gmGuidLow, successMsg);
            handler->PSendSysMessage("City Life: Queued hub '{}' save.", name);
            return true;
        }

        if ((operation == "enable" || operation == "disable" || operation == "population") && args.size() >= 3 &&
            !FindHub(args[2]))
        {
            handler->PSendSysMessage("City Life: Hub '{}' not found among the loaded hubs (try .citylife reload data).", args[2]);
            return true;
        }

        if ((operation == "enable" || operation == "disable") && args.size() >= 3)
        {
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            uint32 gmGuidLow = (handler->GetSession() && handler->GetSession()->GetPlayer()) ? handler->GetSession()->GetPlayer()->GetGUID().GetCounter() : 0;
            std::string sql = Acore::StringFormat("UPDATE city_life_hub SET enabled={} WHERE name='{}'",
                operation == "enable" ? 1 : 0, name);
            std::string successMsg = Acore::StringFormat("City Life: Hub '{}' {}.", name, operation == "enable" ? "enabled" : "disabled");
            QueueMutation(sql, gmGuidLow, successMsg);
            handler->PSendSysMessage("City Life: Queued hub '{}' update.", name);
            return true;
        }

        if (operation == "population" && args.size() >= 4)
        {
            uint32 population;
            if (!ParseUInt(args[3], population))
                return false;
            std::string name = args[2];
            WorldDatabase.EscapeString(name);
            uint32 gmGuidLow = (handler->GetSession() && handler->GetSession()->GetPlayer()) ? handler->GetSession()->GetPlayer()->GetGUID().GetCounter() : 0;
            std::string sql = Acore::StringFormat("UPDATE city_life_hub SET default_population={} WHERE name='{}'", population, name);
            std::string successMsg = Acore::StringFormat("City Life: Set DB population for hub '{}' to {}.", name, population);
            QueueMutation(sql, gmGuidLow, successMsg);
            handler->PSendSysMessage("City Life: Queued hub '{}' population update.", name);
            return true;
        }
        return false;
    }

    bool Manager::HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args)
    {
        if (args.size() < 3)
            return false;
        std::string operation = Lower(args[1]);
        Hub* hub = FindHub(args[2]);
        if (!hub)
        {
            handler->PSendSysMessage("City Life: Hub '{}' not found.", args[2]);
            return true;
        }

        uint32 hubId = hub->Id;
        std::string hubName = hub->Name;

        if (operation == "list")
        {
            handler->PSendSysMessage("City Life - Spot list for {}:", hubName);
            for (Spot const& spot : hub->Spots)
                handler->PSendSysMessage("#{} {} [{}] weight={} radius={} ({:.1f}, {:.1f}, {:.1f})",
                    spot.Id, spot.Name, spot.Category, spot.Weight, spot.Radius, spot.X, spot.Y, spot.Z);
            return true;
        }

        if (operation == "add")
        {
            if (args.size() < 5 || !handler->GetSession() || !handler->GetSession()->GetPlayer())
                return false;
            Player* gm = handler->GetSession()->GetPlayer();
            if (hub->MapId != gm->GetMapId())
            {
                handler->PSendSysMessage("City Life: Cannot add spot to hub '{}' (map {}) while GM is on map {}.",
                    hubName, hub->MapId, gm->GetMapId());
                return true;
            }
            if (!std::isfinite(gm->GetPositionX()) || !std::isfinite(gm->GetPositionY()) ||
                !std::isfinite(gm->GetPositionZ()) || !std::isfinite(gm->GetOrientation()))
            {
                handler->PSendSysMessage("City Life: GM coordinates are not finite.");
                return true;
            }

            uint32 weight = 100;
            float radius = 4.0f;
            if ((args.size() > 5 && !ParseUInt(args[5], weight)) ||
                (args.size() > 6 && !ParseFloat(args[6], radius)))
                return false;
            weight = std::max<uint32>(1, weight);
            radius = std::clamp(radius, 0.0f, 30.0f);

            std::string spotName = args[3];
            std::string category = Lower(args[4]); // the role checks compare lower-case categories
            WorldDatabase.EscapeString(spotName);
            WorldDatabase.EscapeString(category);
            uint32 gmGuidLow = gm->GetGUID().GetCounter();

            bool hasPoiType = WorldDatabase.Query("SHOW COLUMNS FROM `city_life_spot` LIKE 'poi_type'") != nullptr;
            std::string sql = hasPoiType
                ? Acore::StringFormat(
                    "INSERT INTO city_life_spot (hub_id,name,category,enabled,x,y,z,o,radius,weight,poi_type,district) "
                    "VALUES ({},'{}','{}',1,{},{},{},{},{},{},{},'{}') ON DUPLICATE KEY UPDATE category=VALUES(category),"
                    "enabled=1,x=VALUES(x),y=VALUES(y),z=VALUES(z),o=VALUES(o),radius=VALUES(radius),"
                    "weight=VALUES(weight),poi_type=VALUES(poi_type)", // an existing spot keeps its district
                    hubId, spotName, category, gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(),
                    gm->GetOrientation(), radius, weight, static_cast<uint32>(CategoryToPoiType(category)), category)
                : Acore::StringFormat(
                    "INSERT INTO city_life_spot (hub_id,name,category,enabled,x,y,z,o,radius,weight) "
                    "VALUES ({},'{}','{}',1,{},{},{},{},{},{}) ON DUPLICATE KEY UPDATE category=VALUES(category),"
                    "enabled=1,x=VALUES(x),y=VALUES(y),z=VALUES(z),o=VALUES(o),radius=VALUES(radius),"
                    "weight=VALUES(weight)",
                    hubId, spotName, category, gm->GetPositionX(), gm->GetPositionY(), gm->GetPositionZ(),
                    gm->GetOrientation(), radius, weight);

            std::string successMsg = Acore::StringFormat("City Life: Saved spot '{}/{}' at GM position.", hubName, spotName);
            QueueMutation(sql, gmGuidLow, successMsg);
            handler->PSendSysMessage("City Life: Queued spot '{}/{}' save.", hubName, spotName);
            return true;
        }

        if (operation == "delete" && args.size() >= 4)
        {
            std::string spotName = args[3];
            WorldDatabase.EscapeString(spotName);
            uint32 gmGuidLow = (handler->GetSession() && handler->GetSession()->GetPlayer()) ? handler->GetSession()->GetPlayer()->GetGUID().GetCounter() : 0;
            // Disabled rather than deleted: a seeded spot that is deleted comes back the next time
            // the updater re-applies city_life.sql, a disabled one stays off.
            std::string sql = Acore::StringFormat("UPDATE city_life_spot SET enabled=0 WHERE hub_id={} AND name='{}'", hubId, spotName);
            std::string successMsg = Acore::StringFormat("City Life: Removed spot '{}/{}'.", hubName, spotName);
            QueueMutation(sql, gmGuidLow, successMsg);
            handler->PSendSysMessage("City Life: Queued spot '{}/{}' deletion.", hubName, spotName);
            return true;
        }
        return false;
    }

    bool Manager::HandleCommand(ChatHandler* handler, std::string const& text)
    {
        std::vector<std::string> args = Tokenize(text);
        if (args.empty() || Lower(args[0]) == "help")
        {
            PrintHelp(handler);
            return true;
        }

        std::string command = Lower(args[0]);
        if (command == "status")
        {
            PrintStatus(handler);
            return true;
        }
        if (command == "compat")
        {
            PrintCompat(handler);
            return true;
        }
        if (command == "debug")
        {
            return HandleDebugCommand(handler, args);
        }
        if (command == "reload")
        {
            std::string sub = args.size() > 1 ? Lower(args[1]) : "all";
            if (sub != "config" && sub != "data" && sub != "all")
            {
                PrintHelp(handler);
                return true;
            }

            // LoadConfig already verifies and loads the data when the module is enabled.
            bool dataOk;
            if (sub == "data")
            {
                _databaseReady = VerifyDatabase() && LoadData();
                dataOk = _databaseReady;
            }
            else
            {
                LoadConfig();
                dataOk = _databaseReady || !_enable;
            }

            if (!dataOk)
                handler->PSendSysMessage("City Life: Reload encountered database errors. Check logs.");
            else if (sub == "config")
                handler->PSendSysMessage("City Life: Successfully reloaded configuration.");
            else if (sub == "data")
                handler->PSendSysMessage("City Life: Successfully reloaded hubs and spots data.");
            else
                handler->PSendSysMessage("City Life: Successfully reloaded configuration and data.");
            return true;
        }
        if (command == "hub" && HandleHubCommand(handler, args))
            return true;
        if (command == "spot" && HandleSpotCommand(handler, args))
            return true;

        PrintHelp(handler);
        return true;
    }
}
