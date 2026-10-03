#ifndef BOTCONFIG_H
#define BOTCONFIG_H

#include "botcommon.h"

enum SharedOwnerOptions : uint32
{
    SHARED_OWNER_ENABLE                 = 1,
    SHARED_OWNER_EQUIPMENT              = 2,
    SHARED_OWNER_ADD_OWNERS             = 3,
    SHARED_OWNER_REMOVE_OWNERS          = 4,

    MAX_SHARED_OWNER_OPTIONS
};
enum SharedOwnerOptionMask : uint32
{
    SHARED_OWNER_OPTION_MASK_ENABLE         = (1<<(SHARED_OWNER_ENABLE-1)),
    SHARED_OWNER_OPTION_MASK_EQUIPMENT      = (1<<(SHARED_OWNER_EQUIPMENT-1)),
    SHARED_OWNER_OPTION_MASK_ADD_OWNERS     = (1<<(SHARED_OWNER_ADD_OWNERS-1)),
    SHARED_OWNER_OPTION_MASK_REMOVE_OWNERS  = (1<<(SHARED_OWNER_REMOVE_OWNERS-1)),

    SHARED_OWNER_OPTION_MASK_MANAGE_OWNERS  = SHARED_OWNER_OPTION_MASK_ADD_OWNERS | SHARED_OWNER_OPTION_MASK_REMOVE_OWNERS,
    SHARED_OWNER_OPTION_MASK_ALL            = (1<<(MAX_SHARED_OWNER_OPTIONS-1)) - 1
};

class Map;

template<typename U>
using BotBrackets = std::array<U, BRACKETS_COUNT>;
using LvlBrackets = BotBrackets<uint8>;
using PctBrackets = BotBrackets<uint32>;
using ItemLvlBrackets = BotBrackets<uint32>;

class AC_GAME_API BotCfg
{
public:
    static void ReloadConfig();

