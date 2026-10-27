# GTA IV cheats (Xbox 360 version, base game)

GTA IV cheats are phone numbers. They are the same on Xbox 360, PS3 and PC. To use one:

1. Press Up on the D-pad to take out Niko's phone, then Up again to switch to the keypad.
2. Dial the number and call it. "Cheat activated" appears at the top left.
3. After the first time, the cheat is also saved in the phone's **Cheats** menu.

You need the phone from the early story missions. Several cheats block achievements for that
playthrough, so save before experimenting.

| Cheat | Number |
|---|---|
| Full health and armor | 362-555-0100 |
| Full health, armor and ammo | 482-555-0100 |
| Weapons set 1 | 486-555-0100 |
| Weapons set 2 | 486-555-0150 |
| Remove wanted level | 267-555-0100 |
| Raise wanted level | 267-555-0150 |
| Change weather | 468-555-0100 |
| Annihilator (helicopter) | 359-555-0100 |
| FIB Buffalo | 227-555-0100 |
| Cognoscenti | 227-555-0142 |
| Turismo | 227-555-0147 |
| Super GT | 227-555-0168 |
| Comet | 227-555-0175 |
| NRG-900 | 625-555-0100 |
| Sanchez | 625-555-0150 |
| Jetmax (boat) | 938-555-0100 |

Sources disagree about which weapon set is which (0100 versus 0150); check in game.
The Lost and Damned and The Ballad of Gay Tony have extra cheats of their own that this list
does not cover, and those episodes are not installed in this build.

Source: https://gtaintel.com/news/gta-4-cheat-codes-canonical-guide (one page, 16 base game
entries; other pages online contradict some numbers).

## Cheat buttons in game (experimental)

The touch layout has a **CH** button next to the phone button (on foot and driving). It opens a
list of cheats. Tapping one takes the phone out, opens the keypad, moves the keypad cursor with
D-pad presses to each digit and presses select, as you would by hand. Touch any control, or tap
**CH** again, to cancel.

How the keypad cursor behaves was not known when this was written, so the whole dialing recipe is
a text file you can edit without rebuilding: `liberty_cheats.txt` in the app's user folder
(created on first use). It holds the cheat list (add your own, for example the episode cheats),
`home`/`wrap`/`start` for the cursor, the `tail` buttons that press call (empty by default: the
digits are dialed and you press call yourself), and the press timings. Every step is logged as
`liberty-cheat:` lines in the runtime log.

## Instant cheats (no phone, experimental)

Every cheat in the CH list is now instant. The app calls the game's own script functions from inside
a running game script, so nothing is dialed. Save first and use single player only.

| Cheat | What it calls |
|---|---|
| Full health and armor | `SET_CHAR_HEALTH` 200, `ADD_ARMOUR_TO_CHAR` 100 |
| Health, armor and ammo | the same, plus `ADD_AMMO_TO_CHAR` for the weapon in hand |
| Weapons set 1 / 2 | `GIVE_WEAPON_TO_CHAR` (set 1: bat, grenades, pistol, shotgun, micro SMG, AK-47, sniper; set 2: molotovs, Desert Eagle, combat shotgun, MP5, M4, combat sniper, RPG). The sets are a best match: the guides disagree on which is which |
| Remove wanted level | `CLEAR_WANTED_LEVEL` |
| Raise wanted level | `ALTER_WANTED_LEVEL` one star up |
| Change weather | `FORCE_WEATHER_NOW`, cycles |
| Vehicles | the game's `GET_HASH_KEY`, `REQUEST_MODEL`, `LOAD_ALL_OBJECTS_NOW`, `CREATE_CAR` six metres in front of you |

Add your own vehicle with a line like `Infernus = @car:infernus` in `liberty_cheats.txt` (user
folder). Lines with a phone number still dial the phone. An older `liberty_cheats.txt` is kept as
`liberty_cheats.old.txt` and replaced by the new list on first start.

Everything it does is logged as `liberty-direct:` lines in the runtime log. The game's complete
list of script functions is written to `liberty_natives.txt` a few seconds after it starts.

## Episode cheats (The Lost and Damned, The Ballad of Gay Tony)

The cheat panel now also lists the vehicle cheats of both episodes as instant buttons (file version v3,
the old file is kept as `liberty_cheats.old.txt`):

| Button | Phone code | Model names tried |
|---|---|---|
| TLAD Innovation | 245-555-0100 | innovation |
| TLAD Hexer | 245-555-0150 | hexer |
| TLAD Double T Custom | 245-555-0125 | double, doublet, doublecustom |
| TLAD Hakuchou Custom | 245-555-0199 | hakuchou2, hakuchoucustom, hakuchou |
| TLAD Slamvan | 826-555-0100 | slamvan |
| TLAD Gang Burrito | 826-555-0150 | gburrito, gangburrito, burrito2 |
| TBoGT Bullet GT | 227-555-9666 | bullet |
| TBoGT APC | 272-555-8265 | apc |
| TBoGT Buzzard | 359-555-2899 | buzzard |
| TBoGT Akuma | 625-555-0200 | akuma |
| TBoGT Vader | 625-555-3273 | vader |
| TBoGT Floater | 938-555-0150 | floater |

A button only works in its own episode (the model does not exist elsewhere). The model names are
from memory, so a button that does nothing may need another name: check `liberty-direct:` lines in the
runtime log, or dial the phone code instead (`Name = 245-555-0100`). Not done: Parachute, explosive
punches and explosive sniper rounds (no script function for them was found).
