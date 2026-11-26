# Roadmapa Roundie do 1.0

Plan rozwoju platformy po audycie Phosphora. Stan odniesienia dla faktów z kodu to `e4a3041`. Opisane niżej kontrakty, limity i progi bramek są propozycjami wdrożeniowymi. Wyniki będą zapisywane przy odpowiednim etapie wraz z wersją firmware, wersją aplikacji i płytką.

## Zasady dostaw

- Każdy enabler dostarczamy z najmniejszą działającą aplikacją, która go używa. Istniejący przykład wystarcza, jeśli sprawdza cały nowy kontrakt. Nagłówek przyszłej usługi powstaje z pierwszym konsumentem.
- Raport rozdziela fakty ze źródeł, decyzje, założenia i eksperymenty. Wyniki hosta, kompilacji Xtensa, prób na 1.85B i 1.75C oraz prób długich lub z wstrzykiwaniem błędów mają osobne statusy. Kompilacja potwierdza zgodność narzędziową. Bramka sprzętowa wymaga działania aplikacji na 1.85B oraz na 1.75C, jeśli etap jej dotyczy.
- Brak pomiaru, płytki lub wymaganej interakcji oznacza **PENDING**. Wstrzyknięcie zdarzenia przez USB sprawdza logikę po stronie urządzenia. Dotyk, obraz na panelu, słyszalność i prąd wymagają własnych prób.
- Każdy etap raportuje deltę względem E0 tej samej płytki, przy porównywalnych warunkach. Etapy dotyczące snu, IMU lub sieci mają bramkę prądową. Pomiar długi i działania na sprzęcie wymagają autoryzacji właściwej dla danego etapu.
- Do 1.0 firmware obsługuje jedną bieżącą wersję ABI i jedną bieżącą wersję pakietu. Zmiana publicznego kontraktu podbija wspólny numer ABI, a zmiana layoutu kontenera podbija wersję formatu. CI przebudowuje przykłady. Samo dołożenie usługi w ramach istniejących konwencji nie wymaga sztucznej zmiany layoutu. Kontrakty zamrażamy przy 1.0.
- Limity widgetów, alokacji, danych, kodu i kolejek wynikają z pomiarów i są niezależne od wersji kontenera. Zmiana ABI nie podnosi ich automatycznie.
- Jeden AppExecutor, bez prywatnych tasków i kodu ELF działającego w tle. I/O wykonują systemowi workerzy, a LVGL należy do GUI. Uchwyt ma typ, właściciela i generację. Współdzielenie kodu z desktopem zachowuje te same granice.

## E0 — baza obu płytek i runner

**Fakt.** Istnieją `RS1 HEALTH`, `RP1 PERF/BENCH` i skrypty prób sprzętowych. `BENCH` mierzy pracę GUI, z pomiarami DMA w diagnostyce, lecz sam nie mierzy scanoutu. Manager ma już korelację żądań po Web Serial. [Diagnostyka GUI](../main/gui_diagnostics.c) · [Runner sprzętowy](../tools/hardware_validation/README.md)

**Zakres.** Uruchamianie i zamykanie istniejącego stopera, semantyczne `PRESS/VALUE`, statusy, raport i kontrolowany scenariusz light sleep. Wykonawcą jest runner Pythona (`tools/hardware_validation/run_e0.py`, jedna komenda na płytkę: `e0_board.py --board 1.85b|1.75c`), a nie tryb deweloperski Managera. Użyć istniejących protokołów i procedur testowych.

**Decyzja (domknięcie E0, 2026-11-26): runner zamiast trybu deweloperskiego Managera.** Uzasadnienie:
- runner działa bez obecności użytkownika, a Web Serial wymaga gestu użytkownika przy każdym otwarciu portu;
- obsługuje oczekiwaną utratę USB w light sleep i powrót przez ponowne otwarcie ścieżki `/dev/serial/by-id`;
- jego logika ma testy hosta w CI (`tests/test_run_e0*.py`, `tests/test_e0_board.py`);
- raporty JSON zapisuje w repozytorium roboczym obok obrazu i commitu.

Manager zachowuje swoją rolę (instalacja, pakiety, w EABI prezentacja formatu i wymagań). Tryb deweloperski Managera nie jest warunkiem żadnej bramki. Jeśli okaże się potrzebny do prób z użytkownikiem (PENDING), zostanie dodany jako cienka nakładka na te same protokoły. W scenariuszu snu tolerować oczekiwaną utratę USB, a po powrocie odczytać stan urządzenia. Pomiary czasu wake opierać na niezależnym odniesieniu.

