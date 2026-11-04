# Quick settings

Status: implemented in firmware 0.3.31 and host-tested. The device check is
PENDING (see the end of this document).

## Decision

**Decision, 2026-11-04:** quick settings as in Wear OS, after the home watch
face ([system navigation](system-navigation.md)). This is the spec's
`QuickSettings` component (§12.2), kept minimal.

- **Opening.** A swipe down from the top edge of the home application opens
  the panel. The swipe must start in a 180 × 64 strip (x=90..269, y=0..63) and
  travel at least 80 px down, at least twice its sideways travel. A blue
  "Ustawienia" pill follows the finger. Other applications keep their full
  screen; the strip exists only on home.
- **Panel.** An opaque, full-screen object on LVGL's top layer, owned by the
  launcher on the GUI task:
  - the battery charge from the BQ27220 sample of the system worker, with
    "ładuje" when the current is positive, or "brak odczytu";
  - **Jasność**: the backlight in steps of 30, 60 and 100 %, the same steps as
    the diagnostic screen's button;
  - **Wygaś ekran**: the screen off, as in the power menu;
  - **Uśpij**: light sleep, as in the power menu.
- **Closing.** A swipe up that starts outside the buttons closes the panel.
  The screen and sleep actions also close it, and so does any change away
  from a running home (exit, list).
- **Ownership.** `main.c` passes the actions as a `roundie_launcher_quick_t`
  table, so the launcher has no board or power dependency. Brightness is not
  persisted; it starts at 60 % after each boot, as before.
- **Language.** System strings are Polish (see below).

## Sleep and restart with home loaded

**Decision, 2026-11-04.** Light sleep, Restart and Safe Mode require an idle
runtime ([PowerManager](power-manager.md)). The home application is always
loaded, so without a change they would always be refused.

- The power worker now asks the launcher to unload the home application
  (`roundie_launcher_park_home`). It waits up to 3 s for the runtime to become
  idle, then admits the request as before.
- After the wake, or after a refusal, `roundie_launcher_resume_home` launches
  home again. A restart needs no resume.
- Any other loaded application still refuses, as before.
- Light sleep with native code loaded remains excluded; it is unmeasured.

## Evidence

- **Host-verified.** `tests/test_launcher.c`, using a real LVGL pointer:
  - the strip exists only on home, and a short swipe is rejected;
  - the pill text, the battery and brightness text, and a brightness step;
  - closing by a swipe up, and the Screen off and Sleep actions;
  - the panel closes with home;
  - park and resume: home unloads, stays unloaded, and relaunches once on
    resume; another application is not parked.
- **Not host-tested.** The power worker glue in `main/power_manager.c` and the
  callbacks in `main/main.c`.
- **Device, 0.3.31 (OTA VALID, RW1 command, not touch):** with home ACTIVE
  (generation 1), `roundie_power.py sleep 5` slept 4991 ms with refusal
  `none`. After the timer wake, `sleeps` was 1 and home ran again
  (`org.roundie.watch`, generation 2). The screen stayed off after the timer
  wake, as for any timer wake.
- **Device (PENDING until reported):**
  - finger swipe down and its panel;
  - brightness steps;
  - screen off;
  - Uśpij from the panel, then wake by touch: the watch face returns;
  - Restart from the BOOT menu with home loaded.

## Polish system strings

**Decision, 2026-11-04.** The launcher, the swipe pills and the BOOT power menu
are Polish: "Aplikacje", "Zamknij", "Zegarek", "Zamykanie...", "Odśwież",
"Wstecz", the status messages, and the menu items "Uśpij", "Wygaś ekran",
"Restart", "Tryb awaryjny", "Anuluj". Not translated: the developer diagnostic
screen, the result screen's technical lines apart from their labels, logs,
RP1/RW1 protocol text, and the package names, which belong to the packages.

- **Fonts.** The LVGL fonts (`components/roundie_fonts`) cover the Polish
  letters (U+0104-0107, 0118-0119, 0141-0144, 015A-015B, 0179-017C, Ó, ó) but
  not U+00B7. The battery line therefore separates with "•" (U+2022), which
  they do include. Any new string must stay inside those ranges.
- **Verification.** Host: `tests/test_launcher.c` asserts the pill, status and
  result texts. Device 0.3.33: OTA VALID.
- **User-reported, 2026-11-04 (0.3.33):** "Wyglądają dobrze" (they look good).
  Which screens were looked at was not recorded.
