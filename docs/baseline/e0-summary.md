# E0 — podsumowanie sesji 2026-11-25

Gałąź `e0-baseline`. Bez push i bez merge do `main`. Szczegóły: [e0-plan.md](e0-plan.md), [e0-1.85b.md](e0-1.85b.md), [e0-1.75c.md](e0-1.75c.md).

## 1. Co zrobiono

| Etap | Wynik | Commit |
|---|---|---|
| 0. Roadmapa | `--clean` usuwa wszystkie wpisy OBSOLETE po pokazaniu listy; `--prune` bez zmian, app-data zostaje. Pomiary prądu i próby ręczne E0 odłożone (PENDING) z wymogiem wcześniejszego wyboru przyrządu i punktu pomiaru. i18n dodane jako zadanie wypełniające. | 86e7966 |
| 1. Inwentaryzacja i plan | `e0-plan.md`: co dają RS1 HEALTH, RP1 PERF/BENCH/STATUS, RW1, RG1 i `run_m8`; luki; format raportu | 1934d35 |
| 2. Diagnostyka | RP1 FSBENCH PREP/READ/DEL 64/512/1024 KiB (deterministyczny wzorzec, SHA-256 przez PSA, w system_worker, tylko build `rep`); RP1 HOME (lista → tarcza Home) | a6fa955 |
| 2. Runner | `tools/hardware_validation/run_e0.py`, 17 testów hosta | ab1889d |
| 3. Poprawki runnera po pierwszych przebiegach | czekanie na admission runtime; role stopera od uchwytu widoku; ekran trzymany włączony | 584dcde |
| 3. Baza 1.85B, raporty | `e0-1.85b.md`, `e0-1.75c.md`, to podsumowanie, README runnera | następny commit |

## 2. Weryfikacja (osobno wg rodzaju)

- **Testy hosta:** `python3 -m unittest tests.test_run_e0 tests.test_run_m8 tests.test_roundie_launcher tests.test_endurance tests.test_roundie_display` → 95 testów, OK (po commicie 584dcde).
- **Kompilacja Xtensa:** `tools/ci/build_firmware.sh <płytka> rep` dla `waveshare_s3_touch_lcd_1_85b` i `waveshare_s3_touch_amoled_1_75c` → sukces, 0 ostrzeżeń. Kod firmware nie zmienił się po tym buildzie; późniejsze commity dotyczą tylko runnera i dokumentacji.
- **Urządzenie (1.85B):** OTA obrazu `0.3.80-e0` → PASS (`ota_1`, VALID, ELF zgodny z buildem). Pełny przebieg `run_e0.py --light-sleep` → PASS, 0 nieoczekiwanych resetów.
- **Długie i fault-injection:** nie uruchamiane (poza zakresem).

## 3. Status pozycji bramki E0

| Pozycja | 1.85B | 1.75C |
|---|---|---|
| Heap wewn./PSRAM: bieżący, min, największy blok | PASS | BLOCKED |
| Zapas stosów | PASS | BLOCKED |
| Czasy GUI/DMA | PASS | BLOCKED |
| Wolne miejsce LittleFS | PASS | BLOCKED |
| Stoper: start i zamknięcie (wstrzyknięte) | PASS | BLOCKED |
| LittleFS odczyt + SHA-256 64/512/1024 KiB | PASS | BLOCKED |
| Light sleep, utrata USB, stan po powrocie | PASS | BLOCKED |
| Jasność, zasilanie, ładowanie, USB zapisane | PASS | BLOCKED |
| Prąd: ekran włączony, wyłączony, light sleep, szczyty | PENDING | PENDING |
| Dotyk palcem, obraz, słyszalność | PENDING | PENDING |
| Chwila wybudzenia vs niezależny zegar | PENDING | PENDING |

Kluczowe liczby 1.85B, szczegóły w [e0-1.85b.md](e0-1.85b.md):
- Heap: wewnętrzny wolny 151147 B, min 114608 B, największy blok 77824 B; PSRAM wolny 7119256 B.
- Stos GUI: zapas 2256 B.
- Pełne odświeżenie: 10,0 kl./s (render ok. 93 ms, DMA ok. 28 ms).
- Odczyt + SHA-256 1 MiB z LittleFS: 234 ms (odczyt ok. 4,9 MB/s).
- Light sleep 20 s: wybudzenie timerem, suspend 21 ms, resume 3 ms (zegar firmware).

## 4. Decyzje podjęte w sesji