**Bramka.** Dla Home, stopera, pełnego odświeżania i stanu po zamknięciu zebrać bieżący i minimalny wolny heap wewnętrzny/PSRAM, największe bloki, zapas stosów, czasy GUI/DMA i wolne miejsce LittleFS. Zmierzyć prąd średni i szczytowy w pracy, przy zgaszonym ekranie i w light sleep. Na obu płytkach zapisać jasność, rodzaj zasilania, stan ładowania i podłączenie USB. Zmierzyć osobno odczyt, SHA-256 i czas łączny dla 64, 512 i 1024 KiB danych testowych w LittleFS, ze szczególnym uwzględnieniem wpływu na system worker.

**Odłożone pomiary.** Pomiary prądu i próby fizyczne (dotyk palcem, ocena obrazu, słyszalność, czas wake względem niezależnego zegara) są w pierwszym przebiegu E0 oznaczone jako **PENDING**. Przed ich zebraniem trzeba ustalić przyrząd i punkt pomiaru, osobno dla średniej w light sleep i dla szczytów przy wake i starcie, bo wymagają innego zakresu i próbkowania. Odczyty BQ27220 i AXP2101 nie są metodą pomiarową prądu dla tej bramki.

**Zależności i nakład.** Brak zależności, **M**. Ryzyka to porównywalność pomiarów LCD i AMOLED oraz pomylenie czasu wybudzenia z ponownym pojawieniem się USB.

## EABI — ABI 4, minimalny RPK2 i reinstalacja

**Zakres.** Jeden lifecycle i wspólne typy, uchwyty i zdarzenia w `include/rep/`. Przyjąć główne tablice core, UI, audio oraz istniejące storage. W audio uwzględnić stan i powód zakończenia. Używać modułowych getterów usług oraz jednego numeru ABI. E2 dostarcza nowe gesty i pierścień w głównym UI, a E4c wdraża arbitraż audio.

Zdarzenie ma proponowane 64 B. Pola obejmują usługę i rodzaj zdarzenia, flagi, uchwyt z generacją, rewizję, długość payloadu, korelację operacji lub gestu, czas monotoniczny oraz 24 B danych. ABI wynika z kontekstu aplikacji. 24 B mieszczą sześć wartości IMU po 32 bity, a także typowe wyniki UI, audio i HTTP. Większe odpowiedzi oraz stan odtwarzany po ponownym starcie mają ograniczony odczyt z usługi. Routing sprawdza import oraz aktywną subskrypcję lub operację, właściciela i generację. Ścieżka lifecycle zachowuje pierwszeństwo.

**Fakt i decyzja dotycząca importów.** Walidator Xtensa sprawdza nazwę i ABI przy `SHN_UNDEF` bez rozróżnienia `STB_WEAK` i `STB_GLOBAL`. Resolver runtime dostaje tylko nazwę. Loader IDF odmawia przy nierozwiązanym symbolu w używanej ścieżce relokacji. Walidator x86-64 jawnie wymaga `STB_GLOBAL` dla niezdefiniowanego importu. Słaby symbol mógłby opisać opcjonalność całej nazwy po zmianie loaderów. Manifest dodatkowo wyraża `feature_mask` i pozwala Managerowi porównać wymagania z capabilities urządzenia przed transferem. Zachowujemy manifest jako deklarację wymagań i zweryfikowane importy jako deklarację użycia SDK. Firmware sprawdza zgodność obu przy instalacji i uruchomieniu. Kontrola importów dotyczy kontraktu SDK zaufanego ELF. [Xtensa](../components/roundie_elf/validate.c) · [Resolver](../components/roundie_elf/manager.c) · [x86-64](../components/roundie_elf/validate_x86_64.c)

**RPK2.** Bieżący kontener zawiera jeden ELF. Minimalne metadane to format, ABI, target, długość i SHA-256 ELF, ID, jedna nazwa UTF-8, wersja aplikacji i ograniczona lista twardych wymagań `(service_id, feature_mask)`. Builder jest deterministyczny. Parser sprawdza kanoniczne pola, kolejne offsety wyliczane z layoutu, dokładną długość i uporządkowane wymagania bez duplikatów. Padding wynika z wyrównania. Katalog sekcji i ikony trafią do formatu wraz z pierwszym konsumentem, domyślnie w E6. Projekt podpisów jest częścią przygotowania 1.0.

