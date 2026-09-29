#ifndef BOTACTIVITY_H
#define BOTACTIVITY_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <vector>

/*
NpcBot Activities: what a free wandering bot is doing (NpcBot.WanderingBots.Activities.*).
- active:   wandering from node to node (default, all wanderers spawn active)
- rest:     sitting somewhere without hostile creatures around
- roleplay: 2-5 bots sitting around a campfire telling each other stories, the host bot leads the circle

Stories are stored in npc_text: story k (0-49) lines in 71101+2k, listener reactions in 71102+2k.
*/

class bot_ai;
class Creature;

enum BotActivityMode : uint8
{
    BOT_ACTIVITY_ACTIVE     = 0,
    BOT_ACTIVITY_REST,
    BOT_ACTIVITY_ROLEPLAY
};

class BotActivity
{
public:
    BotActivity(bot_ai* ai, Creature* bot);

    void Update(uint32 diff);

    BotActivityMode GetMode() const { return _mode; }
    bool IsResting() const { return _mode == BOT_ACTIVITY_REST; }
    bool IsInRoleplay() const { return _mode == BOT_ACTIVITY_ROLEPLAY; }

    // back to active, stands up
    void Stop();

private:
    enum RoleplayPhase : uint8
    {
        ROLEPLAY_GATHER = 0,
        ROLEPLAY_LINE,
        ROLEPLAY_REACTION,
        ROLEPLAY_APPLAUSE
    };

    bool CanStartActivity() const;
    bool IsSafeSpot() const;
    void ChooseActivity();

    bool StartRest();
    void UpdateRest(uint32 diff);

    bool StartRoleplay();
    void JoinCircle(ObjectGuid host, Position const& seat, Position const& fire);
    void UpdateSeat();
    void UpdateRoleplayHost(uint32 diff);
    bool UpdateRoleplayGuest();
    bool IsGuest(ObjectGuid guid) const;
    std::vector<Creature*> GetCircle(bool seatedOnly) const;
    bool StartNextStory(std::vector<Creature*> const& circle);
    void EndRoleplay();

    void SitDown();
    void StandUp();

    bot_ai* _ai;
    Creature* _me;

    BotActivityMode _mode;
    uint32 _decisionTimer;
    uint32 _modeTimer;

    // roleplay, the host keeps the circle
    ObjectGuid _host;
    std::vector<ObjectGuid> _guests;
    ObjectGuid _campfire;
    Position _fire;
    Position _seat;
    bool _seated;

    RoleplayPhase _phase;
    uint32 _phaseTimer;
    uint32 _story;
    uint8 _storyLine;
    uint8 _storiesLeft;
    ObjectGuid _teller;
};

#endif //BOTACTIVITY_H
