#include "bot_ai.h"
#include "botactivity.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "botmemory.h"
#include "bottext.h"
#include "botworldevents.h"
#include "CellImpl.h"
#include "Containers.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

/*
NpcBot Activities, see botactivity.h
*/

namespace
{
    constexpr float ACTIVITY_SAFE_RADIUS = 30.0f;
    constexpr float ROLEPLAY_RECRUIT_RADIUS = 40.0f;
    constexpr float ROLEPLAY_CAMPFIRE_SEARCH_RADIUS = 20.0f;
    constexpr float ROLEPLAY_CIRCLE_RADIUS = 2.5f;
    constexpr uint32 ROLEPLAY_MAX_GUESTS = 4;
    constexpr uint32 CAMPFIRE_FOCUS_ID = 4;         // SpellFocusObject.dbc: Campfire
    constexpr uint32 GO_BASIC_CAMPFIRE = 29784;     // spell 818 Basic Campfire
    constexpr uint32 STORIES_COUNT = 50;
    constexpr uint32 STORY_TEXT_FIRST = 71101;     // lines 71101+2k, reactions 71102+2k
    constexpr uint32 HOLD_POSITION_TIME = 5000;     // refreshed every update, see bot_ai::HoldPosition()
    constexpr float STORY_MAX_PLAYER_DISTANCE = 30.0f;
    constexpr float STORY_START_MAX_DISTANCE = 20.0f;
    constexpr float HIRED_MAX_MASTER_DISTANCE = 20.0f;
    constexpr float STORY_SEAT_DISTANCE = 2.2f;
    constexpr uint32 STORY_MAX_TIME = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 STORY_ANSWER_TIMEOUT = 45 * IN_MILLISECONDS;
    constexpr uint32 EVENT_MAX_TIME = 15 * MINUTE * IN_MILLISECONDS;
    constexpr float EVENT_RALLY_SPREAD = 5.0f;
    constexpr float EVENT_TARGET_SPREAD = 8.0f;
    constexpr uint32 EVENT_REPATH_DELAY = 3000;
    constexpr float COMPANION_MAX_DISTANCE = 150.0f;
    constexpr float COMPANION_FOLLOW_DISTANCE = 3.0f;
    constexpr uint32 COMPANION_REPATH_DELAY = 1500;
    constexpr float GATHER_NODE_SEARCH_RADIUS = 40.0f;
    constexpr float FISHING_MIN_DISTANCE = 5.0f;
    constexpr float FISHING_MAX_DISTANCE = 15.0f;
    constexpr uint32 GATHER_MAX_TRAVEL_TIME = 40 * IN_MILLISECONDS;
    constexpr uint32 ITEM_FISHING_POLE = 6256;
    constexpr uint32 GO_FISHING_BOBBER = 35591;
    constexpr uint32 VANITY_PET_CHECK_DELAY = 5 * IN_MILLISECONDS;
    constexpr uint32 VANITY_PET_LIFETIME = 10 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 VANITY_PET_CHANCE = 30;        // % of bots having one
    constexpr uint32 RARE_MOUNT_CHANCE = 25;        // % of bots riding a rare mount

    // companions of the vanity pet items
    constexpr std::array VanityPets =
    {
        7380u, 7381u, 7382u, 7383u, 7384u, 7385u, 7386u, 7387u, 7389u, 7390u, 7391u, 7395u, 7543u, 7544u, 7545u,
        7547u, 7549u, 7550u, 7553u, 7554u, 7555u, 7560u, 7561u, 7562u, 7565u, 7567u, 2671u, 9656u, 9657u, 9662u,
        10259u, 10598u, 12419u, 14421u
    };

    // rare and prestigious ground mounts any faction may ride, then alliance and horde only ones
    constexpr std::array RareMountsAny =
    {
        17481u, 24242u, 24252u, 36702u, 41252u, 43688u, 49322u, 43900u, 46628u
    };
    constexpr std::array RareMountsAlliance = { 60114u, 61425u, 59785u, 22719u, 23510u, 16056u, 17229u, 63232u };
    constexpr std::array RareMountsHorde = { 60116u, 61447u, 59788u, 22718u, 23509u, 16081u, 63640u };

    // a herb or an ore vein, by the skill its lock needs
    bool GetNodeGatherKind(GameObject const* go, BotGatherKind& kind)
    {
        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST || !go->isSpawned())
            return false;

        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock)
            return false;

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        {
            if (lock->Type[i] != LOCK_KEY_SKILL)
                continue;
            if (lock->Index[i] == LOCKTYPE_MINING)
                kind = BOT_GATHER_MINING;
            else if (lock->Index[i] == LOCKTYPE_HERBALISM)
                kind = BOT_GATHER_HERBALISM;
            else
                continue;
            return true;
        }
        return false;
    }

    uint8 CountTextSlots(uint32 textId)
    {
        GossipText const* text = sObjectMgr->GetGossipText(textId);
        if (!text)
            return 0;

        uint8 count = 0;
        while (count < MAX_GOSSIP_TEXT_OPTIONS && !text->Options[count].Text_0.empty())
            ++count;
        return count;
    }

    // hostile creatures, critters aside
    struct HostileCreatureCheck
    {
        HostileCreatureCheck(Creature const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && !creature->IsCritter() && _bot->IsHostileTo(creature) &&
                _bot->IsWithinDistInMap(creature, _range);
        }

    private:
        Creature const* _bot;
        float _range;
    };

    // free wandering bots doing nothing special
    struct IdleWandererCheck
    {
        IdleWandererCheck(Creature const* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature) const
        {
            if (creature == _bot || !creature->IsNPCBot() || !creature->IsAlive() || creature->IsInCombat())
                return false;
            bot_ai* ai = creature->GetBotAI();
            return ai && ai->IsWanderer() && ai->IAmFree() && !ai->IsDuringTeleport() &&
                ai->GetActivity().GetMode() == BOT_ACTIVITY_ACTIVE && _bot->IsFriendlyTo(creature) &&
                _bot->IsWithinDistInMap(creature, _range);
        }

    private:
        Creature const* _bot;
        float _range;
    };
}