Instalacja weryfikuje pełny SHA-256 i ELF w stagingu, zapisuje nowy artefakt i atomowo przełącza rekord aktywacji. Rekord przechowuje pełny skrót jako identyfikator instalacji i skrót metadanych jako kotwicę. Skan sprawdza strukturę i zakotwiczone metadane. Start dodatkowo czyta, haszuje i waliduje wybrany ELF. Budżet obejmuje użycie całego LittleFS, miejsce na staging, narzut commitu i wspólną rezerwę. Manager pokazuje wolne i wymagane miejsce, ABI, target, wymagania, deklarowane usługi oraz powód odmowy. Instalacja pokazuje postęp (zapisane bajty / długość pakietu) i pozwala anulować przed przełączeniem rekordu aktywacji. **Fakt (E0 §14):** zapis na LittleFS osiąga 121–122 KiB/s z workera i 83–98 KiB/s z `io_worker` poniżej GUI, czyli ok. 4–6 s na 512 KiB i ok. 8–12 s na 1 MiB. Bez postępu taka instalacja wygląda jak zawieszenie.

**Rejestr i dane.**

- Firmware używa jednego `/storage/packages`. Skan rozpoznaje identyfikator katalogu i klasyfikuje wpis, którego rekord aktywacji lub kontener nie daje się odczytać według bieżącego formatu, jako `OBSOLETE`. Wpis w bieżącym formacie z błędnym hashem jest `CORRUPT`. Błąd I/O zgłasza błąd odczytu, żeby chwilowa niedostępność nie udawała wpisu do sprzątnięcia.
- Instalator może zastąpić `OBSOLETE` pod tym samym ID po pełnej walidacji nowego pakietu, także przy braku porównywalnej starej wersji. `REMOVE` może jawnie usunąć taki wpis. Skan i start pozostają operacjami odczytu, także po potwierdzeniu OTA. W razie automatycznego rollbacku przed potwierdzeniem stary firmware zastaje swój rejestr. Po ręcznym powrocie ze zmienionym rejestrem trzeba ponownie zainstalować starsze pakiety. Obecny stary skaner klasyfikuje nieczytelne wpisy jako `CORRUPT` i jego instalator wymaga wcześniejszego jawnego `REMOVE` dla kolidującego ID. [PackageManager](../components/roundie_packages/manager.c) · [Polityka OTA](ota.md)
- `rep_storage` używa `/storage/app-data/<id>`, początkowo pustego. Dane są identyfikowane przez ID aplikacji. Wersję schematu nosi payload i interpretuje aplikacja. Dalsza reinstalacja, zmiana formatu i usunięcie pakietu zachowują ten magazyn. Dawne bloby przy RPK1 nie są importowane do nowego magazynu.

**Ponowna instalacja.** Skrypt `reinstall-all <katalog-CI>` weryfikuje cały zestaw, ABI, target i unikalność ID przed pierwszą zmianą. Instaluje sekwencyjnie przez staging, potwierdza wynik każdego pakietu i pomija identyczny aktywny artefakt. Raport pokazuje wynik częściowy i umożliwia ponowienie. Domyślnie zastępuje `OBSOLETE` atomowo przy instalacji tego samego ID. `--clean` usuwa przed instalacją wszystkie wpisy `OBSOLETE`, także o ID spoza zestawu, co może zwolnić miejsce na staging. Wpisy w bieżącym formacie o ID spoza zestawu zostają, dopóki użytkownik nie poda `--prune`. Przed usuwaniem skrypt pokazuje listę ID. Obie flagi zachowują `/storage/app-data`.

