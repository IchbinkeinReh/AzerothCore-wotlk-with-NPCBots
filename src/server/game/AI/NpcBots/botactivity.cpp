#include "bot_ai.h"
#include "botactivity.h"
#include "botchatter.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botdefine.h"
#include "CellImpl.h"
#include "Containers.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"

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
    _decisionTimer(0), _modeTimer(0), _seated(false), _phase(ROLEPLAY_GATHER), _phaseTimer(0), _story(0),
    _storyLine(0), _storiesLeft(0)
{
}

void BotActivity::Update(uint32 diff)
{
    if (!BotCfg::IsBotActivitiesEnabled())
    {
        if (_mode != BOT_ACTIVITY_ACTIVE)
            Stop();
        return;
    }

    if (_mode != BOT_ACTIVITY_ACTIVE)
    {
        // fights, being hired or leaving the world end any activity
        if (!_me->IsInWorld() || !_me->IsAlive() || _me->IsInCombat() || !_ai->IsWanderer() || !_ai->IAmFree() ||
            _ai->IsDuringTeleport())
        {
            Stop();
            return;
        }

        _ai->HoldPosition(HOLD_POSITION_TIME);

        if (_mode == BOT_ACTIVITY_REST)
            UpdateRest(diff);
        else if (_host == _me->GetGUID())
            UpdateRoleplayHost(diff);
        else if (!UpdateRoleplayGuest())
            Stop();
        return;
    }

    if (!_ai->IsWanderer())
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

    if (roll < roleplayChance)
    {
        if (!StartRoleplay())
            StartRest();
    }
    else if (roll < roleplayChance + restChance)
        StartRest();
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

void BotActivity::Stop()
{
    if (_mode == BOT_ACTIVITY_ROLEPLAY && _host == _me->GetGUID())
        EndRoleplay();

    if (_mode != BOT_ACTIVITY_ACTIVE)
        StandUp();

    _mode = BOT_ACTIVITY_ACTIVE;
    _modeTimer = 0;
    _decisionTimer = 0;
    _host.Clear();
    _guests.clear();
    _campfire.Clear();
    _seated = false;
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

    // something made us stand up (talking, a spell...), sit down again
    if (!_me->isMoving() && _me->GetStandState() == UNIT_STAND_STATE_STAND)
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
        if (!_me->isMoving() && _me->GetStandState() == UNIT_STAND_STATE_STAND)
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
    if (!_campfire.IsEmpty())
        if (GameObject* fire = ObjectAccessor::GetGameObject(*_me, _campfire))
            fire->Delete();

    // guests notice the host is done on their next update
    _guests.clear();
}