BotActivity::BotActivity(bot_ai* ai, Creature* bot) : _ai(ai), _me(bot), _mode(BOT_ACTIVITY_ACTIVE),
    _decisionTimer(0), _modeTimer(0), _requested(false), _seated(false), _phase(ROLEPLAY_GATHER), _phaseTimer(0),
    _story(0), _storyLine(0), _storiesLeft(0), _storyLength(0), _storyLinesTold(0), _storyWaiting(false),
    _storyEnding(false), _storyTimer(0), _eventId(0), _eventAttack(false), _eventMoveTimer(0),
    _companionMounted(false), _gatherKind(BOT_GATHER_FISHING), _gatherWorking(false), _gatherTimer(0),
    _gatherOldItem(0), _vanityPetTimer(urand(1000, 5000))
{
}

void BotActivity::Update(uint32 diff)
{
    UpdateVanityPet(diff);

    // world events run without the activities config, a hired bot or a teleport ends them
    if (_mode == BOT_ACTIVITY_EVENT || _mode == BOT_ACTIVITY_COMPANION)
    {
        if (!_me->IsInWorld() || !_ai->IsWanderer() || !_ai->IAmFree() || _ai->IsDuringTeleport())
            Stop();
        else if (_mode == BOT_ACTIVITY_EVENT)
            UpdateEvent(diff);
        else
            UpdateCompanion(diff);
        return;
    }

    // activities players asked for work without the autonomous ones
    if (!BotCfg::IsBotActivitiesEnabled() && !_requested)
    {
        if (_mode != BOT_ACTIVITY_ACTIVE)
            Stop();
        return;
    }

    if (_mode != BOT_ACTIVITY_ACTIVE)
    {
        if (!IsActivityValid())
        {
            Stop();
            return;
        }

        _ai->HoldPosition(HOLD_POSITION_TIME);

        if (_mode == BOT_ACTIVITY_REST)
            UpdateRest(diff);
        else if (_mode == BOT_ACTIVITY_GATHER)
            UpdateGather(diff);
        else if (_mode == BOT_ACTIVITY_STORY)
            UpdateStory(diff);
        else if (_host == _me->GetGUID())
            UpdateRoleplayHost(diff);
        else if (!UpdateRoleplayGuest())
            Stop();
        return;
    }

    // hired bots never change their activity on their own
    if (!BotCfg::IsBotActivitiesEnabled() || !_ai->IsWanderer() || !_ai->IAmFree())
        return;

    if (!_decisionTimer)
    {
        uint32 interval = urand(BotCfg::GetBotActivitiesIntervalMin(), BotCfg::GetBotActivitiesIntervalMax());
        _decisionTimer = interval * IN_MILLISECONDS;
    }

    if (_decisionTimer > diff)
    {
        _decisionTimer -= diff;
        return;
    }

    _decisionTimer = 0;
    if (CanStartActivity())
        ChooseActivity();
}

void BotActivity::ChooseActivity()
{
    uint32 roll = urand(0, 99);
    uint32 roleplayChance = BotCfg::GetBotActivitiesRoleplayChance();
    uint32 restChance = BotCfg::GetBotActivitiesRestChance();
    // tired bots rather sit down
    if (_ai->GetChatter().IsTired())
        restChance = std::min<uint32>(restChance * 3, 100 - roleplayChance);

    uint32 gatherChance = BotCfg::GetBotActivitiesGatherChance();
    if (roll < roleplayChance)
    {
        if (!StartRoleplay())
            StartRest();
    }
    else if (roll < roleplayChance + restChance)
        StartRest();
    else if (roll < roleplayChance + restChance + gatherChance)
        StartGather();
}

bool BotActivity::CanStartActivity() const
{
    return _me->IsInWorld() && _me->IsAlive() && !_me->IsInCombat() && _ai->IsWanderer() && _ai->IAmFree() &&
        !_ai->IsDuringTeleport() && !_ai->GetBG() && _me->GetMap()->GetEntry()->IsContinent() && !_me->IsInWater() &&
        !_me->IsFlying() && _me->getAttackers().empty();
}

bool BotActivity::IsSafeSpot() const
{
    Creature* hostile = nullptr;
    HostileCreatureCheck check(_me, ACTIVITY_SAFE_RADIUS);
    Acore::CreatureSearcher<HostileCreatureCheck> searcher(_me, hostile, check);
    Cell::VisitObjects(_me, searcher, ACTIVITY_SAFE_RADIUS);
    return !hostile;
}

// fights, leaving the world or being hired end any activity, an activity asked for also ends when the owner of
// a hired bot moves on
bool BotActivity::IsActivityValid() const
{
    if (!_me->IsInWorld() || !_me->IsAlive() || _me->IsInCombat() || _ai->IsDuringTeleport())
        return false;

    if (!_requested)
        return _ai->IsWanderer() && _ai->IAmFree();

    if (!_ai->IAmFree())
    {
        Player const* master = _ai->GetBotOwner();
        if (!master || !master->IsInWorld() || master->GetMap() != _me->GetMap() || master->IsInCombat() ||
            !_me->IsWithinDistInMap(master, HIRED_MAX_MASTER_DISTANCE))
            return false;
    }

    return true;
}