1. **Wyjście z Safe Mode przez `RS1 REBOOT`** (udokumentowana ścieżka operatora, bez operacji destrukcyjnych). 1.85B była w Safe Mode `reason=core`, bez zapisu crash. Inaczej nie dało się zebrać bazy.
2. **Runner w Pythonie zamiast trybu deweloperskiego Managera.** Runner jest skryptowalny, testowalny na hoście i wykorzystuje `run_m8`. Tryb dev w Managerze zostaje propozycją.
3. **Obraz z `CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH=600`**, jak poprzedni obraz deweloperski, żeby nie zmieniać zachowania gauge w trakcie uczenia FCC.
4. **Jasność z kodu (60 %)**, oznaczona jako nieodczytana; RW1 nie ma pola jasności.
5. **Jawne `--board`.** PCF85063 na 1.85B bywa niewidoczny, więc autodetekcja przez RTC jest niewiarygodna.
6. **FSBENCH:**
   - tylko w buildzie `rep`, w istniejącym system_worker;
   - plik w `/storage/dev` z zapasem 128 KiB;
   - PREP/DEL odmawiają przy zajętym runtime, bo zapis Flash zatrzymuje cache kodu natywnego;
   - watchdog karmiony co 64 KiB;
   - wzorzec xorshift32 sprawdzany niezależnie na hoście.
7. **RP1 HOME** odpowiada gestowi przejścia z listy na tarczę; wcześniej nie było drogi diagnostycznej do Home.
8. **Ekran w scenariuszach:** RW1 SCREEN ON + inhibitor DISPLAY_AWAKE. Bez tego panel gasł po 30 s i baza Home mierzyła wygaszony ekran. Przed light sleep inhibitor jest zwalniany, a ekran wyłączany.
9. **Wznowienia:**
   - Pełny przebieg powtórzono raz, po poprawce runnera (błąd ról stopera). To nie było ponawianie nieodpowiadającej płytki.
   - Płytka odpowiadała przez cały czas.

## 5. Otwarte kwestie (do decyzji lub dalszej pracy)

- **Przyczyna Safe Mode `reason=core` przed sesją:** przeanalizowana w §7 i w [e0-1.85b.md §9](e0-1.85b.md). Bezpośrednia przyczyna nadal jest nierozstrzygnięta.
- **RTC 1.85B ma stan `RTC_INVALID`** (czas nieustawiony po utracie zasilania). Nie ustawiano go, bo nie było to w zakresie.
- **Trwały spadek PSRAM o 131076 B po pierwszym uruchomieniu aplikacji:** rozstrzygnięty w §7 (jednorazowy blok stosu wykonawcy).
- **Wpływ odczytu LittleFS na GUI:** zmierzony w §7. Odczyt zatrzymuje GUI, a mechanizm pozostaje otwarty.
- **Zapas stosu GUI:** w najgorszym zmierzonym przypadku 2128 B (§7). Nie jest to ryzyko, ale trzeba go obserwować w następnych etapach.
- **Tryb deweloperski Managera** (stoper, statusy, raport z GUI): nie wykonany, pozostaje propozycją.
- **1.75C** jest BLOCKED, bo płytka nie była podłączona. Trzeba też sprawdzić obsługę AXP2101 w runnerze.
- **„-dirty” w commicie źródeł raportu** pochodzi z niezacommitowanych w czasie przebiegu poprawek runnera (potem 584dcde) i z niezwiązanej zmiany użytkownika w `AGENTS.md`, której nie commitowano.

## 6. PENDING na sesję z użytkownikiem

1. **Wybór przyrządu i punktu pomiaru prądu,** osobno dla średniej w light sleep (zakres µA–mA, długie okno) i dla szczytów wake/start (szybkie próbkowanie). BQ27220/AXP2101 nie są metodą pomiaru.
2. **Prąd na obu płytkach:**
   - średnia przy włączonym ekranie (Home, stoper; jasność 60 %);
   - średnia przy wyłączonym ekranie;
   - średnia w light sleep;
   - szczyty przy wybudzeniu i starcie.
3. **Dotyk palcem:** stoper (start/stop/lap) i Home.
4. **Ocena wzrokowa:** obraz panelu i pełne odświeżenie.
5. **Słyszalność.**
6. **Chwila wybudzenia względem niezależnego zegara** (powrót USB nie jest pomiarem).
7. **Baza E0 na 1.75C** po podłączeniu płytki.

## 7. Kontynuacja 2026-11-25: pomiary uzupełniające 1.85B