**Lifecycle zatrzymania.** Stany instancji to RUNNING → STOPPING → UNLOADED. STOP zlecony przez system (EXIT, przełączenie aplikacji, Safe Mode) przełącza instancję w STOPPING. W tym stanie operacje UI i inne operacje usług zwracają udokumentowany, niekrytyczny kod „zatrzymywanie”. Aplikacja może go zignorować, a SDK nie traktuje go jak błędu. Callback, który już trwa albo ma pobrane zdarzenie, kończy się normalnie. Nowe zdarzenia poza ścieżką lifecycle nie są dostarczane. Wynik aplikacji po STOP zleconym przez system nie jest pokazywany jako błąd. Wyjątkiem jest crash, przekroczenie limitu czasu albo błąd zwolnienia zasobów, które launcher nadal pokazuje. **Fakt (E0, `baseline/e0-1.85b.md` §8.6, §11):** dziś STOP zamyka admisję UI asynchronicznie wobec wykonawcy. Odrzucony `ui_update` stoper zwraca jako −23/−24, a runtime pokazuje RESULT z błędem. Z diagnostycznym opóźnieniem dostarczenia (`RP1 ELFDELAY 300`, build deweloperski) dzieje się tak w 10 z 10 prób. Odrzucenie trafia w różne wywołania UI, zależnie od tego, w którym miejscu callbacku nastąpi zamknięcie admisji. Zaobserwowano −23 (`set_text` etykiety okrążeń w `show_laps`) i −24 (pierwsze z czterech wywołań w `show_state`), [stoper](../examples/stopwatch_elf/stopwatch.c). Ten scenariusz (`run_e0_stall.py --only race`) jest testem regresji EABI. Test obejmuje odrzucenie w każdym z tych miejsc, a nie tylko w pierwszym wywołaniu callbacku. Opóźnienie dostarczenia przesuwa moment zamknięcia admisji przez kolejne wywołania UI, a raport zlicza kody wyniku. Kryterium: 0 widoków RESULT w 100 powtórzeniach z opóźnieniem i bez niego, przy czym każdy kod odrzucenia zaobserwowany przed poprawką (−23, −24) musi wystąpić w przebiegu z opóźnieniem jako obsłużony STOP.

**Backpressure UI (P2).** Kolejka poleceń UI ma 12 pozycji i GUI opróżnia ją raz na iterację. Gdy jest pełna, `ui_create`/`ui_update`/`ui_destroy` zwracają udokumentowany, niekrytyczny kod „zajęte”. Ten kod różni się od kodu „zatrzymywanie” i od błędu żądania. Dziś wszystkie trzy przypadki zwracają −1 i aplikacja nie odróżni przyczyny. Odrzucenie „zajęte” nie zmienia stanu widżetu. Aplikacja ponawia je później (np. przy następnym ticku timera) albo rysuje cały stan od nowa. SDK nie traktuje tego kodu jak błędu. Diagnostyka `RW1 STATUS` zlicza odrzucenia według przyczyny (`rejected_full`, `rejected_admission`, `rejected_request`) oraz podaje ostatnią przyczynę, głębokość kolejki i wiek ostatniego opróżnienia przez GUI. **Fakt (E0 A6, `docs/progress.md`):** w `worst_stack` STOP i reset obsłużone 10 ms po sobie, przed kolejną iteracją GUI, dają 13 poleceń. Odrzucenie trafiło na 13. polecenie (`reject_reason=FULL`, `reject_depth=12`, ostatnie opróżnienie 26 ms wcześniej), stoper zwrócił −24 i został wyładowany. Ten sam błąd występuje na 0.3.84-e0 i 0.3.86-e0, ze starym i nowym runnerem. To defekt kontraktu, który ujawnił timing, a nie regresja. Do czasu EABI stoper traktuje odrzucenie jako „nieaktualny widok” i rysuje cały stan przy następnym ticku. Za błąd uznaje dopiero odrzucenie trwające 50 ticków (5 s). Test regresji: blok „Backpressure” w `tests/test_app_round.c` (`stopwatch_runtime_tests`, uruchamiany przez `tests/test_app_ui.py`). Przepełnia kolejkę STOP-em i resetem przed opróżnieniem i wymaga, żeby aplikacja działała dalej, a następny tick odtworzył stan wyzerowany. W EABI test dodatkowo sprawdza kod „zajęte”.

