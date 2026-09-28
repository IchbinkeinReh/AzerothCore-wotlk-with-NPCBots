# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

## NPCBots fork

`AGENTS.md` is upstream AzerothCore's file and is kept untouched to avoid merge conflicts. This repo is the
trickerer NPCBots fork; the notes below cover what differs from upstream.

### Branches and remotes

- Working branch: `npcbots_3.3.5`. It regularly merges the AzerothCore `3.3.5` branch ("Merge branch '3.3.5' into
  npcbots_3.3.5").
- Remotes: `origin` = trickerer/AzerothCore-wotlk-with-NPCBots (fork upstream), `upstream` = azerothcore/azerothcore-wotlk,
  `ibkr` = the user's own fork (push target).

### Where the bot code lives

- `src/server/game/AI/NpcBots/` holds the whole bot system. It compiles into the game library, not as a module.
  - `bot_ai.{h,cpp}`: base bot AI. Each class has its own `bot_<class>_ai.cpp`, including custom classes (archmage,
    blademaster `bm`, dreadlord, necromancer, sea_witch, spellbreaker, …). Bot pets are `bpet_ai.*` / `bpet_<class>.cpp`.
  - `botmgr.*`: per-player bot ownership and management. `botdatamgr.*`: global bot data, spawning, DB persistence.
    `botconfig.*`: reads the `NpcBot.*` config. `botcommands.cpp`: the `.npcbot` chat commands. `botgiver.cpp`:
    the bot-hiring gossip. `botwanderful.*`: wandering/free bots. `botspell.*`, `botgearscore.*`, `botdump.*`, `botlog.*`.
- Hooks in core files are wrapped in marker comments:
  ```cpp
  //npcbot
  ...
  //end npcbot
  ```
  Keep every fork change to shared AC files inside these markers. When resolving merges from `3.3.5`, they show which
  side belongs to the fork. They appear throughout `src/server/game` (Creature, Unit, Player, Group, Map, …) and in a
  few scripts.
- Config: the `NpcBot.*` options live in `src/server/apps/worldserver/worldserver.conf.dist`, not in a separate
  conf file.

### Bot SQL

- Base schema: `data/sql/base/db_characters/characters_npcbot*.sql` and
  `data/sql/base/db_world/creature_template_npcbot_*.sql`.
- Bot data and updates are tracked in `data/sql/custom/db_{auth,characters,world}/`. In this fork that directory is
  committed, not gitignored as `AGENTS.md` says. Files there must be re-applicable (`DELETE`+`INSERT`,
  `REPLACE`, `IF NOT EXISTS`, …; see `data/sql/custom/README.md`).
- Bot text locales: `data/sql/Bots/locales/<locale>/`.