Szczegóły i dane: [e0-1.85b.md §7–§9](e0-1.85b.md). Obraz `0.3.81-e0` (dodaje RP1 MEMPROBE tylko w buildzie deweloperskim), OTA na `ota_0`, VALID, 0 nieoczekiwanych resetów. Wejścia wstrzyknięte, nie dotyk.

### 7.1. Co zrobiono

| Punkt | Wynik | Commit |
|---|---|---|
| 7. Diff konfiguracji względem CI `rep` | jedyna różnica sdkconfig to `GAUGE_DESIGN_CAPACITY_MAH` 0 → 600; poza nią tylko `PROJECT_VER` | 5237444 |
| 1. Sonda pamięci | RP1 MEMPROBE i runner `run_e0_followup.py` | fbad70d |
| 6. Safe Mode przed sesją | analiza ścieżek, hipotez i propozycja danych diagnostycznych, bez zmian w logice | 4f84ca3, 28f3db0 |
| Runner | obsługa widoku ERROR launchera (brak migawki listy po starcie) | 5d86dee |
| Runner | pomiar przerwy GUI wokół FSBENCH; przerwa przed EXIT w worst_stack | b4622cb |
| 1–5. Wyniki | `e0-1.85b.md` §8 | 205bd24 |

### 7.2. Weryfikacja

- **Testy hosta:** `tests.test_run_e0_followup` i `tests.test_run_e0` → 29 testów, OK. Pełny zestaw wcześniej w sesji: 104 testy, OK.
- **Kompilacja Xtensa:** `tools/ci/build_firmware.sh` `rep` dla 1.85B i 1.75C po dodaniu MEMPROBE → sukces, 0 ostrzeżeń. Obraz E0 `0.3.81-e0` → 0 ostrzeżeń.
- **Urządzenie (1.85B):** przebiegi `followup-185b-2/3/4` → wszystkie scenariusze PASS w ostatnim przebiegu. Jedno niepowodzenie worst_stack w `-2` opisuje §8.6.
- **Długie, endurance i fault-injection:** nie uruchamiane.

### 7.3. Co rozstrzygnięto

1. **Sonda pamięci działa:** każda kontrolowana alokacja (1 KiB i 16 KiB wewn., 4 KiB i 128 KiB PSRAM) dała spadek rozmiar + 4 B i dokładny powrót po zwolnieniu. Wcześniejsze „0 B” wewnętrznej przy stoperze było prawdziwe. Pomiarów nie trzeba powtarzać.
2. **PSRAM −131076 B to jednorazowy blok, nie wyciek:** w 20 cyklach stopera zmiana wystąpiła tylko w pierwszym, potem 0 B/cykl, wszystkie zasoby zwolnione. Źródło: `components/roundie_elf/idf/executor_stack.c:90` (64 KiB strony ochronnej i 64 KiB stosu), celowo nigdy nie zwalniany (`:114`), tworzony leniwie z `executor.c:151`.
3. **BENCH mierzy rasteryzację CPU, nie magistralę:** przesył pełnej klatki trwa 19,42 ms, czyli tyle co teoretyczne minimum QSPI 19,44 ms przy 26,667 MHz. Render to 51 ms (lista), 56–62 ms (stoper) i 95 ms (Home). 10 kl./s to wynik tarczy Home, nie limit wyświetlacza.
4. **Stos GUI:** najgorszy zmierzony przypadek (BENCH na stoperze, nawigacja, BENCH na liście i Home) daje 2128 B zapasu, czyli powyżej progu ryzyka 1024 B.
5. **FSBENCH READ 1 MiB przy działającym GUI:** system worker nie odpowiada przez cały odczyt (250 ms zamiast 10 ms). GUI zatrzymuje się na 0,3–0,34 s (normalnie 100–110 ms). ~~Takty aplikacji zatrzymują się na 0,3–0,44 s~~ **błędne (§9):** takty wykonawcy nie stają; przerwa w logu to utrata linii na hoście. Iteracje, które się odbywają, mają normalny czas.
6. **Safe Mode:** reset `core` wymaga nieprawidłowego resetu po STABLE. Sama utrata zasilania ani brownout nie ustawiają latcha. Bezpośredniej przyczyny nie da się ustalić z zachowanych danych.

### 7.4. Co zmienia założenia roadmapy (stan przed §8; roadmapę zaktualizowano w §8)