**Model priorytetów I/O.** Decyzja na podstawie E0 §10 i §14. Zastępuje regułę „kawałki 16 KiB + `vTaskDelay`”.
- `io_worker`: zadanie z prio 0, czyli poniżej GUI (prio 1), bez przypięcia. Wykonuje wszystkie długie operacje na `/storage`: odczyt, hash, zapis, instalację i usuwanie pakietów, skan rejestru, `esp_littlefs_info`, odczyt tarcz i blobów aplikacji. Nie oddaje CPU jawnie, bo wywłaszcza je każde zadanie wyższego priorytetu. Rozmiar pojedynczego wywołania VFS jest ograniczony do 4 KiB, bo ogranicza czas trzymania blokady LittleFS (`chunk_max` 58–80 ms przy 4 KiB i 184–240 ms przy 16 KiB).
- `system_worker` (prio 4) pozostaje nad GUI, ale wykonuje tylko krótkie operacje: polecenia sterujące, stan, kolejkowanie zadań do `io_worker` i odbiór wyników. **Nie bierze blokad LittleFS ani VFS `/storage`.** Pojemność, listę pakietów i stan instalacji czyta z pamięci, uzupełnianej przez `io_worker`.
- **Fakt (E0 §14):** w `io_worker` z prio 0 GUI nie stoi, a `system_worker` odpowiada w ≤ 13,4 ms zamiast 0,69 s (odczyt) i 4,27 s (zapis). Wyniki przy odczycie 1 MiB: przerwa GUI 20–30 ms, 2,5 MiB/s. Wyniki przy zapisie: przerwa 10,5 ms, 83–98 KiB/s (wariant z oddawaniem CPU: 122 KiB/s, przerwa do 51 ms). Jedno wywołanie LittleFS z `system_worker` w trakcie zapisu `io_worker` dało 145–228 ms przerwy GUI. Mutex `esp_littlefs` ma dziedziczenie priorytetu, więc `io_worker` przejmuje prio 4 na czas wywołania. Stąd zakaz dostępu do `/storage` poza `io_worker`.
- **Próg.** Przerwa GUI ≤ **33 ms**, czyli jeden okres odświeżania LVGL (`CONFIG_LV_DEF_REFR_PERIOD=33`, ok. 30 kl./s). Taka przerwa gubi co najwyżej jedną klatkę, czego przy animacji i przewijaniu nie widać jako szarpnięcia. Ten sam próg dotyczy sondy prio 1 na CPU1 (wykonawca) i odpowiedzi `system_worker` na polecenie sterujące w trakcie I/O. Poprzedni próg 50 ms dopuszczał utratę dwóch klatek.
- Przypięcia I/O do CPU1 z prio nad wykonawcą nie stosujemy, bo głodzi wykonawcę (241 ms). Przypięcie `io_worker` do CPU0 (`L0`) daje zapis 98 zamiast 84 KiB/s, ale odczyt 1,7 zamiast 2,5 MiB/s. To otwarta optymalizacja, nie wymóg. Mapowanie Flash (`mmap_remain=1`) wymaga ponownego pomiaru zapisu, bo zapis wyłączy wtedy cache obu rdzeni.
- Test akceptacyjny: `run_e0_stall.py --only lowprio`, czyli odczyt 1 MiB przy działającej aplikacji i zapis 512 KiB z odpytywaniem `system_worker` co 200 ms, w tym jeden skan rejestru w trakcie zapisu. Kryterium: wszystkie trzy wielkości ≤ 33 ms.

**Wpięcie.** Nagłówki i runtime REP, oba walidatory ELF, PackageManager, integracja storage w firmware i na Linuksie, Manager, builder, USB i CI. Przebudować istniejące przykłady. `package-format.md` opisuje nowy format. Tarcze RWF zachowują własny format i katalog.

**Bramka.** Na 1.85B i 1.75C stoper instaluje się, działa i wyładowuje, przykład dźwiękowy potwierdza PCM, a `watch` odczytuje nowo zapisany motyw po usunięciu i reinstalacji. Uruchomienie i skan z wpisem `OBSOLETE` zachowują jego pliki. Osobne próby potwierdzają zastąpienie i `REMOVE`, ochronę wpisów spoza CI bez `--prune`, zachowanie danych przy obu flagach oraz odporność atomowego commitu na przerwanie. Ogólny przypadek złego magic lub wersji daje odmowę instalacji i wykonania. Weryfikacja bramki OTA obejmuje brak operacji kasowania i przenoszenia przy starcie. Raport zawiera deltę E0.

**Zależności i nakład.** Po E0, **M** w dolnej części zakresu. Główny koszt to migracja kodu i narzędzi, w tym normalna ścieżka instalacji i wyładowania. Wypadają koszty przenoszenia zapisów i automatycznego resetu rejestru.

## E1 — losowość

**Zakres.** Moduł `rep_random` z ograniczonym `fill` i równomiernym losowaniem z podanego zakresu. Backend platformowy działa także przy wyłączonym RF. Deklaracje wymagań i użycia usługi są widoczne w Managerze. Aplikacją jest Moneta.

**Bramka.** Obie płytki działają offline. Aplikacja losuje wynik, a testy sprawdzają za małe bufory i błędne zakresy. Brak pozostawionych zasobów po wyładowaniu. Test rozkładu nie stanowi oceny entropii kryptograficznej.

**Zależności i nakład.** EABI, **S**. Ryzyko dotyczy źródła ziarna i zachowania offline.

## E2 — wejście i postęp