    static bool IsNpcBotModEnabled();
    static bool IsNpcBotLogEnabled();
    static bool IsNpcBotDungeonFinderEnabled();
    static bool IsNpcBotDungeonFinderBotGenerationEnabled();
    static bool LimitNpcBotsInDungeons();
    static bool LimitNpcBotsInRaids();
    static bool IsNpcBotsPremadeEnabled();
    static bool DisplayEquipment();
    static bool ShowEquippedCloak();
    static bool ShowEquippedHelm();
    static bool SendEquipListItems();
    static bool IsGearBankEnabled();
    static bool IsTransmogEnabled();
    static bool MixArmorClasses();
    static bool MixWeaponClasses();
    static bool MixWeaponInventoryTypes();
    static bool TransmogUseEquipmentSlots();
    static bool IsClassEnabled(uint8 m_class);
    static bool IsWanderingClassEnabled(uint8 m_class);
    static bool EnableWanderingUntargetNpcQuestgiver();
    static bool EnableWanderingUntargetNpcFlightmaster();
    static bool HideBotSpawns();
    static bool IsEnrageOnDimissEnabled();
    static bool IsBotStatsLimitsEnabled();
    static bool IsPvPEnabled();
    static bool IsFoodInterruptedByMovement();
    static bool FilterRaces();
    static bool IsBotGenerationEnabledBGs();
    static bool IsBotLevelCappedByConfigBG();
    static bool IsBotLevelCappedByConfigBGFirstPlayer();
    static bool IsBotGenerationEnabledWorldMapId(uint32 mapId);
    static bool IsBotHKEnabled();
    static bool IsBotHKMessageEnabled();
    static bool IsBotHKAchievementsEnabled();
    static bool IsSharedOwnerOptionEnabled(SharedOwnerOptionMask options);
    static uint8 GetMaxClassBots();
    static uint8 GetMaxAccountBots();
    static uint8 GetMaxSharedOwners();
    static uint32 GetGearBankCapacity();
    static uint32 GetGearBankEquipmentSetsCount();
    static uint8 GetHealTargetIconFlags();
    static uint8 GetTankTargetIconFlags();
    static uint8 GetOffTankTargetIconFlags();
    static uint8 GetDPSTargetIconFlags();
    static uint8 GetRangedDPSTargetIconFlags();
    static uint8 GetNoDPSTargetIconFlags();
    static uint32 GetBaseUpdateDelay();
    static uint32 GetOwnershipExpireTime();
    static uint8 GetOwnershipExpireMode();
    static uint32 GetDesiredWanderingBotsCount();
    static uint32 GetBGTargetTeamPlayersCount(BattlegroundTypeId bgTypeId);
    static float GetBotHKHonorRate();
    static float GetBotStatLimitDodge();
    static float GetBotStatLimitParry();
    static float GetBotStatLimitBlock();
    static float GetBotStatLimitCrit();
    static float GetBotDamageModPhysical();
    static float GetBotDamageModSpell();
    static float GetBotHealingMod();
    static float GetBotHPMod();
    static float GetBotWandererDamageMod();
    static float GetBotWandererHealingMod();
    static float GetBotWandererHPMod();
    static float GetBotWandererSpeedMod();
    static float GetBotWandererXPGainMod();
    static PctBrackets GetBotWandererLevelBrackets();
    static uint32 GetBotWandererMaxItemLevel(uint8 level);
    static uint32 GetBotWandererKillRewardMoney();
    static uint32 GetBotWandererKillRewardItemMaxCount();
    static uint32 GetBotWandererKillRewardItemMaxQuality();
    static bool EnableWandererFreeLootSkinning();
    static bool SpawnWanderingBotsNearPlayers();
    static uint32 GetMaxWanderingBotsPerGrid();
    static uint32 GetBotWandererDespawnDelay();
    static bool IsBotActivitiesEnabled();
    static uint32 GetBotActivitiesIntervalMin();
    static uint32 GetBotActivitiesIntervalMax();
    static uint32 GetBotActivitiesRestChance();
    static uint32 GetBotActivitiesRoleplayChance();
    static bool IsBotMemoryEnabled();
    static std::string const& GetBotMemoryFile();
    static uint32 GetBotMemorySaveInterval();
    static bool IsBotGroupFinderEnabled();
    static uint32 GetBotGroupFinderInterval();
    static uint32 GetBotGroupFinderChance();
    static bool IsBotQuestHelpEnabled();
    static uint32 GetBotQuestHelpChance();
    static uint32 GetBotQuestHelpCooldown();
    static bool IsBotWorldEventsEnabled();
    static uint32 GetBotWorldEventsInterval();
    static uint32 GetBotWorldEventsMaxBots();
    static bool IsBotRaidOffersEnabled();
    static uint32 GetBotRaidOffersInterval();
    static uint32 GetBotRaidOffersSize();
    static bool IsBotGuildsEnabled();
    static std::string const& GetBotChatterLocale();
    static bool IsBotOpenAIEnabled();
    static std::string const& GetBotOpenAIApiKey();
    static std::string const& GetBotOpenAIModel();
    static std::string const& GetBotOpenAIEndpoint();
    static std::string const& GetBotOpenAIReasoningEffort();
    static std::string const& GetBotOpenAIInstructions();
    static uint32 GetBotOpenAIMaxOutputTokens();
    static uint32 GetBotOpenAITimeout();
    static uint32 GetBotOpenAIThreads();
    static uint32 GetBotOpenAIQueueSize();
    static uint32 GetBotOpenAIPlayerCooldown();
    static uint32 GetBotOpenAIChatContextSize();
    static bool IsBotOpenAIEmotesEnabled();
    static uint32 GetBotGuildChance();
    static bool IsBotChatterEnabled();
    static bool IsBotChatterHiredBotsEnabled();
    static bool IsBotChatterChannelEnabled();
    static uint32 GetBotChatterChannelCooldown();
    static uint32 GetBotChatterIdleIntervalMin();
    static uint32 GetBotChatterIdleIntervalMax();
    static uint32 GetBotChatterIdleChance();
    static uint32 GetBotChatterGreetChance();
    static uint32 GetBotChatterGreetCooldown();
    static uint32 GetBotChatterReplyChance();
    static uint32 GetBotChatterEventChance();
    static uint32 GetBotChatterEnvironmentChance();
    static uint32 GetBotDungeonMaxItemLevel(uint8 level, uint16 map_id, Difficulty map_difficulty);
    static float GetBotDamageModByClass(uint8 botclass);
    static float GetBotDamageModByLevel(uint8 botlevel);
    static float GetBotHealingModByLevel(uint8 botlevel);
    static float GetBotHPModByLevel(uint8 botlevel);
    static float GetBotMPModByLevel(uint8 botlevel);

    static uint8 GetFollowDistDefault();
    static uint32 GetEngageDelayDPSDefault();
    static uint32 GetEngageDelayHealDefault();

    static uint8 GetMaxNpcBots(uint8 level);
    static bool IsNpcBotXpReductionEnabled();
    static bool IsNpcBotXpReductionGroupOnly();
    static uint8 GetNpcBotXpReductionExtraAmount();
    static uint8 GetNpcBotXpReductionExtraStartingNumber();
    static bool IsNpcBotHonorReductionEnabled();
    static bool IsNpcBotHonorReductionGroupOnly();
    static bool GetNpcBotMoneyShareEnabled();
    static bool GetNpcBotMoneyShareGroupOnly();
    static uint8 GetNpcBotMountLevel60();
    static uint8 GetNpcBotMountLevel100();
    static int32 GetBotInfoPacketsLimit();

    static uint32 GetNpcBotCostRent(uint8 level, uint8 botclass);
    static uint32 GetNpcBotCostHire(uint8 level, uint8 botclass);
    static std::string GetNpcBotCostStr(uint8 level, uint8 botclass);

    static bool IsMapAllowedForBots(Map const* map);
    static bool IsMapIdAllowedForBots(uint32 mapId);
private:
    static uint32 _normalizedCostForLevel(uint32 cost_base, uint8 bot_class, uint8 level);
};

void AddNpcBotScripts();

#endif