- **E6 (UI):** płynność trzeba planować przez małe obszary unieważniania, nie przez szybszy zegar QSPI. Pełnoekranowe animacje to przy 160 MHz realnie 10–17 kl./s, zależnie od widoku. Do zmierzenia przed decyzjami: CPU 240 MHz, rozdzielczość ticka pętli GUI (okresy to wielokrotności 10 ms przy `CONFIG_FREERTOS_HZ=100`), koszt tarczy Home.
- **E2 (pakiety, instalacja) i każda długa praca pamięci masowej:** synchroniczna praca w `system_worker` blokuje interaktywność na cały czas operacji. Odczyt, hash i kopiowanie trzeba dzielić na odcinki albo przenieść do osobnego workera, z kryterium przerwy GUI ≤ 130 ms.
- **Budżet pamięci:** pierwsze uruchomienie aplikacji trwale zajmuje 128 KiB PSRAM z wyrównaniem do 64 KiB. Budżet należy liczyć od stanu po pierwszym uruchomieniu.
- **Kontrakt zatrzymania aplikacji (runtime/SDK):** STOP zamyka admisję UI asynchronicznie wobec trwającego callbacku. Aplikacja, która traktuje odrzucenie `ui_update` jako błąd, kończy się wtedy widokiem błędu (raz na 9 prób w tej sekwencji, §8.6). Potrzebna jest decyzja o semantyce przed zamrożeniem ABI.

### 7.5. PENDING / BLOCKED

- **PENDING (sesja z użytkownikiem):** prąd i przyrząd pomiarowy, dotyk palcem, ocena wzrokowa, słyszalność, chwila wybudzenia względem niezależnego zegara (§6 wyżej).
- **ROZSTRZYGNIĘTE w §8:** mechanizm zatrzymania GUI przy FSBENCH. Hipotezy: priorytet `system_worker`, dostęp do Flash i cache albo wspólna konsola. Metoda i kryterium w e0-1.85b.md §8.5.
- **PENDING (decyzja):** semantyka odrzucenia `ui_update` podczas STOP (§8.6). Które dane diagnostyczne Safe Mode wdrożyć (§9.4: propozycje 1–4 bez zmian partycji; ring rozruchów wymaga decyzji; core dump do Flash wymaga repartycjonowania, czyli operacji destrukcyjnej).
- **BLOCKED:** cała baza i pomiary uzupełniające na 1.75C, bo płytka nie była podłączona.

## 8. Mechanizm przestoju, wyścig STOP i historia rozruchów (2026-11-25, druga sesja)

Tryb autonomiczny na gałęzi `e0-baseline`: bez push i merge, z flashowaniem i testami automatycznymi, bez kasowania Flash, storage i NVS. Szczegóły i tabele: `e0-1.85b.md` §10–§12.

### 8.1. Commity

| Punkt | Zakres | Commit |
|---|---|---|
| 1. Przestój GUI | sondy przestoju, `FSBENCH XREAD/XWRITE` (build deweloperski), runner `run_e0_stall.py` z testami | 985a244 |
| 2. Wyścig STOP | `RP1 ELFDELAY` w wykonawcy (build deweloperski), bez zmiany runtime | 4fd6086 |
| 3. Safe Mode | pierścień 8 rozruchów w NVS (przyczyna resetu, Vbat przy starcie, licznik brownoutów), `RS1 HISTORY`, test hosta | 052c1cc |
| Runner | oczekiwanie na admisję FSBENCH po OTA; start stopera przed `PRESS lap` | d54d76a |
| Wyniki | `e0-1.85b.md` §10–§12, `safe-mode.md` „Boot history” | 3b3ffd8 |
| 4. Roadmapa | EABI: lifecycle STOPPING i reguła I/O; E6: bufory rysowania | 3a0b9fa |

### 8.2. Weryfikacja

- **Testy hosta:** pełny `unittest discover` daje 298 testów. Bez środowiska IDF były 2 błędy środowiskowe: brak `IDF_PATH` i `gen_esp32part`. Oba moduły powtórzone w środowisku IDF v6.1 → 19 testów, OK. Nowe testy: `test_boot_history` (ASan i UBSan), `test_run_e0_stall`.
- **Kompilacja Xtensa:** obraz E0 1.85B `0.3.82-e0`, `build_firmware.sh` 1.85B `legacy` i 1.75C `rep` → sukces, 0 ostrzeżeń. Wszystkie konfiguracje mają `ROUNDIE_DEV_PACKAGES=y`. Gałąź bez tej opcji nie była kompilowana. Commity pośrednie (985a244, 4fd6086) nie były budowane osobno, zbudowano tylko stan końcowy.
- **Urządzenie (1.85B):** OTA `0.3.82-e0` → PASS. `stall-185b-1`: odczyt PASS. Zapis i wyścig dały FAIL przez błędy runnera, poprawione w d54d76a. `stall-185b-2`: history, stall_write i race PASS. Zero nieoczekiwanych resetów, pliki testowe usunięte.
- **Długie, endurance, fault-injection, prąd:** nie uruchamiane.