**Zakres.** W głównym `rep_ui` dodać pierścień postępu oraz naciśnięcie, puszczenie, długie przytrzymanie i anulowanie dla własnego widgetu. Gest ma identyfikator i stan do odczytu po przepełnieniu kolejki. Pierścień liczy się do limitu widgetów. Aplikacja Oddech pokazuje fazę, postęp i reset przytrzymaniem. Integracja w UI bridge i GUI.

**Bramka.** Na obu panelach sprawdzić krótki dotyk, przytrzymanie, anulowanie przez nawigację, skrajne wartości postępu i powrót do poprawnego stanu po przeciążeniu. Na 1.75C sprawdzić skalowanie i mapowanie dotyku 360 → 466. Runner pokrywa logikę gestu, a próba palcem potwierdza dotyk.

**Zależności i nakład.** EABI, **M**. Ryzyka to kolejność zdarzeń wejścia i kolizja z nawigacją.

## E4a — terminy po wyładowaniu

**Zakres.** `rep_deadline` przy `roundie_time`. System przechowuje ograniczoną liczbę terminów w RAM, z właścicielem określonym przez ID pakietu i logiczny klucz. Po zamknięciu ELF rekord nadal należy do systemu. Systemowy komunikat pokazuje wynik, a stan oczekuje na potwierdzenie. Minutnik tekstowy uzbraja, odczytuje i anuluje termin względny. Do E4b uzbrojony termin ma jawną politykę odmowy wejścia w light sleep.

**Bramka.** Na obu płytkach komunikat pojawia się raz po wyjściu do Home albo przełączeniu aplikacji. Test obejmuje anulowanie, przepełnienie, powrót do stanu uzbrojenia oraz odrzucenie zdarzeń starej instancji.

**Zależności i nakład.** EABI, **M**. E2 jest opcjonalnym ulepszeniem widoku. Ryzyko dotyczy wyścigu anulowanie–wykonanie.

## E4b — termin ze snu i sygnał po wybudzeniu

**Fakt.** Oba dostępne buildy REP ustawiają `RTC_SLOW_CLK` na wewnętrzny RC z kalibracją 1024 cykli. 1.85B ma niezależny PCF85063, a 1.75C nie ma zewnętrznego RTC. Obecny timerowy wake pozostawia ekran zgaszony. BSP tworzy kodek raz, odtwarzanie ponownie otwiera go, a działanie pierwszego dźwięku po śnie wymaga próby. [PowerManager](../main/power_manager.c) · [Czas RTC](rtc-time-sync.md) · [Audio](audio.md)

**Zakres.** PowerManager wybiera najbliższy termin i rozróżnia alarm od technicznego wybudzenia. Aplikacją pozostaje minutnik. Przy 25 minutach błąd 1000 ppm odpowiada 1,5 s, a 1% odpowiada 15 s. Są to scenariusze, rzeczywisty błąd trzeba zmierzyć. Jeśli dokładność 1.85B wymaga korekty, rozważyć wcześniejsze wybudzenie i sprawdzenie PCF wraz z kosztem energii. Odczyt po spóźnionym wybudzeniu pozwala wykryć błąd. Przywracanie kodeka dopisać, jeśli normalne otwarcie okaże się niewystarczające.

**Bramka.** Na obu płytkach sprawdzić 25-minutowy termin względem niezależnego zegara, osobno moment wake, pokazanie komunikatu i początek pierwszego słyszalnego sygnału. Proponowane cele to wake w granicach ±2 s, komunikat do 500 ms od wake i działające audio. Raport rozróżnia sen ciągły i techniczne wybudzenia. Prąd podczas snu, przy wybudzeniu i po sygnale porównać z E0.

**Zależności i nakład.** E4a, **L**. Ryzyka to dryft RC, odzyskanie GUI i audio oraz zapas energii.

## E4c — arbitraż audio

**Zakres.** Alarm systemowy ma pierwszeństwo przed strumieniem aplikacji, a strumień przed zwykłymi sygnałami UI. Audio publikuje przyczynę przerwania w swoim głównym kontrakcie. Przejęcie unieważnia strumień i nie zależy od wykonania callbacku ELF. Minutnik służy jako walidator razem z istniejącym przykładem PCM.

**Bramka.** Na obu płytkach alarm przejmuje wyjście podczas PCM. Proponowany cel to dźwięk do 250 ms od żądania przejęcia. Stary uchwyt jest odrzucany, a po zakończeniu wzmacniacz i prąd wracają do stanu spoczynkowego.

**Zależności i nakład.** E4a, E4b dla prób po śnie, **M**. Ryzyko dotyczy zamknięcia strumienia i startu alarmu w tej samej chwili.

