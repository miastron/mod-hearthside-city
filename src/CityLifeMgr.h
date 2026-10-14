/*
 * City Life
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CITY_LIFE_MGR_H
#define CITY_LIFE_MGR_H

#include "CityLifeTypes.h"
#include "../../mod-hearthside-life/src/LifeMedic.h"
#include "../../mod-hearthside-life/src/LifeBots.h"
#include "../../mod-hearthside-life/src/LifePresence.h"
#include "../../mod-hearthside-life/src/LifeAudit.h"
#include "AsyncCallbackProcessor.h"
#include "QueryCallback.h"
#include "Transaction.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChatHandler;
class Field;
class Player;

namespace CityLife
{
    // LifeBots::IsBotSession / IsHumanPlayer with this module's bot-account filter
    // (CityLife.Bots.AccountPrefix/AccountMin/AccountMax/AccountFilterMode).
    bool IsBotSession(Player const* player);
    bool IsHumanPlayer(Player const* player);

    // CityLife.Audit.*: Logs/citylife_audit.log by default.
    inline LifeAudit::Logger& AuditLogger()
    {
        static LifeAudit::Logger logger;
        return logger;
    }

    class Manager
    {
    public:
        static Manager& Instance();

        void LoadConfig();
        void Update(uint32 diff);
        void Shutdown();
        bool HandleCommand(ChatHandler* handler, std::string const& text);

        // Interaction & Player Event Callbacks
        void OnPlayerLogout(Player* player);
        void DisengageResident(uint32 guidLow);
        // PlayerScript/GroupScript hooks can fire on map-update threads (a bot's own AI logs it out
        // or accepts an invite from inside Player::Update), concurrently with other hooks that read
        // the resident list. They queue the GUID here; Update() disengages it on the world thread.
        void QueueDisengage(uint32 guidLow);
        // PlayerScript::OnPlayerDuelEnd, any duel won anywhere (map-update threads): queued for the
        // duel medics, handled on the world thread.
        void QueueDuelEnd(Player* winner, Player* loser);
        bool IsResident(uint32 guidLow) const;

        // Activity Helper Methods
        Spot const* FindSpot(uint32 hubId, uint32 spotId) const;
        Spot const* FindInnBedSpot(Hub const& hub) const;
        Player* FindCrowdChatPartner(uint32 hubId, Player* bot);
        // Pairs two crowd residents near `witness` (a real player): they face each other and hold the
        // pair for a while, and LifeEvents::CrowdPair tells whoever listens (mod-hearthside-chat).
        void BeginCrowdChat(Player* bot, Player* partner, Resident& resident, uint32 now, Player* witness);
        void ClearResidentChatPartner(uint32 partnerGuidLow, uint32 expectedGuidLow);
        Resident* FindResident(uint32 guidLow);
        Resident const* FindResident(uint32 guidLow) const;
        void AssignSpot(Resident& resident, Hub const& hub, Spot const& spot, bool allowStationary);
        // Points mod-playerbots' "stay" position at the resident's target (journaled for the restore).
        void SetStayPosition(Player* player, Resident& resident) const;

        // Config Accessors
        bool IsInteractionsEnabled() const { return _enableInteractions; }
        bool IsPlayerTradingEnabled() const { return _enablePlayerTrading; }
        bool IsBlockAchievementsEnabled() const { return _blockAchievements; }
        void RecordSuppressedAchievement() { ++_suppressedAchievements; }
        void RecordSuppressedCriteria() { ++_suppressedCriteria; }
        bool IsResidentInSyntheticActivity(uint32 guidLow) const;
        bool ValidateResidentAction(Player* bot, Resident const& resident) const;
        bool IsHumanControlled(Player* player) const;
        bool IsConfiguredBotAccount(uint32 accountId) const;
        LifeBots::AccountFilter const& Accounts() const { return _accounts; }

    private:
        Manager() = default;

        bool VerifyDatabase();
        bool LoadData();
        bool CheckCoexistence();

        TeamSide TeamForRace(uint8 race) const;
        Player* FindPlayer(uint32 guidLow) const;
        void RefreshAccountCache();
        bool IsCandidate(Player* player);
        // True if this bot's mod-hearthside-chat archetype is on CityLife.Bots.Archetypes
        // (or that list is empty, or the chat module is not in the build).
        bool FitsArchetype(uint32 guidLow) const { return _archetypeFilter.Fits(guidLow); }
        bool IsResidentUsable(Player* player) const;
        // Cast in a mod-hearthside-chat conversation (LifeActivity): not moved, given an antic, paired or
        // released while idle until the scene ends.
        static bool Talking(Player const* player) { return player && LifeActivity::Busy(player->GetGUID().GetRawValue()); }
        uint32 GetLiveWorldBotCount(bool forceRefresh = false) const;
        std::vector<BotCandidate> LoadCandidates(Hub const& hub, uint32 poolCount = 0);

        Hub* FindHub(uint32 hubId);
        Hub const* FindHub(uint32 hubId) const;
        Hub* FindHub(std::string const& name);
        Hub const* FindHubByZone(uint32 zoneId) const;
        bool IsStaticServiceSpot(Hub const& hub, Spot const& spot) const;
        Spot const* ChooseSpot(Hub const& hub, uint32 ignoredSpotId = 0, bool filterByDistrict = false, bool targetHubDistrict = false, Player* botForPathCheck = nullptr) const;
        bool UsesAlternatingHubRoute(std::string const& hubName) const;
        bool IsHubDistrict(std::string const& hubName, std::string const& district) const;
        CitizenRole DetermineRole(Hub const& hub, Spot const& spot, BotCandidate const& candidate) const;
        CitizenActivity ChooseInitialActivity(CitizenRole role, Spot const& spot) const;
        CitizenActivity PickNextActivity(Resident const& resident, Hub const& hub) const;
        Spot const* ChooseSpotForActivity(Hub const& hub, CitizenActivity activity, uint32 ignoredSpotId, Player* botForPathCheck = nullptr) const;
        static bool DistrictMatchesClass(std::string const& hubName, std::string const& district, uint8 botClass);

        uint32 CountResidents(uint32 hubId) const;
        // Residents of the hub that are not on their way out (bound for an inn bed, or due to leave).
        uint32 CountStaying(uint32 hubId) const;
        static bool IsLeaving(Resident const& resident, uint32 nowSec);
        // mod-playerbots' BotActiveAlone wake rule for the hub (its zone, its map, or the radius
        // around any of its spots), held for CityLife.Population.BurstGraceSeconds after the player
        // leaves. Refreshes the snapshot (at most every 3s).
        bool PlayerPresent(Hub const& hub) const;
        // Wintergrasp hubs sit in the battlefield zone; while its battle runs they take no residents.
        bool IsWintergraspWarHub(Hub const& hub) const;
        uint32 TargetPopulation(Hub const& hub) const;
        uint32 CurrentTimeMultiplier() const;
        uint32 CalculateTotalBudget(uint32 eligiblePoolCount) const;
        // While a real player is in a hub its residents come and go one at a time
        // (CityLife.Population.WatchedPaceMin/MaxSeconds); true if this hub's next change may happen now.
        bool PaceAllows(Hub const& hub, uint32 nowSec) const;
        void PaceUsed(Hub const& hub, uint32 nowSec);

        bool AddResident(Hub const& hub, uint32 poolCount = 0);
        bool AdmitResident(Hub const& hub, BotCandidate const& bot, PlayerbotsLife::LeaseToken token);
        void ProcessAdmissionGrants();
        void CancelAdmissions();
        void ReleaseResident(Resident const& resident, bool returnHome);
        void ProcessReleasingResidents(bool isShutdown = false);
        // The release itself, once the bot is ours to restore: restores its state and sends it home,
        // or walks it off when a real player can see it (LifeArrival::WalkOff).
        void FinishRelease(Player* player, Resident& resident, bool returnHome, bool allowTeleport);
        void ReleaseAll(bool returnHome);
        void QueueMutation(std::string const& sql, uint32 gmGuidLow = 0, std::string const& successMsg = "");
        void RemoveUnavailableResidents();
        void MaintainPopulation();
        void MaintainResidents();
        void RelocateResident(Resident& resident, Hub const& hub);
        // Starts the walk in of residents dropped off out of sight on arrival (every ~250 ms).
        void ProcessArrivals();
        // Keeps mod-playerbots' level brackets from re-leveling a resident (LifeBots::HoldLevels).
        void KeepResidentLevelsFixed();
        void AuditLog(std::string const& tag, std::string const& text) const;
        // Advances running antics and starts mid-air dismounts; every ~100 ms from Update.
        void UpdateAntics();
        // Healer residents who now and then heal the duelists of a duel that just finished in front
        // of them (LifeMedic.h, CityLife.DuelMedics.*). Every ~100 ms from Update.
        void UpdateDuelMedics();
        // Any random bot running somewhere within CityLife.TravelHops.Radius of a real player now and
        // then jumps a few times on the way (LifeMotion::HopOnTheWay). Every ~250 ms from Update.
        void UpdateTravelHops();
        // Starts `kind` on an idle resident; with None, rolls CityLife.Antics.ChancePerMinute and picks one.
        bool TryStartAntic(Resident& resident, Player* player, uint32 nowMs, LifeMotion::Antic kind = LifeMotion::Antic::None);
        void SnapTargetToGround(Player* player, Resident& resident, Hub const& hub, Spot const& spot) const;

        void ApplyCityStrategies(Player* player, Resident& resident) const;
        void RestoreResidentState(Player* player, Resident const& resident) const;

        // Command Handlers
        void PrintStatus(ChatHandler* handler) const;
        void PrintCompat(ChatHandler* handler) const;
        void PrintHelp(ChatHandler* handler) const;
        void PrintDebugStatus(ChatHandler* handler) const;
        bool HandleHubCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleSpotCommand(ChatHandler* handler, std::vector<std::string> const& args);
        bool HandleDebugCommand(ChatHandler* handler, std::vector<std::string> const& args);

        bool _enable = true;
        bool _databaseReady = false;
        bool _configLoaded = false;
        bool _debug = false;
        uint32 _startupDelaySeconds = 90;
        uint32 _tickSeconds = 10;
        uint32 _startupElapsedMs = 0;
        uint32 _timerMs = 0;

        // Population Budget & Demand Profiles
        bool _budgetPercentage = true; // CityLife.Population.BudgetMode: percentage, or count (fixed)
        uint32 _fixedCount = 100;
        uint32 _borrowPercent = 10;
        uint32 _maxTotalPopulation = 120;
        DemandMode _demandMode = DemandMode::Hybrid;
        DistributionMode _distributionMode = DistributionMode::Scaled;
        uint32 _baselinePercent = 50; // the floor an empty hub keeps (hybrid)
        uint32 _burstGraceSeconds = 180;
        uint32 _watchedPaceMinSeconds = 10;
        uint32 _watchedPaceMaxSeconds = 40;
        std::unordered_map<uint32, uint32> _hubNextChangeAt; // hub id -> game time its next watched change may happen
        mutable std::unordered_map<uint32, uint32> _hubLastPresentAt; // hub id -> game time a player last woke it
        mutable LifePresence::Snapshot _presence;
        uint32 _maxChangesPerTick = 3;
        // Releases have their own allowance, MaxChangesPerTick a second (ProcessReleasingResidents).
        uint32 _releasesThisSecond = 0;
        uint32 _releaseSecond = 0;

        // Candidate Filters & Safety
        bool _usePlayerbotConfig = true;
        std::string _botAccountPrefix = "auto";
        uint32 _botAccountMin = 0;
        uint32 _botAccountMax = 0;
        bool _accountsRequireBoth = false; // CityLife.Bots.AccountFilterMode = all
        LifeBots::AccountFilter _accounts;
        uint32 _botQueryLimit = 500;
        LifeBots::ArchetypeFilter _archetypeFilter; // CityLife.Bots.Archetypes
        bool _skipGroupedBots = true;
        bool _respectPlayerbotActivity = false;
        bool _returnBots = true;

        // Time Scaling
        bool _timeScaling = true;
        uint32 _morningMultiplier = 60;
        uint32 _dayMultiplier = 80;
        uint32 _eveningMultiplier = 100;
        uint32 _nightMultiplier = 25;

        // Behavior & Relocation
        bool _roaming = true;
        uint32 _relocateMinSeconds = 180;
        uint32 _relocateMaxSeconds = 600;
        uint32 _leashRadius = 35;
        uint32 _emoteMinSeconds = 120;
        uint32 _emoteMaxSeconds = 360;
        uint32 _serviceSpotWeightMultiplier = 250;
        uint32 _staticServiceChance = 80;
        std::unordered_set<std::string> _staticServiceHubs;
        std::unordered_set<std::string> _staticServiceCategories;
        std::string _nonCombatStrategies = "+stay,-follow,-passive,-grind,-rpg,-new rpg,-travel,-duel,-pvp";

        // Activity Switches
        bool _actLeaveInn = true;
        bool _actReturnToInn = true;
        bool _actRoaming = true;
        bool _actWallWalking = true;
        bool _actTavernSitting = true;
        bool _actTavernDrinking = true;
        bool _actRandomEmotes = true;
        bool _actAuctionHouse = true;
        bool _actMailbox = true;
        bool _actTrainer = true;
        bool _actFoodVendor = true;
        bool _actDuelWatching = true;
        bool _actFishing = true;
        bool _actInnDancing = true;
        bool _actPlazaSocial = true;
        bool _actCrowdScenes = true;
        bool _actMountedTravel = true;

        // Living Crowd Settings
        bool _crowdEnable = true;
        float _crowdPlayerRadius = 60.0f;
        uint32 _crowdChatCooldownSec = 90;
        uint32 _crowdAfkMinSec = 60;
        uint32 _crowdAfkMaxSec = 180;
        bool _activitiesAfkEnable = true;

        // Duel spectators
        float _spectatorWatchRadius = 40.0f;

        // Idle antics
        LifeMotion::AnticConfig _antics;
        uint32 _anticTimerMs = 0;

        // Duel medics
        LifeMedic::Config _medics;
        std::mutex _duelEndMutex;
        std::vector<LifeMedic::DuelEnd> _duelEnds;
        std::vector<LifeMedic::PendingHeal> _pendingHeals;

        // Audit log
        bool _auditEnable = true;
        std::string _auditFilePath = "Logs/citylife_audit.log";

        // Hops on the way
        bool _travelHops = true;
        float _travelHopRadius = 80.0f;
        uint32 _travelHopMinSeconds = 15;
        uint32 _travelHopMaxSeconds = 45;
        uint32 _travelHopMaxJumps = 5;
        uint32 _travelHopTimerMs = 0;
        uint32 _travelHopsDone = 0;
        uint32 _travelHopPruneMs = 0;
        struct TravelHopState
        {
            uint32 NextMs = 0;
            uint32 SeenMs = 0;
            uint8 Left = 0; // jumps left in the burst under way
        };
        std::unordered_map<uint32, TravelHopState> _travelHopStates;

        // Movement & Sink Settings
        uint32 _mountBetweenPoiChance = 90;
        bool _sinkForensics = true;

        // Chat & Interaction Settings
        bool _enableInteractions = false;
        bool _enableBuyFood = false;
        bool _enablePlayerTrading = false;
        bool _blockAchievements = false;
        std::atomic<uint32> _suppressedAchievements{0}; // bumped from map-update threads
        std::atomic<uint32> _suppressedCriteria{0};

        // Advanced Micro-Behaviors (Ported from mod-city-bots)
        bool _alternatingDistricts = true;
        bool _waterAvoidance = true;

        // Configurable Activity Weights (Ported from mod-city-bots)
        uint32 _weightWanderDistrict = 30;
        uint32 _weightSitInTavern = 20;
        uint32 _weightRandomEmote = 15;
        uint32 _weightVisitAuctionHouse = 10;
        uint32 _weightVisitMailbox = 10;
        uint32 _weightBuyFood = 10;
        uint32 _weightVisitTrainer = 8;
        uint32 _weightDrinkInTavern = 10;
        uint32 _weightWalkCityWalls = 10;
        uint32 _weightFishAtDocks = 5;
        uint32 _weightReturnToInn = 15;

        std::vector<Hub> _hubs;
        struct ReleasingResident
        {
            Resident Actor;
            bool ReturnHome = false;
            uint32 ReleaseStartedAt = 0;
        };
        std::vector<ReleasingResident> _releasingResidents;
        std::vector<Resident> _residents;
        std::unordered_map<uint32, uint32> _hubResidentCounts;
        void ProcessQueuedDisengages();
        std::mutex _queuedDisengageMutex;
        std::vector<uint32> _queuedDisengages;
        mutable uint32 _cachedLiveBotCount = 0;
        mutable uint32 _lastCensusTimeMs = 0;
        mutable std::vector<uint32> _cachedOnlineGuids;
        mutable uint32 _lastCandidateScanMs = 0;
        uint64 _nextAssignmentId = 1;
        QueryCallbackProcessor _queryProcessor;
        AsyncCallbackProcessor<TransactionCallback> _transactionProcessor;
        uint32 _pendingDbWrites = 0;
        uint32 _releasingTimerMs = 0;
        bool _releasingDirty = false;
        uint64 _dataGeneration = 0;
        struct PendingAdmission
        {
            uint64 RequestId = 0;
            uint32 HubId = 0;
            uint32 Deadline = 0;
            BotCandidate Bot;
            PlayerbotsLife::LeaseToken Token;
        };
        std::vector<PendingAdmission> _pendingAdmissions;
        std::unordered_map<uint32, uint32> _hubTargets;
        uint32 _changesThisTick = 0;
    };
}

#define sCityLifeMgr CityLife::Manager::Instance()
#define sCityLifeAudit CityLife::AuditLogger()

#endif