### 8.3. Co rozstrzygnięto

1. **Mechanizm przestoju GUI to zagłodzenie CPU, nie cache.** Niepinowany `system_worker` (prio 4) wykonuje odczyt na CPU0, gdzie działa GUI (prio 1).
   - Przez cały odczyt 1 MiB (234 ms) GUI nie ma ani jednej iteracji. Zadania prio 20 na obu rdzeniach i prio 1 na CPU1 działają co tick.
   - Rozmiar kawałka (512 B–32 KiB) bez oddania CPU niczego nie zmienia. `vTaskDelay(1)` po kawałku daje 30 ms (4 KiB, czas ×11) albo 40 ms (16 KiB, czas ×2,8).
   - Przypięcie do CPU1 uwalnia GUI, ale głodzi rdzeń wykonawcy (241 ms).
2. **Zapis 64/512/1024 KiB** (na Home) daje przerwy GUI 33–51 ms. Nie wyłącza cache (`mmap_remain=0`). Trwa 0,26–0,45 s, 3,7–5,1 s i 7,7 s.
3. **Bufory rysowania LVGL** leżą w wewnętrznej SRAM z DMA (1.85B: 2 × 14 400 B). Kod GUI wykonuje się z PSRAM (XIP).
4. **Wyścig STOP vs `on_event`** występuje deterministycznie: z `ELFDELAY 300` 10 na 10 prób kończy się RESULT `[0,-1,-23]`, bez opóźnienia 0 na 10. Runtime bez zmian, poprawka należy do EABI.
5. **Historia rozruchów działa na urządzeniu.** Pierwszy wpis po OTA to `[1, ESP_RST_SW, 4105 mV]`. Logika Safe Mode bez zmian.

### 8.4. Decyzje podjęte w trybie autonomicznym

- **Próg przerwy GUI dla systemowego I/O: 50 ms** (zastępuje 130 ms z §7.4). To górna wartość zmierzona dla zapisu i 16 KiB z oddaniem CPU.
- Proponowany mechanizm to oddanie CPU najpóźniej po 20 ms pracy, z kawałkami do 16 KiB. Jego wariant czasowy jest eksperymentem bramki EABI. Przypięcie I/O do CPU1 odrzucone.
- Z §9.4 wdrożono tylko tani pierścień rozruchów: co najwyżej 2 małe zapisy NVS na rozruch, uszkodzony rekord pozostaje nietknięty. Rekord przyczyny latcha, kod miejsca restartu, kopia crash do NVS i próbka baterii w RTC pozostają propozycjami.
- `ELFDELAY` jest ograniczone do 1000 ms i istnieje tylko w buildzie deweloperskim, podobnie jak `FSBENCH X*`.

### 8.5. PENDING

- **Eksperyment (EABI):** wariant czasowy oddawania CPU w odczycie i hashu. Kryterium: przerwa GUI i sondy prio 1 na CPU1 ≤ 50 ms przy 1 MiB. Obniżenie priorytetu workera nie było mierzone.
- **Wyjaśnione w §9 (artefakt hosta):** przerwa 410–440 ms logów `rep_app` z §8.5 raportu (CPU1 nie był głodzony w nowych pomiarach). Hipoteza: synchroniczny log na wspólnej konsoli.
- **Próba ręczna:** brownout i licznik brownoutów na urządzeniu. Historia przy prawdziwym wejściu w Safe Mode.
- **Bez zmian:** prąd, dotyk, ocena wzrokowa i słyszalność (§7.5). Cała 1.75C nadal BLOCKED.
- **Uwaga do metody:** przy braku iteracji GUI raport `gui` pokazuje przerwę 0. Miarą jest wtedy sonda prio 1 na CPU0.

## 9. Domknięcie E0 na 1.85B (2026-11-25, trzecia sesja)

Tryb autonomiczny na gałęzi `e0-baseline`: bez push i merge, z flashowaniem (OTA) i testami automatycznymi, bez kasowania Flash, storage i NVS. Szczegóły: `e0-1.85b.md` §13–§15.

### 9.1. Commity

