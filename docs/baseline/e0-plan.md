# E0 — inwentaryzacja i plan zmian

Etap E0 z [roadmapy](../roadmap.md). Stan kodu: gałąź `e0-baseline` od `e4a3041`
(z commitem roadmapy `86e7966`). Fakty poniżej pochodzą z czytania kodu w tym
commicie; nic tu nie jest wynikiem pomiaru.

## 1. Co bramka E0 już ma

| Pozycja bramki | Źródło | Fakt z kodu |
|---|---|---|
| Heap wewnętrzny bieżący | `RS1 STATUS/HEALTH` `internal_free` | `heap_caps_get_free_size(INTERNAL\|8BIT)` — [recovery_diagnostics.c:380](../../main/recovery_diagnostics.c) |
| Heap wewnętrzny minimalny, największy blok | `RS1 HEALTH` `internal_min`, `internal_largest` | [recovery_diagnostics.c:60](../../main/recovery_diagnostics.c) |
| PSRAM bieżący, minimalny, największy blok | `RS1 HEALTH` `psram_free`, `psram_min`, `psram_largest` | jw. |
| Nieudane alokacje | `RS1 HEALTH` `alloc_failed*` | callback `heap_caps_register_failed_alloc_callback` |
| Zapas stosów | `RS1 HEALTH` `stacks` | `main` (task GUI), `system_worker`, `rep_executor`, `audio`, `esp_timer`, `ipc0/1`, `IDLE0/1`; plus `executor_guard.usable_min` |
| Stos GUI z okresu | `RP1 PERF` `summary[11]`, `RP1 BENCH` `stack_min` | [gui_diagnostics.c](../../main/gui_diagnostics.c) |
| Czasy GUI/DMA | `RP1 PERF` (maksima od startu + pierścień 32 iteracji z `dma_us`, `render_us`, `submit_us`, `wait_us`, `scale_us`) | maksima nie są zerowane; okno scenariusza trzeba liczyć z pierścienia |
| Pełne odświeżanie | `RP1 BENCH <30..1000>` | okres liczony między końcami iteracji GUI, **nie** scanout panelu |
| Wolne LittleFS | `RP1 STATUS` `capacity:[ok,total,used]` | tylko gdy runtime aplikacji jest dopuszczony — [package_diagnostics.c:196](../../main/package_diagnostics.c) |
| Uruchamianie/zamykanie aplikacji | `RP1 SELECT/EXIT/BACK/NAV/LAUNCH` | wybór semantyczny „ta sama skrzynka co dotyk”, pochodzenie INJECTED |
| `PRESS/VALUE` | `RE1 UI PRESS <gen> <widget>`, `RE1 UI VALUE …` | adresowanie surowym uchwytem widgetu; brak nazw ról |
| Light sleep z utratą USB | `RW1 SLEEP <s>` + `run_m8.s_light_sleep` | timer wake; runner pasywnie czeka na powrót portu i czyta `RW1 STATUS` |
| Statusy i raport | `run_m8.py` | PASS/FAIL/BLOCKED/PENDING, `report.json/.md`, `serial.log`, detekcja resetu po uptime |
| Zasilanie (1.85B) | `RG1 STATUS` | napięcie, prąd, flagi BQ27220 — **stan**, nie metoda pomiaru prądu |

## 2. Czego brakuje

| Brak | Decyzja |
|---|---|
| Odczyt + SHA-256 w LittleFS dla 64/512/1024 KiB i wpływ na system worker | **Firmware:** `RP1 FSBENCH PREP|READ|DEL <kib>` (patrz §3) |
| Jasność w protokole | Brak w żadnym protokole; stan GUI w `main.c`. Runner zapisuje wartość domyślną po starcie (60 %, [main.c:1279](../../main/main.c)) z adnotacją „ze źródła, bez odczytu”, a bazę zbiera zaraz po restarcie OTA bez dotyku. Bez zmiany firmware. |
| Okno czasów GUI/DMA na scenariusz | Runner: odczyt pierścienia `PERF <cursor>` przed/po scenariuszu, statystyki z iteracji, które w nim wypadły (32 ostatnie). Bez zmiany firmware. |
| Semantyczne `PRESS/VALUE` | Runner: mapa ról widgetów stopera z kolejności tworzenia (`view, dial, time, laps, lap` — [stopwatch.c:221](../../examples/stopwatch_elf/stopwatch.c)), weryfikowana przez `RE1 UI STATUS` (`widgets == 5`). |
| Wykrycie płytki | `run_m8` zgaduje płytkę po obecności RTC. Na 1.85B PCF85063 bywa niewidoczny ([bringup.md:435](../bringup.md)) — runner E0 wymaga jawnego `--board` i tylko zapisuje stan RTC. |
| Stan po zamknięciu | Runner: snapshot po `EXIT` stopera i powrocie do Home, delta do snapshotu Home. |
| Manager | Patrz §4. |

