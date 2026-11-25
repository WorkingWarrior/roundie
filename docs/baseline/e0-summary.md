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

- **Przyczyna Safe Mode `reason=core` przed sesją** jest nieznana: brak zapisu crash, reset power-on, zresetowany gauge i RTC. **Hipoteza:** pełna utrata zasilania. Wymaga sprawdzenia, czy utrata zasilania po STABLE powinna w ogóle prowadzić do Safe Mode.
- **RTC 1.85B ma stan `RTC_INVALID`** (czas nieustawiony po utracie zasilania). Nie ustawiano go, bo nie było to w zakresie.
- **Trwały spadek PSRAM o 131076 B po pierwszym uruchomieniu aplikacji.** Hipoteza: leniwie tworzony wykonawca; do potwierdzenia w kodzie i do ujęcia w budżecie pamięci.
- **Wpływ odczytu LittleFS na GUI pozostaje niezmierzony,** bo tarcza Home rzadko się przerysowuje. Propozycja: powtórzyć FSBENCH READ przy działającym stoperze.
- **Zapas stosu GUI wynosi 2256 B** po pełnym odświeżeniu. Wartość jest mała, ale nie mieści się w zakresie E0 (nie podnosimy limitów). Warto ją obserwować w następnych etapach.
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
