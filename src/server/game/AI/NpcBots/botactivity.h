#ifndef BOTACTIVITY_H
#define BOTACTIVITY_H

#include "botopenai.h"
#include "Common.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <vector>

/*
NpcBot Activities: what a free wandering bot is doing (NpcBot.WanderingBots.Activities.*).
- active:   wandering from node to node (default, all wanderers spawn active)
- rest:     sitting somewhere without hostile creatures around
- roleplay: 2-5 bots sitting around a campfire telling each other stories, the host bot leads the circle
- story:    a bot at a campfire telling a player a story made up by OpenAI, sentence by sentence, reacting
            to what the player says (1 on 1 roleplay)
- event:    a world event (botworldevents.h): gathering at a rally point, then attacking a camp or a rare
            creature, defending a town or taking a world pvp objective together with other bots and players
- companion: travelling with a friend (another bot, see BotChatter bonds) for a while, helping in fights
- gather:   fishing at a lake or the sea, mining an ore vein or picking herbs nearby (looks only, the node
            stays), the finds are sold in the Trade channel (botworldevents.h)

Free wandering bots choose rest and roleplay on their own. Players can ask any bot (hired ones too) through
OpenAI to rest or to tell a story.

Stories are stored in npc_text: story k (0-49) lines in 71101+2k, listener reactions in 71102+2k.
*/

class bot_ai;
class Creature;
class Player;

enum BotActivityMode : uint8
{
    BOT_ACTIVITY_ACTIVE     = 0,
    BOT_ACTIVITY_REST,
    BOT_ACTIVITY_ROLEPLAY,
    BOT_ACTIVITY_STORY,
    BOT_ACTIVITY_EVENT,
    BOT_ACTIVITY_COMPANION,
    BOT_ACTIVITY_GATHER
};

enum BotGatherKind : uint8
{
    BOT_GATHER_FISHING = 0,
    BOT_GATHER_MINING,
    BOT_GATHER_HERBALISM
};

class BotActivity
{
public:
    BotActivity(bot_ai* ai, Creature* bot);

    void Update(uint32 diff);

    BotActivityMode GetMode() const { return _mode; }
    bool IsResting() const { return _mode == BOT_ACTIVITY_REST; }
    bool IsInRoleplay() const { return _mode == BOT_ACTIVITY_ROLEPLAY; }
    bool IsTellingStory() const { return _mode == BOT_ACTIVITY_STORY; }
    bool IsTellingStoryTo(ObjectGuid player) const { return IsTellingStory() && _storyPlayer == player; }
    // sitting somewhere instead of wandering or following
    bool IsSeated() const
    {
        return _mode == BOT_ACTIVITY_REST || _mode == BOT_ACTIVITY_ROLEPLAY || _mode == BOT_ACTIVITY_STORY;
    }
    bool IsInEvent() const { return _mode == BOT_ACTIVITY_EVENT; }
    uint32 GetEventId() const { return IsInEvent() ? _eventId : 0; }

    // back to active, stands up
    void Stop();

    // asked by a player (OpenAI)
    bool RequestRest(Player const* player);
    bool StartStoryWith(Player const* player);

    // world events, world thread: gathering at the rally point, then fighting at the target
    bool JoinEvent(uint32 eventId, Position const& rally);
    void StartEventAttack(Position const& target);

    // travelling with a friend
    bool FollowCompanion(Creature const* leader, uint32 duration);
    bool IsTravellingWithFriend() const { return _mode == BOT_ACTIVITY_COMPANION; }
    // mounted like the friend in front
    bool WantsMount() const { return _mode == BOT_ACTIVITY_COMPANION && _companionMounted; }

    // story told to a player, see BotChatter::RequestStoryStep()
    void OnStoryComment(std::string_view comment);
    void OnStoryLine(std::string const& line, bool end);
    std::vector<BotAIMessage> const& GetStoryTranscript() const { return _storyTranscript; }
    uint8 GetStoryLength() const { return _storyLength; }
    uint8 GetStoryLinesTold() const { return _storyLinesTold; }

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
    bool IsActivityValid() const;
    bool PlaceCampfire(Position const& near, float searchRadius);

    bool StartRest();
    void UpdateRest(uint32 diff);
    void UpdateStory(uint32 diff);
    void UpdateEvent(uint32 diff);
    void UpdateCompanion(uint32 diff);
    bool StartGather();
    bool StartNodeGathering();
    bool StartFishing();
    void UpdateGather(uint32 diff);
    void EndGather();

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
    // asked for by a player, allowed for hired bots too
    bool _requested;

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

    // story told to a player
    ObjectGuid _storyPlayer;
    std::vector<BotAIMessage> _storyTranscript;
    uint8 _storyLength;
    uint8 _storyLinesTold;
    bool _storyWaiting;
    bool _storyEnding;
    uint32 _storyTimer;

    // world event
    uint32 _eventId;
    Position _eventRally;
    Position _eventTarget;
    bool _eventAttack;
    uint32 _eventMoveTimer;

    // companion
    ObjectGuid _companionLeader;
    bool _companionMounted;

    // gathering
    BotGatherKind _gatherKind;
    ObjectGuid _gatherNode;
    Position _gatherSpot;
    Position _gatherFacing;
    bool _gatherWorking;
    uint32 _gatherTimer;
    ObjectGuid _gatherBobber;
    uint32 _gatherOldItem;
};

#endif //BOTACTIVITY_H