## 3. Firmware: `RP1 FSBENCH`

- `FSBENCH PREP <kib>` zapisuje `/storage/dev/e0-<kib>.bin` z deterministycznego wzorca (xorshift32, ziarno stałe, słowa little-endian). Odmowa, gdy runtime aplikacji jest zajęty, storage niedostępny albo brakuje miejsca z zapasem.
- `FSBENCH READ <kib>` czyta plik blokami 4 KiB przez VFS i liczy SHA-256 (PSA). Zwraca `read_us` (suma `read()`), `sha_us` (suma `psa_hash_*`), `total_us` (cała operacja = czas zablokowania system worker), `chunk_max_us` i skrót.
- `FSBENCH DEL <kib>` usuwa plik.
- Rozmiary tylko 64, 512, 1024. Działa w istniejącym system worker (właściciel VFS i USB), bez nowego tasku. Watchdog jest karmiony między blokami.
- Host liczy ten sam wzorzec i porównuje SHA-256 — to dowód poprawności odczytu.
- Wpływ na system worker: `total_us` to czas, przez który worker nie obsługuje innych zadań; dodatkowo `RS1 HEALTH stacks.system_worker` po pomiarze i liczniki `RP1 PERF` GUI przed/po.

## 4. Manager i runner

- **Decyzja:** automatyczny runner E0 powstaje w Pythonie (`tools/hardware_validation/run_e0.py`) na bazie `run_m8.py`: odkrycie po `/dev/serial/by-id`, monitor szeregowy, detekcja resetu, pasywne ponowne połączenie po light sleep. Uzasadnienie: działa bez przeglądarki i bez gestu użytkownika (Web Serial wymaga zgody na port), ma testy hosta z fałszywym urządzeniem i może być uruchamiany bez nadzoru.
- **Manager (propozycja, nie w tym etapie):** tryb deweloperski z tymi samymi operacjami (`HEALTH`, `PERF`, start/zamknięcie stopera, `SLEEP` z oczekiwaną utratą portu i ponownym otwarciem przez `navigator.serial` `connect`). Manager ma już korelację żądań; brakuje parserów RS1 HEALTH/RP1 PERF/RW1 i obsługi utraty portu. Odłożone, żeby nie dublować runnera przed ustaleniem formatu raportu.

Scenariusze runnera (kolejno):

1. `context` — commit, płytka (jawna), firmware i ELF SHA, RTC, zasilanie (`RG1` na 1.85B), USB = podłączone (runner działa przez USB), jasność ze źródła.
2. `home` — Home aktywne, odczekanie, snapshot (`HEALTH`, `PERF`, `STATUS capacity`).
3. `stopwatch` — `SELECT` zainstalowanego stopera, semantyczne `PRESS dial` (start), sprawdzenie zmiany tekstu, `PRESS dial` (stop), `PRESS lap` (reset), snapshot w trakcie.
4. `full_refresh` — `RP1 BENCH 120` na Home, wynik + snapshot.
5. `after_close` — `EXIT` stopera, powrót do Home, snapshot i delta względem `home`.
6. `littlefs` — dla 64/512/1024 KiB: `PREP`, 3 × `READ`, `DEL`; porównanie SHA z hostem.
7. `light_sleep` — `RW1 SLEEP <s>` z oczekiwaną utratą USB, pasywne czekanie, odczyt `RW1/RS1/NAV` po powrocie. Czas wake: **PENDING** (powrót USB nie jest pomiarem chwili wake).
8. PENDING: prąd (praca, zgaszony ekran, light sleep, szczyty wake/start), dotyk palcem, obraz, słyszalność, czas wake względem niezależnego zegara.

## 5. Format raportu `docs/baseline/e0-<płytka>.md`

- Nagłówek: data UTC, commit źródeł, wersja firmware, SHA-256 ELF, slot, płytka i numer seryjny USB, jasność (źródło), zasilanie i stan ładowania (źródło), USB podłączone, RTC.
- Tabela statusów pozycji bramki: PASS / FAIL / BLOCKED / PENDING z dowodem.
- Tabele wartości: heap wewn./PSRAM (bieżący, min, największy blok) per scenariusz; stosy; GUI/DMA (okno scenariusza, BENCH); LittleFS (wolne, odczyt/SHA/łącznie per rozmiar i powtórzenie).
- Ścieżka do surowego `report.json` (w `build-ci/`, niecommitowany) i wersja runnera.

## 6. Kontrola etapów

1. Firmware `FSBENCH` → kompilacja Xtensa 1.85B i 1.75C bez nowych ostrzeżeń.
2. Runner → `python3 -m unittest tests.test_run_e0` (fałszywe urządzenie) + istniejące `tests.test_run_m8`.
3. Baza → `run_e0.py` na podłączonej płytce; brak płytki = BLOCKED.