void BotActivity::Stop()
{
    if (_mode == BOT_ACTIVITY_ROLEPLAY && _host == _me->GetGUID())
        EndRoleplay();
    else if (!_campfire.IsEmpty())
        if (GameObject* fire = ObjectAccessor::GetGameObject(*_me, _campfire))
            fire->Delete();

    if (_mode == BOT_ACTIVITY_GATHER)
        EndGather();
    if (IsSeated() || _seated)
        StandUp();

    _mode = BOT_ACTIVITY_ACTIVE;
    _eventId = 0;
    _eventAttack = false;
    _eventMoveTimer = 0;
    _companionLeader.Clear();
    _companionMounted = false;
    _modeTimer = 0;
    _decisionTimer = 0;
    _requested = false;
    _host.Clear();
    _guests.clear();
    _campfire.Clear();
    _seated = false;
    _storyPlayer.Clear();
    _storyTranscript.clear();
    _storyWaiting = false;
    _storyEnding = false;
    _storyTimer = 0;
}

bool BotActivity::JoinEvent(uint32 eventId, Position const& rally)
{
    if (!_me->IsInWorld() || !_me->IsAlive() || !_ai->IsWanderer() || !_ai->IAmFree() || _ai->IsDuringTeleport() ||
        _ai->GetBG())
        return false;

    Stop();
    _mode = BOT_ACTIVITY_EVENT;
    _modeTimer = EVENT_MAX_TIME;
    _eventId = eventId;
    _eventAttack = false;
    _eventMoveTimer = 0;

    // everyone gets an own spot around the rally point
    float angle = rand_norm() * 2.0f * float(M_PI);
    float dist = frand(1.0f, EVENT_RALLY_SPREAD);
    _eventRally.Relocate(rally.GetPositionX() + dist * std::cos(angle), rally.GetPositionY() + dist * std::sin(angle),
        rally.GetPositionZ());
    _me->UpdateAllowedPositionZ(_eventRally.m_positionX, _eventRally.m_positionY, _eventRally.m_positionZ);
    _eventRally.SetOrientation(_eventRally.GetAbsoluteAngle(&rally));
    return true;
}

void BotActivity::StartEventAttack(Position const& target)
{
    if (_mode != BOT_ACTIVITY_EVENT)
        return;

    float angle = rand_norm() * 2.0f * float(M_PI);
    float dist = frand(0.0f, EVENT_TARGET_SPREAD);
    _eventTarget.Relocate(target.GetPositionX() + dist * std::cos(angle),
        target.GetPositionY() + dist * std::sin(angle), target.GetPositionZ());
    _me->UpdateAllowedPositionZ(_eventTarget.m_positionX, _eventTarget.m_positionY, _eventTarget.m_positionZ);
    _eventAttack = true;
    _eventMoveTimer = 0;
}

// walks to the rally point or the target, fighting is left to the bot AI
void BotActivity::UpdateEvent(uint32 diff)
{
    if (_modeTimer <= diff)
    {
        Stop();
        return;
    }
    _modeTimer -= diff;

    if (!_me->IsAlive() || _me->IsInCombat() || !_me->getAttackers().empty())
        return;

    // no wandering meanwhile, short enough to keep looking for enemies around (see bot_ai::_getTargets())
    _ai->HoldPosition(HOLD_POSITION_TIME);

    Position const& dest = _eventAttack ? _eventTarget : _eventRally;
    if (_me->GetExactDist2d(dest) < 2.0f)
    {
        if (!_me->isMoving() && !_eventAttack && std::abs(_me->GetOrientation() - dest.GetOrientation()) > 0.1f)
            _me->SetFacingTo(dest.GetOrientation());
        return;
    }

    if (_eventMoveTimer > diff)
    {
        _eventMoveTimer -= diff;
        return;
    }
    _eventMoveTimer = EVENT_REPATH_DELAY;

    if (!_me->isMoving() || _eventAttack)
        _ai->BotMovement(BOT_MOVE_POINT, &dest, nullptr, true);
}

bool BotActivity::FollowCompanion(Creature const* leader, uint32 duration)
{
    if (!CanStartActivity() || _mode != BOT_ACTIVITY_ACTIVE || leader == _me)
        return false;

    _mode = BOT_ACTIVITY_COMPANION;
    _modeTimer = duration;
    _companionLeader = leader->GetGUID();
    _companionMounted = false;
    _eventMoveTimer = 0;
    return true;
}