| Punkt | Zakres | Commit |
|---|---|---|
| 1. Inwentaryzacja tasków | tabela zadań FreeRTOS obrazu `rep` i `CONFIG_FREERTOS_HZ=100` (`e0-1.85b.md` §13) | 88b6c1f |
| 2. Model priorytetów | `FSBENCH X… L/L0` (pomocnik z prio 0, `XRESULT`) | cddc9bb |
| 2. Model priorytetów | poprawka `L` (core −1 wykonywany w workerze) + `e0-1.85b.md` §14 | 801998a |
| 2–3. Narzędzia | `RP1 TICKGAP`, `RP1 LOGLEVEL` (build deweloperski), scenariusze `lowprio` i `loggap` w `run_e0_stall.py`, testy | 227cfeb |
| 3. Przerwa w logach | `e0-1.85b.md` §15 i oznaczenie §8.5/§10 | 3ef9c14 |
| 4. CI bez trybu deweloperskiego | profil `rep-nodev` (`sdkconfig.nodev.defaults`), macierz CI, `gate.py`, `docs/ci.md` | 643d30e |
| 5. Roadmapa | EABI: model priorytetów I/O z progiem 33 ms, test wyścigu STOP z −23/−24, postęp instalacji w Managerze | 6bc8081 |

Zmiany runnera z punktów 2 i 3 weszły w jednym commicie (227cfeb), bo dotyczą tego samego pliku i były rozwijane równolegle.

### 9.2. Weryfikacja

- **Testy hosta:** `unittest discover` w środowisku IDF v6.1 → 315 testów, OK (1 pominięty). W tym `test_run_e0_stall` (11) i `test_ci_gate`.
- **Kompilacja Xtensa:** sześć konfiguracji CI (`build_firmware.sh`): 1.85B i 1.75C × `legacy`, `rep`, `rep-nodev` → rc=0, 0 ostrzeżeń. W obu `rep-nodev` żadna opcja `ROUNDIE_DEV_*`, `BOOT_GUARD_TEST` ani `OTA_TEST_UNCONFIRMED_RESET` nie jest ustawiona. Obraz deweloperski 1.85B `0.3.84-e0` → sukces. Workflow CI na GitHub nie był uruchamiany (brak push).
- **Urządzenie (1.85B):** OTA `0.3.84-e0` → `ota_0`, VALID. Przebiegi `lowprio-185b-3` (z jednym LIST w trakcie I/O) i `lowprio-185b-4` (bez LIST) po 2 powtórzenia → PASS i spójne. `loggap` 2 powtórzenia → PASS. Pliki testowe usunięte, 0 nieoczekiwanych resetów. Przebiegi `lowprio-185b-1` (błąd `L`, poprawiony) i `-2` (przeciążenie przez odpytywanie bez przerw; runner poprawiony) → FAIL, odrzucone.
- **Długie, endurance, fault-injection, prąd:** nie uruchamiane.

### 9.3. Co rozstrzygnięto

1. **Zadania i zegar (fakt z kodu i obrazu):** pełna tabela w §13. GUI to zadanie główne z prio 1 na CPU0, `system_worker` ma prio 4 bez przypięcia, wykonawca prio 1 na CPU1. Tick wynosi 10 ms.
2. **I/O poniżej GUI działa (fakt):** pomocnik z prio 0 bez `vTaskDelay` daje przerwę GUI 20–30 ms przy odczycie i 10,5 ms przy zapisie. Sonda prio 1 na CPU1 ≤ 14 ms. `system_worker` odpowiada w ≤ 13,4 ms (wcześniej 0,69 s i 4,27 s). Odczyt jest szybszy od wariantu „16 KiB + oddanie CPU” (2,5 wobec 1,6 MiB/s), zapis wolniejszy (83–98 wobec 122 KiB/s).
3. **Ryzyko inwersji jest realne (fakt + hipoteza mechanizmu):** jedno wywołanie LittleFS z `system_worker` (RP1 LIST) w trakcie zapisu z prio 0 dało przerwę GUI 145–228 ms. Mutex `esp_littlefs` jest wspólny i dziedziczy priorytet. GUI nie wywołuje VFS. Dokładny przebieg (dziedziczenie vs własny czas CPU LIST) nie był śledzony planistą.
4. **Przerwa 410–440 ms w logach `rep_app` to artefakt hosta (fakt):** takty wykonawcy co 110 ms przy logach INFO i WARN, przy porcie zamkniętym, otwartym i w trakcie odczytu 1 MiB. Linie giną, gdy runner zamyka port. H3 obalona.
5. **Zawyżone lub błędne wartości E0:**
   - „odstęp logów `rep_app` 410–440 ms” (§8.5) i „takty aplikacji zatrzymują się” (§8.5, tu §7.3 pkt 5): **błędne**;
   - czasy odpowiedzi przez `self.call` (np. RS1 STATUS 10,2 ms w spoczynku): zawyżone o ok. 3–4 ms otwarcia portu, rzeczywiście 6,6 ms;
   - czasy na urządzeniu (io_us, sha_us, GUI, sondy, przerwy GUI 308/339 ms): **bez zmian**.

