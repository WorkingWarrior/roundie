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
5. **FSBENCH READ 1 MiB przy działającym GUI:** system worker nie odpowiada przez cały odczyt (250 ms zamiast 10 ms). GUI i takty aplikacji zatrzymują się na 0,3–0,44 s (normalnie 100–110 ms). Iteracje, które się odbywają, mają normalny czas.
6. **Safe Mode:** reset `core` wymaga nieprawidłowego resetu po STABLE. Sama utrata zasilania ani brownout nie ustawiają latcha. Bezpośredniej przyczyny nie da się ustalić z zachowanych danych.

### 7.4. Co zmienia założenia roadmapy (roadmapy nie edytowano)

- **E6 (UI):** płynność trzeba planować przez małe obszary unieważniania, nie przez szybszy zegar QSPI. Pełnoekranowe animacje to przy 160 MHz realnie 10–17 kl./s, zależnie od widoku. Do zmierzenia przed decyzjami: CPU 240 MHz, rozdzielczość ticka pętli GUI (okresy to wielokrotności 10 ms przy `CONFIG_FREERTOS_HZ=100`), koszt tarczy Home.
- **E2 (pakiety, instalacja) i każda długa praca pamięci masowej:** synchroniczna praca w `system_worker` blokuje interaktywność na cały czas operacji. Odczyt, hash i kopiowanie trzeba dzielić na odcinki albo przenieść do osobnego workera, z kryterium przerwy GUI ≤ 130 ms.
- **Budżet pamięci:** pierwsze uruchomienie aplikacji trwale zajmuje 128 KiB PSRAM z wyrównaniem do 64 KiB. Budżet należy liczyć od stanu po pierwszym uruchomieniu.
- **Kontrakt zatrzymania aplikacji (runtime/SDK):** STOP zamyka admisję UI asynchronicznie wobec trwającego callbacku. Aplikacja, która traktuje odrzucenie `ui_update` jako błąd, kończy się wtedy widokiem błędu (raz na 9 prób w tej sekwencji, §8.6). Potrzebna jest decyzja o semantyce przed zamrożeniem ABI.

### 7.5. PENDING / BLOCKED

- **PENDING (sesja z użytkownikiem):** prąd i przyrząd pomiarowy, dotyk palcem, ocena wzrokowa, słyszalność, chwila wybudzenia względem niezależnego zegara (§6 wyżej).
- **PENDING (eksperyment):** mechanizm zatrzymania GUI przy FSBENCH. Hipotezy: priorytet `system_worker`, dostęp do Flash i cache albo wspólna konsola. Metoda i kryterium w e0-1.85b.md §8.5.
- **PENDING (decyzja):** semantyka odrzucenia `ui_update` podczas STOP (§8.6). Które dane diagnostyczne Safe Mode wdrożyć (§9.4: propozycje 1–4 bez zmian partycji; ring rozruchów wymaga decyzji; core dump do Flash wymaga repartycjonowania, czyli operacji destrukcyjnej).
- **BLOCKED:** cała baza i pomiary uzupełniające na 1.75C, bo płytka nie była podłączona.
