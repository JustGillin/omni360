# Instructions: keeping `featured.json` filled

You maintain `featured.json`, in the root of this repository. It decides which
games the Omni360 Store shows at the top of its front page. Omni360 is an
Xbox 360 app. Your job: plan the featured games for the next **12 weeks**,
week by week, then check your work with the tool.

## What the file does

```json
{
  "events": [
    { "from": "2026-10-05", "to": "2026-10-11", "label": "Gears of War: E-Day is out",
      "games": ["4D5307D5", "4D53082D", "4D5308AB", "4D530A26"] }
  ],
  "rotation": [
    ["4D5307E6", "545407D8", "4D530AA4"]
  ]
}
```

- **events**: each one is shown from its `from` date to its `to` date, both
  days included. `label` is the heading shown over the games. `games` is a
  list of 1 to 5 title IDs. The first game in the list gets the big tile.
- **rotation**: sets of 1 to 5 title IDs. These are used automatically in any
  week that has no event. Do not delete existing rotation sets; you may add
  more.
- If two events cover the same day, the one earlier in the file wins. Avoid
  overlaps.

## Rules (all of them are required)

1. **Never type a title ID from memory.** Every ID must come from this tool:
   ```
   python tools/featured.py search <words of the game's name>
   ```
   It prints `TITLEID  Name`. Copy the 8-character ID exactly (capital
   letters).
2. **Only Xbox 360 games.** If the search output says
   `(Original Xbox - no wallpaper, can't be featured)`, do not use that game.
3. If the search output says `(shows as "...")`, the Store will show that
   other name. Only use it if that name looks right; otherwise pick another
   game.
4. If a game is not found by the search, the Store does not have it. Do not
   use it, and do not invent an ID. Pick a related game the Store does have.
5. Each event and each rotation set has **1 to 5 games**. **3 to 5 looks
   best.** No game twice in one list.
6. Events are **one week long, Monday to Sunday**. Dates are `YYYY-MM-DD`.
7. **Labels are short**: under 40 characters. Plain English, no emoji, and
   nothing that could be read as an official Microsoft or Xbox announcement.
   Good: `Grand Theft Auto VI is out`, `Racing week`,
   `Hidden gems: puzzle games`. Bad: `XBOX OFFICIAL: GTA6 LAUNCH EVENT!!!`.
8. Keep any event that is still running or in the future. Delete events
   whose `to` date is in the past.
9. The file must stay valid JSON.

## How to plan the 12 weeks

**Step 1: big releases come first.** Look up the release dates of big new
games coming out in the next 12 weeks, on any platform: for example a new
Grand Theft Auto, Fable, Call of Duty, Halo, Gears of War, Forza, Battlefield,
Assassin's Creed, Elder Scrolls or Fallout. Only use a date you found from a
reliable source (the publisher, or a major games news site). If a date is
only rumoured, skip it.

For each confirmed release, make an event for the week (Monday to Sunday)
that contains the release day. Feature the **older games in that series that
the Store has**, for example:

- a new Grand Theft Auto: search `grand theft auto`, use GTA V, San Andreas
  and Episodes from Liberty City
- a new Fable: search `fable`
- a new Call of Duty: search `call of duty`, `modern warfare`, `black ops`

Label it `<Game name> is out`, or `<Game name> is out this week` if it comes
out mid-week. If a series has no 360 games in the Store, fill that week with
a theme instead (step 2), for example `Open worlds` for an open-world game.

**Step 2: every other week gets a theme.** Make an event for each week that
is still empty, with a theme. Vary them, and don't repeat a theme within the
12 weeks. Ideas:

- By genre: `Shooters`, `Racing`, `Puzzle games`, `Fighting games`,
  `Role-playing games`, `Platformers`, `Sports`, `Strategy`, `Horror`
- Hidden gems: good games that were less popular, for example
  `Hidden gems: action`, `Hidden gems: puzzle games`
- Collections: `Rare on the Xbox 360`, `BioWare's best`, `Halo`, `Couch co-op`
- Seasons and dates: `Spooky games` in the week of Oct 31,
  `Winter games` in December

For each theme, first think of 6 to 10 Xbox 360 games that fit it, by name.
`search` only matches game names, never genres: `search puzzle` finds
nothing, but `search portal`, `search catherine` or `search tetris` do.
Search each name, keep the ones the Store has, and use 3 to 5 of them.
Expect misses: the Store has disc games only, so Xbox Live Arcade games
(Braid, Limbo, Castle Crashers and so on) are never found. If you aren't
sure what genre a game is, don't use it.

## Check your work

After editing, run:

```
python tools/featured.py check --art
```

It must say `0 error(s)`. Fix every `ERROR` and run it again. Warnings are
allowed, but read them. Then spot-check a few weeks:

```
python tools/featured.py show 2026-11-02
```

This prints exactly what the Store will show that day.

## When you finish

Report a short table: each week's dates, its label, and its games by name.
Then list any big releases you found but could not feature, and why.