## E3 — konfiguracja z Managera i trwałe alarmy

**Zakres.** Wersjonowany protokół ustawień przez USB. Jedna ograniczona domena na operację, rewizja i potwierdzenie trwałego commitu przez system worker. `rep_settings` pozwala aplikacji odczytać właściwą domenę. Pomodoro używa długości faz z Managera. Budzik zapisuje termin UTC niezależnie od życia aplikacji i przywraca go po restarcie. Brak ważnego czasu wstrzymuje alarm, a obsługa przeterminowania jest jawna.

**Bramka.** Obie płytki używają zapisanych faz pomodoro. Budzik po restarcie programowym uruchamia się bez powtórnego startu aplikacji. Przerwany transfer i zła rewizja zachowują poprzedni zapis. Na 1.75C utrata zasilania wymaga odzyskania ważnego czasu. Na 1.85B ważność PCF jest sprawdzana. Scenariusz alarmu ze snu zawiera pomiar prądu.

**Zależności i nakład.** E4a–c, **L**. Ryzyka to trwałość, przestawienie zegara i zachowanie przy restarcie pomiędzy zapisem a potwierdzeniem.

## E5 — QMI8658 i poziomica

**Fakt.** Kod referencyjny nie obsługuje obecnie QMI8658. Schemat 1.85B wskazuje INT przez R51 na GPIO8. W audycie 1.75C tabela i etykieta linii różnią się między GPIO38 i GPIO21. Dokumentacja wariantu A opisuje tap, a dokumentacja wariantu C potwierdza Wake-on-Motion. Wariant układu oraz fizyczna linia INT wymagają sprawdzenia przed pracą nad pukaniem. [Audyt 1.85B](hardware-audit.md) · [Audyt 1.75C](hardware-audit-1.75c.md)

**Zakres.** Sterownik na wspólnej magistrali I²C, polling tylko przy aktywnej subskrypcji, normalizacja osi względem ekranu i próbka ze znacznikiem czasu w `rep_sensor`. Pojedynczy slot ostatniej próbki zamiast historii. Poziomica liczbowa pokazuje kąty i umożliwia zerowanie.

**Bramka.** Obie płytki pokazują poprawne znaki osi w sześciu orientacjach i reagują na przechył po zerowaniu. Porównać z odniesieniem, sprawdzić dotyk i audio podczas odczytów oraz brak starych próbek po unsubscribe. Pomiar obejmuje pracę IMU, stan po wyłączeniu subskrypcji i light sleep. Tap i WoM pozostają oddzielnym eksperymentem dla potwierdzonego układu i połączenia.

**Zależności i nakład.** EABI, E3 tylko dla konfiguracji kalibracji w Managerze, **M**. Ryzyka to różnice osi, obciążenie I²C i pobór przy wyłączaniu sensora.

## Decyzja po E5

Porównać wartość grafiki, sieci i radia z wynikami obu płytek. Użyć delty E0, największych wolnych bloków, zapasów stosów, dokładności alarmów i budżetu energetycznego. Można zakończyć na funkcjach offline, wykonać samą grafikę albo przejść do sieci. E7 nie potrzebuje E6. Zgoda na dalszy etap nie zastępuje jego bramki sprzętowej.

## E6 — rysowanie i zasoby aplikacji

**Zakres.** Pierwsza scena z tłem i sprite'em uzasadnia kolejny format pakietu z ograniczonym katalogiem sekcji. Zasoby I4/I8 z paletą są poza ELF. System worker czyta, haszuje i przygotowuje kopię w PSRAM. GUI używa zatwierdzonego uchwytu zasobu i ograniczonej kolejki klatek. `rep_draw` i `rep_resource` powstają tutaj, wraz z przypięciem artefaktu na czas korzystania z pliku. Używać LittleFS przez VFS. Na tym etapie wystarcza jedna scena.

**Bufory rysowania (fakt z E0 §10.1).** Bufory LVGL leżą w wewnętrznej SRAM z DMA. Na 1.85B to 2 × 14 400 B pasów; 1.75C ma osobną ścieżkę skalowania, także w SRAM. Kod GUI i LVGL wykonuje się z PSRAM (XIP). Odczyt LittleFS nie wyłącza cache, więc GUI z PSRAM nie staje przez sam dostęp do Flash, tylko przez brak CPU (model priorytetów I/O z EABI). Bufory klatek sceny i kopie zasobów w PSRAM trzeba zmierzyć osobno pod kątem przepustowości PSRAM wspólnej z XIP. Przeniesienie buforów rysowania z SRAM do PSRAM wymaga pomiaru czasu flush i stanu SRAM, a nie tylko decyzji pamięciowej.