### 9.4. Decyzje podjęte w trybie autonomicznym

- **`io_worker` z prio 0 bez przypięcia** wykonuje cały dostęp do `/storage`. `system_worker` wykonuje tylko krótkie operacje i nie bierze blokad LittleFS; korzysta ze stanu w pamięci. Wybór bez przypięcia ze względu na odczyt (2,5 MiB/s) i prostotę; przypięcie do CPU0 (zapis +17%) zostaje otwartą optymalizacją.
- **Rozmiar wywołania VFS w `io_worker`: 4 KiB**, bo ogranicza czas trzymania blokady LittleFS (`chunk_max` 58–80 ms wobec 184–240 ms przy 16 KiB) przy tej samej przepustowości ±5%.
- **Próg przerwy GUI: 33 ms** (jedna klatka LVGL, `CONFIG_LV_DEF_REFR_PERIOD=33`). Zastępuje 50 ms z §8.4. Ten sam próg dla sondy prio 1 na CPU1 i odpowiedzi `system_worker`.
- **Profil CI `rep-nodev`** jako warstwa `sdkconfig.nodev.defaults` nad `rep`, przez zmienną `ROUNDIE_EXTRA_DEFAULTS`. Nie zmienia domyślnych buildów ani formatu obrazu.
- `TICKGAP` i `LOGLEVEL` tylko w buildzie deweloperskim (`CONFIG_ROUNDIE_DEV_PACKAGES`).
- Dokument przebiegu lowprio podaje zakres z 2 powtórzeń; przebiegi FAIL z błędów runnera odrzucono, a nie uśredniono.

### 9.5. PENDING i otwarte

- **Eksperyment (EABI, przy implementacji `io_worker`):** test akceptacyjny `run_e0_stall.py --only lowprio` na docelowym `io_worker` z rejestrem z pamięci. Kryterium: przerwa GUI, sonda prio 1 CPU1 i odpowiedź `system_worker` ≤ 33 ms, także ze skanem rejestru w trakcie zapisu.
- **Niezweryfikowane:** mechanizm inwersji (ślad planisty); mechanizm gubienia linii konsoli (`TX_FLUSH_TIMEOUT_US`, hipoteza).
- **Uwaga do raportu CI:** lokalny raport `rep-nodev` nie zawiera linii „Roundie layout”, bo katalog builda był już skonfigurowany i komunikat CMake nie został wypisany ponownie. W czystym CI wystąpi.
- **PENDING (próby fizyczne):** prąd i przyrząd, dotyk palcem, ocena wzrokowa, słyszalność, chwila wybudzenia vs niezależny zegar, brownout na urządzeniu (§7.5, §8.5).
- **PENDING (decyzja):** dane diagnostyczne Safe Mode poza pierścieniem rozruchów (§8.4).
- **BLOCKED:** cała baza i pomiary uzupełniające na 1.75C.

### 9.6. Ocena: E0 dla 1.85B

**E0 dla 1.85B uważam za zamknięte**, z wyłączeniem prób fizycznych (PENDING) i całej 1.75C (BLOCKED). Wszystkie pozycje bramki mierzalne automatycznie mają PASS (§3). Otwarte pytania E0 (mechanizm przestoju GUI, wyścig STOP, przerwa w logach, Safe Mode na tyle, na ile pozwalają zachowane dane) są rozstrzygnięte albo przeniesione jako decyzje i testy akceptacyjne do EABI w roadmapie. CI kompiluje warianty z trybem deweloperskim i bez niego dla obu płytek. Zamknięcie nie obejmuje decyzji o semantyce STOP (EABI) ani implementacji `io_worker` (EABI/E2); to praca następnych etapów, nie braki bazy.

## 10. Domknięcie E0: tabela braków i ich rozstrzygnięcie (2026-11-26, gałąź `e0-baseline`)

Zasada domknięcia: wszystko, co da się zrobić bez 1.75C i bez fizycznej obecności użytkownika, zostało zrobione. Pozostają tylko BLOCKED (1.75C) i PENDING (próby fizyczne).

### 10.1. Tabela braków (A1)