// follows the friend at a short distance, sits down when the friend does, joins the friend's fights
void BotActivity::UpdateCompanion(uint32 diff)
{
    Creature* leader = ObjectAccessor::GetCreature(*_me, _companionLeader);
    bool leaderValid = leader && leader->IsAlive() && leader->GetBotAI() && leader->GetBotAI()->IAmFree() &&
        !leader->GetBotAI()->IsDuringTeleport() && _me->IsWithinDistInMap(leader, COMPANION_MAX_DISTANCE);

    if (_modeTimer <= diff || !leaderValid)
    {
        if (leaderValid && _me->IsAlive() && !_me->IsInCombat())
            _ai->GetChatter().Announce(BOT_TEXT_BOND_TRAVEL_END, {}, CHAT_MSG_MONSTER_SAY);
        Stop();
        return;
    }
    _modeTimer -= diff;

    if (!_me->IsAlive() || _me->IsInCombat() || !_me->getAttackers().empty())
        return;

    if (leader->IsInCombat())
    {
        if (Unit* victim = leader->GetVictim(); victim && _me->IsValidAttackTarget(victim))
            _me->Attack(victim, !_ai->HasRole(BOT_ROLE_RANGED));
        return;
    }

    _ai->HoldPosition(HOLD_POSITION_TIME);
    _companionMounted = leader->IsMounted();

    if (_me->GetExactDist2d(leader) <= COMPANION_FOLLOW_DISTANCE + 1.5f)
    {
        bool leaderSeated = leader->GetBotAI()->GetActivity().IsSeated();
        if (!_me->isMoving())
        {
            if (leaderSeated && !_seated)
            {
                _me->SetFacingToObject(leader);
                SitDown();
                _seated = true;
            }
            else if (!leaderSeated && _seated)
            {
                StandUp();
                _seated = false;
            }
        }
        return;
    }

    if (_seated)
    {
        StandUp();
        _seated = false;
    }

    if (_eventMoveTimer > diff)
    {
        _eventMoveTimer -= diff;
        return;
    }
    _eventMoveTimer = COMPANION_REPATH_DELAY;

    // a spot behind and beside the friend
    float angle = leader->GetOrientation() + float(M_PI) + frand(-0.7f, 0.7f);
    Position pos(leader->GetPositionX() + COMPANION_FOLLOW_DISTANCE * std::cos(angle),
        leader->GetPositionY() + COMPANION_FOLLOW_DISTANCE * std::sin(angle), leader->GetPositionZ());
    _me->UpdateAllowedPositionZ(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
    _ai->BotMovement(BOT_MOVE_POINT, &pos, nullptr, true);
}

// stable per bot (persona): the same bot keeps its pet and mount after a restart
uint32 BotActivity::GetLooksSeed() const
{
    return BotMemory::GetSocialKey(_me->GetEntry()) * 2654435761u;
}

uint32 BotActivity::GetRareMountSpell() const
{
    uint32 seed = GetLooksSeed();
    if (!_ai->IsWanderer() || !_ai->IAmFree() || (seed >> 8) % 100 >= RARE_MOUNT_CHANCE)
        return 0;

    std::vector<uint32> mounts(RareMountsAny.begin(), RareMountsAny.end());
    switch (BotDataMgr::GetTeamIdForFaction(_me->GetFaction()))
    {
        case TEAM_ALLIANCE: mounts.insert(mounts.end(), RareMountsAlliance.begin(), RareMountsAlliance.end()); break;
        case TEAM_HORDE:    mounts.insert(mounts.end(), RareMountsHorde.begin(), RareMountsHorde.end());       break;
        default:                                                                                              break;
    }
    uint32 spellId = mounts[(seed >> 16) % mounts.size()];
    return sSpellMgr->GetSpellInfo(spellId) ? spellId : 0;
}

Creature* BotActivity::GetVanityPet() const
{
    return _vanityPet.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*_me, _vanityPet);
}

// free wandering bots with a pet have it following them, out of fights and instances it stays away
void BotActivity::UpdateVanityPet(uint32 diff)
{
    if (_vanityPetTimer > diff)
    {
        _vanityPetTimer -= diff;
        return;
    }
    _vanityPetTimer = VANITY_PET_CHECK_DELAY;

    uint32 seed = GetLooksSeed();
    bool wanted = BotCfg::IsBotVanityPetsEnabled() && (seed >> 4) % 100 < VANITY_PET_CHANCE && _me->IsInWorld() &&
        _me->IsAlive() && _ai->IsWanderer() && _ai->IAmFree() && !_ai->IsDuringTeleport() &&
        _me->GetMap()->GetEntry()->IsContinent() && !_me->IsInCombat();

    Creature* pet = GetVanityPet();
    if (!wanted)
    {
        if (pet || !_vanityPet.IsEmpty())
            DespawnVanityPet();
        return;
    }

    if (pet)
    {
        // keeps up with its owner, far behind it comes back
        if (!pet->IsWithinDistInMap(_me, 40.0f))
            pet->NearTeleportTo(_me->GetPositionX(), _me->GetPositionY(), _me->GetPositionZ(), _me->GetOrientation());
        return;
    }

    uint32 entry = VanityPets[(seed >> 12) % VanityPets.size()];
    if (!sObjectMgr->GetCreatureTemplate(entry))
        return;

    Position pos = _me->GetNearPosition(1.5f, float(M_PI) * 0.75f);
    // limited lifetime: a pet left behind by a despawned owner goes away on its own, a new one comes
    TempSummon* summon = _me->SummonCreature(entry, pos, TEMPSUMMON_TIMED_DESPAWN, VANITY_PET_LIFETIME);
    if (!summon)
        return;

    summon->SetFaction(_me->GetFaction());
    summon->SetReactState(REACT_PASSIVE);
    summon->SetUnitFlag(UnitFlags(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC));
    summon->SetOwnerGUID(_me->GetGUID());
    summon->GetMotionMaster()->MoveFollow(_me, 1.5f, float(M_PI) * 0.75f);
    _vanityPet = summon->GetGUID();
}

void BotActivity::DespawnVanityPet()
{
    if (Creature* pet = GetVanityPet())
        pet->DespawnOrUnsummon();
    _vanityPet.Clear();
}

bool BotActivity::StartGather()
{
    if (!IsSafeSpot())
        return false;

    // a node nearby, otherwise water to fish in
    return (roll_chance_i(50) && StartNodeGathering()) || StartFishing() || StartNodeGathering();
}

