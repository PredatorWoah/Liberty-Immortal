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