| # | Brak z §5–§9 | Klasa | Rozstrzygnięcie | Dowód |
|---|---|---|---|---|
| 1 | Runner nie obsługuje 1.75C (AXP2101, tożsamość płytki, kontekst ekranu i zasilania) | zrobić teraz | RG1 BATTERY z nazwą płytki i danymi AXP2101/BQ27220, RW1 `brightness`, runner v2, `e0_board.py` | 864fc93; testy hosta. Na 1.75C BLOCKED |
| 2 | Runner czy tryb deweloperski Managera | decyzja | runner w Pythonie, roadmapa E0 | 01d9ab7 |
| 3 | Dane diagnostyczne Safe Mode (§9.4 e0-1.85b, propozycje 1, 2, 4) | zrobić teraz | rekord RTC sesji (miejsce restartu, bateria), rekord przyczyny latcha w osobnym commicie NVS, kopia crash w NVS z limitem 256 B i rezerwą 126 wpisów | 469472c, e5746f8; test hosta (izolacja latcha, pełne NVS); 1.85B: PASS, +17/504 wpisów |
| 4 | Core dump w partycji Flash | nie robić | wymaga repartycjonowania, czyli operacji destrukcyjnej | `safe-mode.md` |
| 5 | Kasowanie latcha po power-on | nie robić | zmienia logikę rozruchu, a A4 wymaga logiki bez zmian | — |
| 6 | RTC 1.85B `RTC_INVALID` | zrobić teraz | RTC jest ważny, retencja przez restart programowy PASS. Błąd w `roundie_time.py verify --restart` (wyścig z publikacją płytki) poprawiony | 8cfa79f; `build-ci/e0/a5-rtc-verify-3.txt` |
| 7 | Mechanizm inwersji LIST (e0-1.85b §14) | zrobić teraz | LIST bez I/O zajmuje CPU0 przez cały skan, 122 ms przerwy GUI | 04834ab; e0-1.85b §14 |
| 8 | Mechanizm gubienia linii konsoli (§9.5) | zrobić teraz (źródło) | IDF v6.1 `usb_serial_jtag_vfs.c`, `usbjtag_tx_char_via_driver`: pierwszy bajt, który się nie mieści, czeka do `TX_FLUSH_TIMEOUT_US` = 50 ms, potem znaki są odrzucane bez czekania aż do udanego zapisu. Roundie używa ścieżki ze sterownikiem (`main.c` `usb_serial_jtag_vfs_use_driver`). Fakt ze źródła, zgodny z obserwacją §15 | kod IDF |
| 9 | Linia „Roundie layout” w raporcie `rep-nodev` | zrobić teraz | czysty `reconfigure` `rep-nodev` wypisuje `Roundie layout: rep; image: roundie_rep.bin`, 0 opcji `ROUNDIE_DEV_PACKAGES=y` | ten przebieg |
| 10 | Instrukcja jednego polecenia dla 1.75C | zrobić teraz | `e0-1.75c.md`: `e0_board.py --board 1.75c`, `--serial`/`--port`, warunek RO1 | c4c67df |
| 11 | Test akceptacyjny `io_worker` (§9.5) | EABI | wymaga `io_worker`, czyli implementacji etapu EABI (P1) | roadmapa |
| 12 | Semantyka odrzucenia `ui_update` przy STOP (§8.6) | EABI | decyzja przed zamrożeniem ABI (P2) | roadmapa |
| 13 | Prąd, dotyk palcem, obraz, słyszalność, chwila wybudzenia, brownout | PENDING | próby fizyczne | §6 |
| 14 | Cała baza E0 na 1.75C | BLOCKED | płytka niepodłączona | `e0-1.75c.md` |

### 10.2. A6: weryfikacja (stan na 2026-11-26)

- Kompilacja: 6 konfiguracji (1.85B i 1.75C × `legacy`, `rep`, `rep-nodev`), rc=0, 0 ostrzeżeń. Obraz E0 `0.3.86-e0`: 0 ostrzeżeń.
- Testy hosta: `tools/ci/run_tests.py --check-coverage`, 333 testy, 0 problemów z pokryciem.
- Urządzenie 1.85B (`0.3.86-e0`, `build-ci/e0/a6-185b-gate/`):
  - base z light sleep: PASS;
  - stall: PASS;
  - **followup: FAIL w `worst_stack`** (2 z 2 przebiegów). Stoper kończy się sam wynikiem `-24` (`show_state`, odrzucone `set()`) przed EXIT, więc to nie jest wyścig STOP. Na `0.3.84-e0` ten scenariusz przeszedł. Analiza i następny krok: `docs/progress.md` A6.
- E0 nie jest jeszcze gotowe do publikacji (A7), dopóki ten FAIL nie zostanie wyjaśniony.