bool BotActivity::StartNodeGathering()
{
    std::list<GameObject*> nodes;
    Acore::AllWorldObjectsInRange check(_me, GATHER_NODE_SEARCH_RADIUS);
    Acore::GameObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(_me, nodes, check);
    Cell::VisitObjects(_me, searcher, GATHER_NODE_SEARCH_RADIUS);

    GameObject* nearest = nullptr;
    BotGatherKind kind = BOT_GATHER_MINING;
    for (GameObject* go : nodes)
    {
        BotGatherKind nodeKind;
        if (GetNodeGatherKind(go, nodeKind) && (!nearest || _me->GetExactDist2d(go) < _me->GetExactDist2d(nearest)))
        {
            nearest = go;
            kind = nodeKind;
        }
    }
    if (!nearest)
        return false;

    _mode = BOT_ACTIVITY_GATHER;
    _modeTimer = GATHER_MAX_TRAVEL_TIME;
    _gatherKind = kind;
    _gatherNode = nearest->GetGUID();
    _gatherWorking = false;
    _gatherFacing.Relocate(nearest);

    // right in front of the node
    float angle = nearest->GetAbsoluteAngle(_me);
    _gatherSpot.Relocate(nearest->GetPositionX() + 1.5f * std::cos(angle),
        nearest->GetPositionY() + 1.5f * std::sin(angle), nearest->GetPositionZ());
    _me->UpdateAllowedPositionZ(_gatherSpot.m_positionX, _gatherSpot.m_positionY, _gatherSpot.m_positionZ);
    return true;
}

// water a few yards away from the shore the bot stands on
bool BotActivity::StartFishing()
{
    if (_me->IsInWater())
        return false;

    Map* map = _me->GetMap();
    float baseAngle = rand_norm() * 2.0f * float(M_PI);
    for (uint8 i = 0; i < 12; ++i)
    {
        float angle = baseAngle + i * float(M_PI) / 6.0f;
        for (float dist = FISHING_MIN_DISTANCE; dist <= FISHING_MAX_DISTANCE; dist += 5.0f)
        {
            float x = _me->GetPositionX() + dist * std::cos(angle);
            float y = _me->GetPositionY() + dist * std::sin(angle);
            float ground = map->GetHeight(_me->GetPhaseMask(), x, y, _me->GetPositionZ() + 5.0f);
            float water = map->GetWaterLevel(x, y);
            if (ground <= INVALID_HEIGHT || water <= INVALID_HEIGHT || water < ground + 1.0f ||
                std::abs(water - _me->GetPositionZ()) > 6.0f)
                continue;

            _mode = BOT_ACTIVITY_GATHER;
            _modeTimer = urand(60, 180) * IN_MILLISECONDS;
            _gatherKind = BOT_GATHER_FISHING;
            _gatherSpot.Relocate(_me);
            _gatherFacing.Relocate(x, y, water);
            _gatherWorking = false;
            return true;
        }
    }
    return false;
}

void BotActivity::UpdateGather(uint32 diff)
{
    if (_modeTimer <= diff)
    {
        Stop();
        return;
    }
    _modeTimer -= diff;

    if (!_gatherWorking)
    {
        if (_gatherKind != BOT_GATHER_FISHING)
        {
            GameObject const* node = ObjectAccessor::GetGameObject(*_me, _gatherNode);
            if (!node || !node->isSpawned())
            {
                Stop();
                return;
            }
        }

        if (_me->GetExactDist2d(_gatherSpot) > 1.5f)
        {
            if (!_me->isMoving())
            {
                if (_me->IsMounted())
                    _me->RemoveAurasByType(SPELL_AURA_MOUNTED);
                _ai->BotMovement(BOT_MOVE_POINT, &_gatherSpot, nullptr, true);
            }
            return;
        }
        if (_me->isMoving())
            return;

        _me->SetFacingTo(_me->GetAbsoluteAngle(&_gatherFacing));
        _gatherWorking = true;
        switch (_gatherKind)
        {
            case BOT_GATHER_FISHING:
            {
                // a fishing pole in hand and the bobber in the water
                _gatherOldItem = _me->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID);
                _me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID, ITEM_FISHING_POLE);
                _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_FISHING);
                if (GameObject* bobber = _me->SummonGameObject(GO_FISHING_BOBBER, _gatherFacing.GetPositionX(),
                    _gatherFacing.GetPositionY(), _gatherFacing.GetPositionZ(), _me->GetOrientation(), 0.0f, 0.0f,
                    0.0f, 0.0f, _modeTimer / IN_MILLISECONDS + 5))
                    _gatherBobber = bobber->GetGUID();
                _gatherTimer = urand(15, 40) * IN_MILLISECONDS;
                break;
            }
            case BOT_GATHER_MINING:
                _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_WORK_MINING);
                _gatherTimer = urand(6, 10) * IN_MILLISECONDS;
                _modeTimer = _gatherTimer + 1000;
                break;
            default:
                _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_LOOT);
                _gatherTimer = urand(4, 8) * IN_MILLISECONDS;
                _modeTimer = _gatherTimer + 1000;
                break;
        }
        return;
    }

    if (_gatherTimer > diff)
    {
        _gatherTimer -= diff;
        return;
    }

    // something found: a fish now and then, the ore or the herbs at the end
    BotWorldEvents::AddGatheredGoods(_me, _gatherKind);
    if (_gatherKind == BOT_GATHER_FISHING)
    {
        _me->HandleEmoteCommand(EMOTE_ONESHOT_FISHING);
        _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_FISHING);
        if (roll_chance_i(25))
            _ai->GetChatter().Announce(BOT_TEXT_GATHER_CATCH, {}, CHAT_MSG_MONSTER_SAY);
        _gatherTimer = urand(15, 40) * IN_MILLISECONDS;
    }
    else
        Stop();
}

void BotActivity::EndGather()
{
    if (_gatherWorking)
        _me->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
    if (_gatherKind == BOT_GATHER_FISHING && _gatherWorking)
        _me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID, _gatherOldItem);
    if (!_gatherBobber.IsEmpty())
        if (GameObject* bobber = ObjectAccessor::GetGameObject(*_me, _gatherBobber))
            bobber->Delete();

    _gatherNode.Clear();
    _gatherBobber.Clear();
    _gatherWorking = false;
    _gatherTimer = 0;
}