**Bramka.** Obie płytki pokazują scenę z przycięciem i przezroczystością oraz zachowują responsywną nawigację. Sprawdzić odmowę wadliwego zasobu, przeciążenie klatek i zwolnienie pamięci po wyładowaniu. Mierzyć FPS, czasy GUI/DMA, skok PSRAM/SRAM i przestrzeń Flash. 1.75C wymaga osobnego pomiaru kosztu skalowania. Limity ustalić według E0 i pomiaru wspólnego szczytu z loaderem.

**Zależności i nakład.** Decyzja po E5, E2 dla nowego wejścia, E5 opcjonalnie dla przechyłu, **L**. Ryzyka to cykl życia buforów i czas klatki.

## E7a/b/c — Wi-Fi, czas, HTTPS

Wspólny zakres obejmuje worker sieciowy, konfigurację z Managera, ograniczone operacje i anulowanie z potwierdzeniem. Aktywne zadanie może czasowo blokować light sleep. Po zamknięciu usługi wracają do stanu spoczynkowego. Każda podbramka obejmuje na obu płytkach pamięć oraz prąd podczas pracy, po zwolnieniu zasobów i w light sleep.

| Etap | Zakres i aplikacja | Bramka | Zależności i ryzyko |
|---|---|---|---|
| **E7a** | `rep_network`, Wi-Fi i aplikacja Stan sieci | Połączenie, utrata AP, ponowne połączenie i zwolnienie zapotrzebowania | E3 i decyzja po E5. SRAM, zachowanie RF i ponawianie |
| **E7b** | Czas sieciowy przez `roundie_time` i aplikacja Zegar | Synchronizacja z nieważnego czasu, zmiana rewizji i brak skoku minutnika względnego | E7a. 1.75C po utracie zasilania musi odtworzyć czas. 1.85B może skorzystać z ważnego PCF |
| **E7c** | `rep_http` i aplikacja pobierająca krótki tekst | Uwierzytelniony HTTPS, odmowa dla złej nazwy lub certyfikatu, timeout i anulowanie | E7a/b. Ważny czas do TLS, pamięć i zwolnienie workera |

**Nakład.** Całość **L**. Budżety usług ustalić z E0 i rzeczywistych szczytów na urządzeniu.

## E8 — strumieniowanie audio

**Zakres.** `rep_media` do obsługi jednej stacji. Worker odbiera i dekoduje MP3, przekazuje PCM do istniejącego audio i korzysta z arbitrażu E4c. Obsługa wejścia 44,1/48 kHz uwzględnia resampling, jeśli potrzebny. Kolejne kodeki wchodzą wraz z próbkami walidacyjnymi.

**Bramka.** Jedna stacja gra na obu płytkach przy włączonym i zgaszonym ekranie. Sprawdzić przerwę transmisji, buforowanie, alarm w trakcie radia, anulowanie w każdej fazie i odzyskanie zasobów. Porównać prąd odtwarzania, stanu po zamknięciu i następującego po nim light sleep. Budżet SRAM i PSRAM musi zostawić rezerwę dla systemowego alarmu.

**Zależności i nakład.** E4c, E7, **L**. Ryzyka to dekoder, jego licencja, chwilowe zużycie SRAM i underruny.

## Zadania wypełniające i kolejność

- Tablica wyników korzysta z istniejących widgetów, dotyku i dialogu. Może posłużyć do prób runnera. EABI umożliwia zachowanie wyniku podczas reinstalacji, gdy aplikacja zapisze go przed wyładowaniem.
- Tarcze w stylu VFD i C64 korzystają z istniejącego RWF, jego typografii i kolorów. To osobna dostawa od REP i nie wymaga nowego API grafiki.
- i18n: wybór języka interfejsu i kontrola kompletności tłumaczeń w buildzie. Korzysta z istniejącego UI i ustawień, bez nowego enablera.

**Kolejność** E0 → EABI → E1 → E2 → E4a/b/c → E3 → E5 → decyzja → E6 i/lub E7a/b/c → E8. E4a może ruszyć przed ukończeniem E2 z widokiem tekstowym. Zadania wypełniające można realizować niezależnie. Przed 1.0 zaprojektować i zwalidować wymagane podpisy, po czym zamrozić sprawdzony format i ABI.