bool BotActivity::RequestRest(Player const* /*player*/)
{
    if (_mode == BOT_ACTIVITY_REST)
        return true;
    if (!_me->IsInWorld() || !_me->IsAlive() || _me->IsInCombat() || _ai->IsDuringTeleport())
        return false;

    Stop();
    _mode = BOT_ACTIVITY_REST;
    _requested = true;
    _modeTimer = urand(60, 180) * IN_MILLISECONDS;
    _ai->HoldPosition(HOLD_POSITION_TIME);
    SitDown();
    return true;
}

// an existing campfire nearby or a conjured one at pos
bool BotActivity::PlaceCampfire(Position const& pos, float searchRadius)
{
    GameObject* fire = _me->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_SPELL_FOCUS, searchRadius);
    if (fire && fire->GetGOInfo()->spellFocus.focusId == CAMPFIRE_FOCUS_ID)
    {
        _fire.Relocate(fire->GetPositionX(), fire->GetPositionY(), fire->GetPositionZ());
        return true;
    }

    if (!sObjectMgr->GetGameObjectTemplate(GO_BASIC_CAMPFIRE))
        return false;

    _me->HandleEmoteCommand(EMOTE_ONESHOT_KNEEL);
    GameObject* summoned = _me->SummonGameObject(GO_BASIC_CAMPFIRE, pos.GetPositionX(), pos.GetPositionY(),
        pos.GetPositionZ(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 15 * MINUTE);
    if (!summoned)
        return false;

    _campfire = summoned->GetGUID();
    _fire.Relocate(pos);
    return true;
}

// 1 on 1 roleplay: the bot sits down at a campfire and tells the player a story OpenAI makes up
bool BotActivity::StartStoryWith(Player const* player)
{
    if (IsTellingStoryTo(player->GetGUID()))
        return true;
    if (!_me->IsInWorld() || !_me->IsAlive() || _me->IsInCombat() || _ai->IsDuringTeleport() ||
        _me->GetMap()->IsBattlegroundOrArena())
        return false;
    if (!player->IsInWorld() || player->GetMap() != _me->GetMap() ||
        !_me->IsWithinDistInMap(player, STORY_START_MAX_DISTANCE))
        return false;

    Stop();

    if (_me->isMoving())
        _me->BotStopMovement();

    // the fire between the bot and the player
    float fireDist = std::min(_me->GetExactDist2d(player) * 0.5f, 3.0f);
    Position firePos = _me->GetFirstCollisionPosition(fireDist, _me->GetRelativeAngle(player));
    if (!PlaceCampfire(firePos, ROLEPLAY_CAMPFIRE_SEARCH_RADIUS))
        _fire.Relocate(firePos);

    // a seat at the fire on the bot's side, facing the fire
    float angle = _fire.GetAbsoluteAngle(_me);
    float x = _fire.GetPositionX() + STORY_SEAT_DISTANCE * std::cos(angle);
    float y = _fire.GetPositionY() + STORY_SEAT_DISTANCE * std::sin(angle);
    float z = _fire.GetPositionZ();
    _me->UpdateGroundPositionZ(x, y, z);
    _seat.Relocate(x, y, z, Position::NormalizeOrientation(angle + float(M_PI)));
    _seated = false;

    _mode = BOT_ACTIVITY_STORY;
    _requested = true;
    _modeTimer = STORY_MAX_TIME;
    _storyPlayer = player->GetGUID();
    _storyTranscript.clear();
    _storyLength = uint8(urand(5, 10));
    _storyLinesTold = 0;
    _storyWaiting = false;
    _storyEnding = false;
    _storyTimer = urand(3000, 5000);
    _ai->HoldPosition(HOLD_POSITION_TIME);
    return true;
}

void BotActivity::UpdateStory(uint32 diff)
{
    Player* player = ObjectAccessor::GetPlayer(*_me, _storyPlayer);
    if (!player || !player->IsInWorld() || !_me->IsWithinDistInMap(player, STORY_MAX_PLAYER_DISTANCE) ||
        _modeTimer <= diff)
    {
        Stop();
        return;
    }
    _modeTimer -= diff;

    UpdateSeat();

    if (_storyTimer > diff)
    {
        _storyTimer -= diff;
        return;
    }
    _storyTimer = 0;

    // the story is over, or no answer came
    if (_storyEnding || _storyWaiting)
    {
        Stop();
        return;
    }

    if (_ai->GetChatter().RequestStoryStep(player))
    {
        _storyWaiting = true;
        _storyTimer = STORY_ANSWER_TIMEOUT;
    }
    else
        _storyTimer = 5000;
}

void BotActivity::OnStoryComment(std::string_view comment)
{
    if (!IsTellingStory() || comment.empty())
        return;

    _storyTranscript.push_back({ false, std::string(comment) });
    // answer soon
    if (!_storyWaiting && !_storyEnding)
        _storyTimer = std::min<uint32>(_storyTimer, 2000);
}

void BotActivity::OnStoryLine(std::string const& line, bool end)
{
    if (!IsTellingStory())
        return;

    _storyWaiting = false;
    if (!line.empty())
        _storyTranscript.push_back({ true, line });
    ++_storyLinesTold;

    if (end || _storyLinesTold >= _storyLength + 2)
    {
        // let the goodbye sink in
        _storyEnding = true;
        _storyTimer = 8000;
    }
    else
        _storyTimer = urand(5000, 8000);
}

void BotActivity::SitDown()
{
    if (_me->IsMounted())
        _me->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (_me->isMoving())
        _me->BotStopMovement();
    _me->SetStandState(UNIT_STAND_STATE_SIT);
}

void BotActivity::StandUp()
{
    if (_me->IsInWorld() && _me->IsAlive() && _me->GetStandState() != UNIT_STAND_STATE_STAND)
        _me->SetStandState(UNIT_STAND_STATE_STAND);
}

bool BotActivity::StartRest()
{
    if (!IsSafeSpot())
        return false;

    _mode = BOT_ACTIVITY_REST;
    _modeTimer = urand(60, 180) * IN_MILLISECONDS;
    _ai->HoldPosition(HOLD_POSITION_TIME);
    SitDown();
    return true;
}

void BotActivity::UpdateRest(uint32 diff)
{
    if (_modeTimer <= diff)
    {
        Stop();
        return;
    }
    _modeTimer -= diff;

    // something made us stand up (talking, a spell...), sit down again, not in the middle of a dance
    if (!_me->isMoving() && _me->GetStandState() == UNIT_STAND_STATE_STAND &&
        !_ai->GetChatter().IsPerformingStateEmote())
        SitDown();
}

bool BotActivity::StartRoleplay()
{
    if (!IsSafeSpot())
        return false;

    std::list<Creature*> candidates;
    IdleWandererCheck check(_me, ROLEPLAY_RECRUIT_RADIUS);
    Acore::CreatureListSearcher<IdleWandererCheck> searcher(_me, candidates, check);
    Cell::VisitObjects(_me, searcher, ROLEPLAY_RECRUIT_RADIUS);
    if (candidates.empty())
        return false;

    std::vector<Creature*> guests(candidates.begin(), candidates.end());
    Acore::Containers::RandomShuffle(guests);
    guests.resize(std::min<std::size_t>(guests.size(), urand(1, ROLEPLAY_MAX_GUESTS)));

    // an existing campfire nearby, otherwise conjure one
    GameObject* fire = _me->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_SPELL_FOCUS, ROLEPLAY_CAMPFIRE_SEARCH_RADIUS);
    if (fire && fire->GetGOInfo()->spellFocus.focusId != CAMPFIRE_FOCUS_ID)
        fire = nullptr;

    if (fire)
        _fire.Relocate(fire->GetPositionX(), fire->GetPositionY(), fire->GetPositionZ());
    else
    {
        if (!sObjectMgr->GetGameObjectTemplate(GO_BASIC_CAMPFIRE))
            return false;

        Position pos = _me->GetFirstCollisionPosition(3.0f, 0.0f);
        _me->BotStopMovement();
        _me->HandleEmoteCommand(EMOTE_ONESHOT_KNEEL);
        // outlasts the stories, removed when the circle breaks up
        GameObject* summoned = _me->SummonGameObject(GO_BASIC_CAMPFIRE, pos.GetPositionX(), pos.GetPositionY(),
            pos.GetPositionZ(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 15 * MINUTE);
        if (!summoned)
            return false;

        _campfire = summoned->GetGUID();
        _fire.Relocate(pos);
    }

    // a circle around the fire, the host takes the first seat
    std::vector<Creature*> circle{ _me };
    circle.insert(circle.end(), guests.begin(), guests.end());
    float step = 2.0f * float(M_PI) / float(circle.size());
    float start = _fire.GetAbsoluteAngle(_me);

    _mode = BOT_ACTIVITY_ROLEPLAY;
    _host = _me->GetGUID();
    _guests.clear();
    _phase = ROLEPLAY_GATHER;
    _phaseTimer = 30 * IN_MILLISECONDS;
    _storiesLeft = uint8(urand(1, 3));
    _teller.Clear();

    for (std::size_t i = 0; i < circle.size(); ++i)
    {
        float angle = start + step * float(i);
        float x = _fire.GetPositionX() + ROLEPLAY_CIRCLE_RADIUS * std::cos(angle);
        float y = _fire.GetPositionY() + ROLEPLAY_CIRCLE_RADIUS * std::sin(angle);
        float z = _fire.GetPositionZ();
        _me->UpdateGroundPositionZ(x, y, z);
        Position seat(x, y, z, Position::NormalizeOrientation(angle + float(M_PI)));

        if (circle[i] == _me)
        {
            _seat.Relocate(seat);
            _seated = false;
        }
        else
        {
            _guests.push_back(circle[i]->GetGUID());
            circle[i]->GetBotAI()->GetActivity().JoinCircle(_host, seat, _fire);
        }
    }

    BOT_LOG_DEBUG("npcbots", "Bot {} ({}) gathers {} bots around a campfire", _me->GetName(), _me->GetEntry(),
        uint32(_guests.size()));
    return true;
}

void BotActivity::JoinCircle(ObjectGuid host, Position const& seat, Position const& fire)
{
    _mode = BOT_ACTIVITY_ROLEPLAY;
    _host = host;
    _guests.clear();
    _campfire.Clear();
    _seat.Relocate(seat);
    _fire.Relocate(fire);
    _seated = false;
    _ai->HoldPosition(HOLD_POSITION_TIME);
}

// walk to the seat, sit down facing the fire
void BotActivity::UpdateSeat()
{
    if (_seated)
    {
        if (!_me->isMoving() && _me->GetStandState() == UNIT_STAND_STATE_STAND &&
            !_ai->GetChatter().IsPerformingStateEmote())
            SitDown();
        return;
    }

    if (_me->GetExactDist2d(_seat) < 1.0f)
    {
        if (!_me->isMoving())
        {
            _me->SetFacingTo(_seat.GetOrientation());
            SitDown();
            _seated = true;
        }
        return;
    }

    if (!_me->isMoving())
    {
        if (_me->IsMounted())
            _me->RemoveAurasByType(SPELL_AURA_MOUNTED);
        _ai->BotMovement(BOT_MOVE_POINT, &_seat, nullptr, true);
    }
}

bool BotActivity::IsGuest(ObjectGuid guid) const
{
    return std::ranges::find(_guests, guid) != _guests.end();
}

bool BotActivity::UpdateRoleplayGuest()
{
    Creature* host = ObjectAccessor::GetCreature(*_me, _host);
    if (!host || !host->IsAlive() || !host->GetBotAI())
        return false;

    BotActivity const& hostActivity = host->GetBotAI()->GetActivity();
    if (!hostActivity.IsInRoleplay() || hostActivity._host != _host || !hostActivity.IsGuest(_me->GetGUID()))
        return false;

    UpdateSeat();
    return true;
}

// the host and the guests still in the circle
std::vector<Creature*> BotActivity::GetCircle(bool seatedOnly) const
{
    std::vector<Creature*> circle{ _me };
    for (ObjectGuid guid : _guests)
    {
        Creature* guest = ObjectAccessor::GetCreature(*_me, guid);
        if (!guest || !guest->IsAlive() || !guest->GetBotAI())
            continue;

        BotActivity const& activity = guest->GetBotAI()->GetActivity();
        if (activity.IsInRoleplay() && activity._host == _host && (!seatedOnly || activity._seated))
            circle.push_back(guest);
    }
    return circle;
}

bool BotActivity::StartNextStory(std::vector<Creature*> const& circle)
{
    if (!_storiesLeft || circle.size() < 2)
        return false;

    --_storiesLeft;

    // somebody else tells the next story
    std::vector<Creature*> tellers;
    for (Creature* bot : circle)
        if (bot->GetGUID() != _teller)
            tellers.push_back(bot);
    _teller = Acore::Containers::SelectRandomContainerElement(tellers)->GetGUID();

    _story = urand(0, STORIES_COUNT - 1);
    _storyLine = 0;
    _phase = ROLEPLAY_LINE;
    _phaseTimer = urand(3000, 6000);
    return true;
}

void BotActivity::UpdateRoleplayHost(uint32 diff)
{
    UpdateSeat();

    // everybody seated: no need to wait any longer
    if (_phase == ROLEPLAY_GATHER && _seated && GetCircle(true).size() == _guests.size() + 1)
        _phaseTimer = std::min<uint32>(_phaseTimer, 2000);

    if (_phaseTimer > diff)
    {
        _phaseTimer -= diff;
        return;
    }
    _phaseTimer = 0;

    std::vector<Creature*> circle = GetCircle(_phase != ROLEPLAY_GATHER);

    if (_phase == ROLEPLAY_GATHER)
    {
        // everybody seated or waited long enough: guests that did not make it leave the circle
        std::vector<Creature*> seated = GetCircle(true);
        if (seated.size() < 2 || !_seated)
        {
            Stop();
            return;
        }

        std::erase_if(_guests, [&seated](ObjectGuid guid) {
            return std::ranges::none_of(seated, [guid](Creature const* bot) { return bot->GetGUID() == guid; });
        });

        if (!StartNextStory(seated))
            Stop();
        return;
    }

    if (circle.size() < 2)
    {
        Stop();
        return;
    }

    Creature* teller = ObjectAccessor::GetCreature(*_me, _teller);
    if (!teller || std::ranges::find(circle, teller) == circle.end())
    {
        if (!StartNextStory(circle))
            Stop();
        return;
    }

    const uint32 linesId = STORY_TEXT_FIRST + 2 * _story;
    const uint32 reactionsId = linesId + 1;

    switch (_phase)
    {
        case ROLEPLAY_LINE:
        {
            if (_storyLine >= CountTextSlots(linesId))
            {
                _phase = ROLEPLAY_APPLAUSE;
                _phaseTimer = urand(1500, 2500);
                return;
            }

            teller->GetBotAI()->GetChatter().SayText(linesId, _storyLine);
            ++_storyLine;

            // now and then a listener reacts, never after the last line
            bool react = _storyLine < CountTextSlots(linesId) && CountTextSlots(reactionsId) && roll_chance_i(40);
            _phase = react ? ROLEPLAY_REACTION : ROLEPLAY_LINE;
            _phaseTimer = react ? urand(3500, 5000) : urand(6000, 9000);
            break;
        }
        case ROLEPLAY_REACTION:
        {
            std::vector<Creature*> listeners;
            for (Creature* bot : circle)
                if (bot != teller)
                    listeners.push_back(bot);

            Creature* listener = Acore::Containers::SelectRandomContainerElement(listeners);
            listener->GetBotAI()->GetChatter().SayText(reactionsId, uint8(urand(0, CountTextSlots(reactionsId) - 1)));

            _phase = ROLEPLAY_LINE;
            _phaseTimer = urand(4000, 6000);
            break;
        }
        case ROLEPLAY_APPLAUSE:
        {
            static constexpr std::array applause{
                TEXT_EMOTE_APPLAUD, TEXT_EMOTE_LAUGH, TEXT_EMOTE_CHEER, TEXT_EMOTE_NOD
            };
            for (Creature* bot : circle)
            {
                if (bot != teller && roll_chance_i(70))
                {
                    uint32 emote = Acore::Containers::SelectRandomContainerElement(applause);
                    bot->GetBotAI()->GetChatter().DoTextEmote(emote);
                }
            }

            if (StartNextStory(circle))
                _phaseTimer = urand(6000, 10000);
            else
                Stop();
            break;
        }
        default:
            Stop();
            break;
    }
}

void BotActivity::EndRoleplay()
{
    // sharing stories at a campfire makes friends
    if (BotCfg::IsBotBondsEnabled())
        for (ObjectGuid guest : _guests)
            if (Creature const* bot = ObjectAccessor::GetCreature(*_me, guest))
                BotChatter::NoteBondBetween(_me, bot, 2);

    if (!_campfire.IsEmpty())
        if (GameObject* fire = ObjectAccessor::GetGameObject(*_me, _campfire))
            fire->Delete();

    // guests notice the host is done on their next update
    _guests.clear();
}
