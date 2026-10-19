# Round Embedded Platform (REP)
## Specyfikacja architektoniczna i założenia projektowe

**Wersja:** 0.4  
**Status:** Draft — Architecture Baseline  
**Data:** 17 października 2026  
**Platforma:** Waveshare ESP32-S3 Touch-LCD-1.85B  
**Docelowy framework:** ESP-IDF v6.1  
**Język:** C  
**Poprzednia wersja:** REP v0.3  
**Zintegrowana decyzja:** ADR-004 — Boot & Recovery Architecture

---

# 1. Cel projektu

## 1.1. Cel nadrzędny

Round Embedded Platform (REP) jest projektem całkowicie nowego firmware'u dla Waveshare ESP32-S3 Touch-LCD-1.85B.

Oprogramowanie ma zastąpić fabryczny firmware i dostarczyć lekką platformę embedded wyposażoną w:

- Nowoczesny interfejs zaprojektowany dla okrągłego wyświetlacza.
- Natywne, dynamicznie ładowane aplikacje ELF.
- Stabilne SDK oraz wersjonowane ABI aplikacyjne.
- Model wykonywania sterowany zdarzeniami.
- Zarządzanie cyklem życia aplikacji i zasobami.
- System instalowania, aktualizowania i usuwania aplikacji.
- Dostęp do plików przez USB MTP.
- Aktualizacje firmware'u OTA A/B.
- Zarządzanie energią i uśpieniem.
- Mechanizmy Safe Mode, rollback i ROM Recovery.

## 1.2. Filozofia architektoniczna

**ESP32-S3 jest mikrokontrolerem, a nie procesorem aplikacyjnym.**

REP nie ma implementować miniaturowego Linuksa, Androida ani Symbiana.

Jego celem jest zapewnienie wybranych funkcji platform aplikacyjnych w ramach lekkiego firmware'u embedded, bez wprowadzania zbędnego narzutu.

Podstawowe priorytety:

1. Niezawodność i możliwość odzyskania systemu.
2. Poprawne zarządzanie pamięcią i zasobami.
3. Responsywność GUI.
4. Przewidywalny czas wykonywania.
5. Prostota implementacji.
6. Stabilność ABI.
7. Modularność.
8. Rozszerzalność.

## 1.3. Inspiracje architektoniczne

| System | Wykorzystane koncepcje |
|---|---|
| Symbian OS | Active Objects, Active Scheduler, ECom, Descriptors |
| Android | Application Lifecycle, PackageInstaller, Wake Locks |
| Wear OS | Interfejs okrągły, listy transformujące, nawigacja gestami |
| ESP-IDF | Bootloader, OTA, FreeRTOS, BSP, zarządzanie pamięcią |

Nie przewiduje się przenoszenia kodu ani zależności systemowych Androida lub Symbiana.

## 1.4. Rozwiązania wykluczone

W MVP nie przewiduje się:

- WebAssembly.
- Interpreterów aplikacji.
- Maszyn wirtualnych.
- Pełnego POSIX.
- Niezależnych procesów.
- Pełnego IPC.
- Uruchamiania wielu zewnętrznych ELF jednocześnie.
- Własnego bootloadera.
- MCUboot.
- Dedykowanej partycji Recovery.
- Ogólnego dynamicznego linkera kompatybilnego z Linuksem.

# 2. Platforma sprzętowa

## 2.1. Docelowa konfiguracja

| Element | Parametr |
|---|---|
| Moduł | Waveshare ESP32-S3 Touch-LCD-1.85B |
| MCU | ESP32-S3R8 |
| CPU | Dual-core Xtensa LX7, do 240 MHz |
| Wewnętrzny SRAM | 512 KB, przed odjęciem pamięci zarezerwowanej |
| Flash | 16 MB |
| PSRAM | 8 MB, Octal |
| Wyświetlacz | LCD, 360 × 360 |
| Kontroler LCD | ST77916 |
| Interfejs LCD | QSPI |
| Panel dotykowy | CST816S |
| IMU | QMI8658 |
| RTC | PCF85063 |
| Fuel gauge | BQ27220 |
| Audio output | ES8311 |
| Audio input | ES7210, dwa mikrofony |
| Łączność | Wi-Fi, Bluetooth LE |
| Pamięć rozszerzalna | microSD |
| USB | Natywne USB Full-Speed ESP32-S3 |
| Przyciski | BOOT, PWR |

Moduł nie udostępnia osobnego fizycznego przycisku RESET.

Pinów wykorzystywanych przez wbudowaną PSRAM, w szczególności GPIO33–37, nie należy traktować jako wolnych GPIO.

## 2.2. Istotne przypisania GPIO

| Funkcja | GPIO |
|---|---|
| BOOT | GPIO0 |
| Touch INT | GPIO4 |
| RTC INT | GPIO6 |
| Backlight | GPIO5 |
| I²C SCL | GPIO10 |
| I²C SDA | GPIO11 |
| USB D− | GPIO19 |
| USB D+ | GPIO20 |
| LCD CS | GPIO21 |
| LCD Clock | GPIO40 |

Szczegółowa mapa wszystkich GPIO powinna znajdować się w `docs/hardware.md`.

## 2.3. Wspólna magistrala I²C

Urządzenia CST816S, QMI8658, ES8311, ES7210, BQ27220 oraz PCF85063 współdzielą magistralę na GPIO10/11.

REP musi wykorzystywać jeden zarządzany kontroler I²C Master i jawnie rejestrowane urządzenia.

W ESP-IDF 6.1 należy używać nowego sterownika:

```c
#include "driver/i2c_master.h"
```

Nie należy mieszać go ze starszym `driver/i2c.h`.

Starsze komponenty BSP należy dostosować do nowego interfejsu, jeśli jest to konieczne.

## 2.4. Zachowanie przycisku PWR

Dokumentacja techniczna Waveshare stwierdza, że PWR nie obsługuje funkcji programowalnych.

Długie przytrzymanie, około dwóch sekund, sprzętowo włącza lub wyłącza urządzenie. Dokumentacja opisuje również krótsze przytrzymanie służące do włączania.

W konsekwencji:

- Firmware nie powinien zakładać możliwości wykrywania naciśnięć PWR.
- PWR nie jest częścią zwykłego API wejściowego systemu.
- Nie zakłada się przejęcia PWR bez modyfikacji sprzętu.
- Menu zasilania będzie udostępniane przez GUI.
- Funkcja programowego `POWER_OFF` nie jest gwarantowana.

Strona marketingowa producenta zawiera sprzeczną informację dotyczącą programowalności PWR. Dlatego dokumentację techniczną przyjmuje się jako podstawę, a schemat Rev1.1 i test fizyczny jako ostateczną weryfikację.

## 2.5. Ograniczenia dodatkowe

- GPIO0 jest pinem strappingowym i musi zachować funkcję wejścia w ROM Download Mode.
- GPIO4 i GPIO6 mogą być kandydatami na źródła wybudzania.
- GPIO0, GPIO4 i GPIO6 posiadają funkcjonalność RTC GPIO.
- Skuteczność wybudzania zależy również od zachowania podłączonych kontrolerów.
- Dokumentacja testu PCF85063 wskazuje na konieczność podłączenia akumulatora dla prawidłowego działania tego przykładu.
- W tabeli GPIO modelu 1.85B nie wskazano LCD_TE. Nie należy zakładać jego dostępności.

**Uwaga:** nie wolno przenosić pinoutu z modelu ESP32-S3 Touch-LCD-1.85 bez litery B, ponieważ jest to inna konstrukcja sprzętowa.

# 3. Stos technologiczny i kompatybilność ESP-IDF

## 3.1. Docelowy stos

| Obszar | Technologia |
|---|---|
| Framework | ESP-IDF v6.1 |
| Język firmware'u | C |
| RTOS | FreeRTOS |
| GUI | LVGL 9 |
| Storage | LittleFS |
| Konfiguracja | NVS |
| Crypto | PSA Crypto / mbedTLS 4.x |
| ELF | Espressif ELF Loader |
| USB | TinyUSB / esp_tinyusb |
| OTA | ESP-IDF App Update |
| Build | CMake / idf.py |
| Testy | Hostowe i sprzętowe |

Wersja 6.1 jest bazą docelową. Dokładny tag lub commit należy przypiąć w repozytorium po przejściu testów integracyjnych.

## 3.2. Migracja BSP do ESP-IDF v6.1

Waveshare dostarcza przykłady przygotowane dla ESP-IDF 5.5.x.

Zgodność źródeł i komponentów z ESP-IDF 6.1 nie została jeszcze potwierdzona eksperymentalnie.

Dlatego Hardware Bring-up musi objąć adaptację BSP i sterowników.

Wymagania:

- Kompilacja na przypiętej wersji ESP-IDF 6.1.
- Obsługa nowych sterowników I²C i I²S.
- Brak zależności od usuniętych API.
- Poprawne deklaracje komponentów i ich zależności.
- Brak globalnego wyłączania `-Werror`.
- Brak wyłączania wszystkich ostrzeżeń kompilatora w celu obejścia problemów.
- Zgodność z domyślnym `gnu23`.

Jeżeli pojedynczy komponent zewnętrzny wymaga tymczasowego obejścia, powinno ono być lokalne, udokumentowane i oznaczone do usunięcia.

## 3.3. Biblioteka standardowa C

ESP-IDF 6.x domyślnie wykorzystuje Picolibc zamiast Newlib.

Jest to szczególnie istotne dla natywnego ABI aplikacji ELF.

REP przyjmuje następujące zasady:

- Picolibc jest początkowym wariantem do testów.
- Wybór libc nie jest jeszcze zamrożony.
- Aplikacje ELF i firmware muszą używać zgodnych kontraktów runtime'u.
- Aplikacje nie mogą polegać na prywatnych strukturach libc.
- Obsługa `errno`, reentrancy i TLS wymaga weryfikacji.
- Nie wolno zakładać wsparcia wszystkich typów relokacji TLS przez ELF Loader.

Zmiana libc po ustabilizowaniu ABI będzie traktowana jako zmiana wymagająca testów kompatybilności.

## 3.4. Kryptografia

ESP-IDF 6.1 wykorzystuje mbedTLS 4.x oraz PSA Crypto.

Weryfikacja podpisów pakietów ma wykorzystywać PSA Crypto zamiast usuniętych starszych interfejsów kryptograficznych.

Jeżeli operacje kryptograficzne wykonywane są przed standardową inicjalizacją ESP-IDF, wymagane jest odpowiednio wczesne `psa_crypto_init()`.

## 3.5. JSON i zależności zewnętrzne

Wbudowany komponent JSON został usunięty z ESP-IDF 6.x.

Jeżeli manifesty aplikacji będą korzystały z JSON, parser należy dodać jako osobną, przypiętą zależność, np. `espressif/cjson`.

Format manifestów pozostaje decyzją otwartą.

Nie należy uzależniać publicznego SDK od parsera JSON.

# 4. Architektura wysokiego poziomu

```text
+------------------------------------------------+
|                  Applications                  |
|             System Apps / ELF Apps             |
+------------------------------------------------+
|                Application SDK                 |
|       Stable ABI / UI / Events / Services      |
+------------------------------------------------+
|               Application Runtime              |
|    AppManager / Registry / Executor / ELF      |
+------------------------------------------------+
|                   System Core                  |
| UI / Storage / Packages / Power / OTA / USB    |
+------------------------------------------------+
|                  ESP-IDF / BSP                 |
|       FreeRTOS / LVGL / Drivers / Hardware     |
+------------------------------------------------+
```

## 4.1. Komponenty systemowe

| Komponent | Odpowiedzialność |
|---|---|
| `AppRegistry` | Rejestr aplikacji i ich wersji |
| `AppManager` | Cykl życia aplikacji |
| `AppExecutor` | Wykonywanie callbacków ELF |
| `ElfLoader` | Ładowanie i zwalnianie modułów |
| `ResourceManager` | Własność zasobów |
| `EventDispatcher` | Zdarzenia, kolejki i anulowanie |
| `UIManager` | GUI i standardowe komponenty |
| `PackageManager` | Instalowanie i aktualizowanie aplikacji |
| `StorageManager` | System plików i dane |
| `OTAManager` | Aktualizacje systemowe |
| `USBManager` | USB Device i MTP |
| `PowerManager` | Stany zasilania |
| `NetworkManager` | Łączność |
| `SettingsManager` | Konfiguracja |
| `TimeService` | Zegar, RTC, timery |
| `CrashRecoveryManager` | Safe Mode i diagnostyka |

Jest to podział logiczny.

Nie wymaga on osobnego taska, globalnego singletonu ani frameworka dla każdego komponentu.

# 5. Application Runtime

## 5.1. Model wykonywania

REP wykorzystuje model inspirowany Symbian Active Objects.

Aplikacje nie posiadają własnej głównej pętli wykonania.

System uruchamia callbacki aplikacji w odpowiedzi na zdarzenia.

W MVP:

- Jedna aktywna aplikacja zewnętrzna.
- Jeden Application Executor.
- Brak własnych tasków FreeRTOS w aplikacjach.
- Brak ELF uruchamianych w tle.
- Brak współdzielonych dynamicznych bibliotek między aplikacjami.

## 5.2. Konteksty wykonania

### GUI Task

Odpowiada za:

- LVGL.
- Rendering.
- Animacje.
- Nawigację.
- Interakcje dotykowe.
- Tworzenie i usuwanie obiektów GUI.

### Application Executor

Odpowiada za:

- Callbacki ELF.
- Logikę aplikacji.
- Obsługę zdarzeń.
- Lifecycle.
- Kontrolowanie czasu wykonania.

### System Workers

Obsługują potencjalnie długotrwałe operacje:

- Network.
- Storage.
- OTA.
- Package installation.
- USB transfer.
- Audio.

## 5.3. Priorytety i watchdog

AppExecutor musi być objęty nadzorem Task Watchdog Timer.

Należy wybrać priorytety zapobiegające zagłodzeniu GUI przez aplikację.

W konfiguracji, w której oba taski pracują na tym samym rdzeniu, AppExecutor powinien mieć niższy priorytet niż GUI Task, jeśli pomiary potwierdzą poprawne zachowanie całego systemu.

Należy unikać niepotrzebnego przypinania tasków do rdzeni.

W szczególności trzeba przetestować:

- Przekroczenie budżetu callbacku.
- Nieskończoną pętlę w ELF.
- Zajęcie całego CPU.
- Zachowanie GUI podczas błędu.
- Reakcję watchdogów obu rdzeni.

Niższy priorytet nie stanowi izolacji przed błędami pamięci ani przed wyłączeniem przerwań przez wadliwy kod.

# 6. EventDispatcher

## 6.1. Model zdarzeń

Zdarzenia obejmują:

- UI.
- Timer.
- Network.
- Storage.
- Power.
- System.
- Lifecycle.
- Custom.

Każda instancja aplikacji ma własny identyfikator generacji.

Opóźnione zdarzenia odnoszące się do nieaktywnej generacji są odrzucane.

## 6.2. Run-to-completion

Callback aplikacji musi przetworzyć zdarzenie i oddać sterowanie.

Długotrwałe operacje powinny być zlecane do systemowych workerów.

Zabronione przez kontrakt aplikacyjny są:

- Blokujące operacje sieciowe.
- Synchroniczne oczekiwanie na GUI.
- Nieskończone pętle.
- Tworzenie własnych tasków.
- Blokowanie executora bez określonego limitu czasu.

## 6.3. Kolejki

Kolejki muszą mieć ograniczoną pojemność.

System powinien obsługiwać:

- Backpressure.
- Odrzucanie zdarzeń niekrytycznych.
- Łączenie zdarzeń, których semantyka na to pozwala.
- Oddzielną obsługę krytycznych poleceń lifecycle.
- Diagnostykę długości kolejki.
- Raportowanie przepełnienia.

## 6.4. Anulowanie

Każda zarządzana operacja asynchroniczna ma identyfikator i stan.

Możliwe stany:

```text
CREATED
PENDING
RUNNING
CANCEL_REQUESTED
COMPLETED
CANCELLED
FAILED
```

Żądanie anulowania nie oznacza, że operacja została natychmiast zakończona.

ResourceManager nie może zwolnić pamięci wykorzystywanej przez workera przed otrzymaniem potwierdzenia zakończenia lub anulowania.

# 7. Application Lifecycle

## 7.1. Stany

```text
INSTALLED
   |
   v
 LOADED
   |
   v
INITIALIZED
   |
   v
 RUNNING <----> SUSPENDED
   |
   v
 STOPPING
   |
   v
 UNLOADING
   |
   v
INSTALLED
```

Dodatkowo runtime musi reprezentować stan `FAILED`.

Niepowodzenie inicjalizacji nie może wymagać przechodzenia przez wszystkie normalne stany lifecycle.

## 7.2. Przejścia przy błędach

Przykładowe scenariusze:

| Stan | Błąd | Działanie |
|---|---|---|
| LOADING | Błąd ELF/relokacji | Zwolnienie częściowo przydzielonych zasobów |
| LOADED | Błąd `on_init` | UNLOADING |
| INITIALIZED | Błąd `on_start` | STOPPING lub cleanup, zgodnie z kontraktem |
| RUNNING | Błąd obsługiwalny | FAILED → STOPPING |
| RUNNING | Panic | Restart systemu |
| STOPPING | Błąd cleanup | Raport diagnostyczny, bez niebezpiecznego unload |

Formalna tabela przejść będzie częścią `docs/application-lifecycle.md`.

## 7.3. Oddzielenie stanu logicznego i widoku

Stan aplikacji nie jest utożsamiany z obiektami LVGL.

Aplikacja może zachować stan logiczny po zniszczeniu widoku.

W MVP odtwarzanie stanu trwałego odbywa się jawnie poprzez Storage API.

Nie przewiduje się automatycznej serializacji całego kontekstu wykonania.

# 8. Native ELF Loader

## 8.1. Format aplikacji

Zewnętrzne aplikacje są kompilowane do natywnego kodu Xtensa LX7.

Preferowanym loaderem jest oficjalny `espressif/elf_loader`.

Wymagane są:

- Obsługiwany ELF.
- Relokacje Xtensa.
- Rozwiązywanie symboli.
- Kontrolowana alokacja.
- Wykonanie punktu wejścia.
- Zwolnienie kodu i danych.

Dokładna wersja loadera zostanie przypięta po testach na ESP-IDF 6.1.

Samo deklarowane wsparcie `ESP-IDF >= 4.4.3` nie stanowi dowodu kompatybilności z wersją 6.1.

## 8.2. Hidden ABI

**Publiczna vtable nie jest jedyną granicą ABI aplikacji ELF.**

Kompilator może wygenerować odwołania do symboli libc lub libgcc niezadeklarowanych jawnie w SDK.

Przykłady:

- `memcpy`
- `memset`
- `memcmp`
- Pomocnicze funkcje dzielenia 64-bitowego.
- Funkcje obsługujące operacje zmiennoprzecinkowe.
- Wybrane funkcje runtime'u kompilatora.

ESP32-S3 posiada sprzętowe możliwości operacji zmiennoprzecinkowych ograniczone względem typowego procesora aplikacyjnego; część operacji może wymagać pomocniczego kodu programowego.

Zmiana toolchaina, libc lub zestawu symboli może powodować niezgodność binarną aplikacji.

## 8.3. Biała lista symboli

REP powinien posiadać wersjonowaną politykę dopuszczalnych symboli importowanych.

Przykładowo:

```text
ABI imports:
    memcpy
    memset
    memmove
    memcmp
    selected libgcc helpers
    approved runtime helpers
```

Nie należy automatycznie eksportować do aplikacji wszystkich funkcji firmware'u.

W szczególności standardowy kontrakt aplikacji nie udostępnia:

- `xTaskCreate`
- `vTaskDelete`
- `lv_*`
- Surowych funkcji sterowników.
- Prywatnych funkcji System Core.

Pakiety powinny być sprawdzane pod kątem dopuszczalnych importów przed instalacją lub aktywacją.

Należy jednak potwierdzić techniczną możliwość ograniczenia tablicy symboli w wybranej wersji ELF Loader.

Ograniczenie symboli nie stanowi zabezpieczenia przed złośliwym natywnym kodem, który może próbować bezpośrednio manipulować pamięcią.

## 8.4. Weryfikacja symboli podczas instalacji

PackageManager powinien analizować ELF przed aktywacją.

Weryfikacja obejmuje:

- Architekturę i format.
- Wersję ABI.
- Typy relokacji.
- Symbole importowane.
- Symbole niedozwolone.
- Wymagania pamięci.
- Obecność uzgodnionego entry point.

Analiza podczas instalacji nie zastępuje walidacji wykonywanej przez loader przy rzeczywistym ładowaniu.

## 8.5. Toolchain aplikacji

SDK dostarcza własną konfigurację kompilacji aplikacji.

Powinna ona określać:

- Docelową architekturę.
- Wersję GCC.
- Standard C.
- Flagi kompilacji.
- Flagi linkera.
- Obsługiwane typy relokacji.
- Politykę importowanych symboli.
- Zasady użycia libc.
- Konwencje ABI.

Nie należy wymagać od autorów aplikacji znajomości wewnętrznej struktury firmware'u REP.

Jeśli ELF Loader wymaga określonych komponentów ESP-IDF do budowania aplikacji, SDK może wykorzystywać je wewnętrznie, bez wystawiania ich jako publiczny kontrakt.

## 8.6. Picolibc i TLS

W proof of concept należy zbadać:

- Działanie helperów libc/libgcc.
- Zachowanie `errno`.
- Potencjalne symbole TLS.
- Typy relokacji.
- Funkcje inicjalizacyjne i destrukcyjne.
- Zależność od konfiguracji Picolibc.

Brak obsługi wymaganej relokacji powinien skutkować odrzuceniem modułu, nie próbą jego częściowego wykonania.

# 9. Application SDK i publiczne ABI

## 9.1. Model API

System udostępnia jedną wersjonowaną tablicę funkcji.

Przykład koncepcyjny:

```c
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;

    const void *system_api;
    const void *ui_api;
    const void *storage_api;
    const void *network_api;
    const void *event_api;
    const void *power_api;
} rep_api_t;
```

Publiczny interfejs ma być niezależny od wewnętrznych struktur LVGL, FreeRTOS i ESP-IDF.

## 9.2. Punkt wejścia

Aplikacja eksportuje uzgodniony symbol:

```c
const rep_app_descriptor_t *rep_app_entry(
    const rep_api_t *api
);
```

Nie należy eksportować zbędnych symboli aplikacji.

## 9.3. Typy ABI

W publicznych strukturach należy preferować typy o ustalonej szerokości.

Przykład:

```c
typedef struct {
    const char *data;
    uint32_t length;
} rep_string_t;

typedef struct {
    void *data;
    uint32_t length;
    uint32_t capacity;
} rep_buffer_t;
```

Zasady:

- UTF-8 dla tekstu.
- Jawna długość.
- Jawna pojemność.
- Jawna własność pamięci.
- Brak zależności od rozmiaru `size_t`.
- Brak polegania na niejawnych rozmiarach enumów.
- Brak struktur zawierających prywatne typy frameworka.

## 9.4. Sprawdzanie layoutu

Publiczne struktury ABI powinny posiadać testy kompilacyjne.

Przykładowo:

```c
_Static_assert(sizeof(rep_string_t) == 8,
               "Invalid ABI size");

_Static_assert(offsetof(rep_string_t, length) == 4,
               "Invalid ABI layout");
```

Nagłówki SDK muszą być zgodne co najmniej z C11 oraz C23.

Testy hostowe powinny sprawdzać semantykę struktur, ale nie mogą zakładać identycznego layoutu wskaźników na hoście 64-bitowym i na Xtensa.

Definicje layoutu docelowego ABI należy walidować właściwym toolchainem Xtensa.

# 10. ResourceManager

## 10.1. Własność zasobów

Każda instancja aplikacji otrzymuje własny kontekst.

Zasoby tworzone przez SDK są przypisywane do tego kontekstu.

Dotyczy to:

- UI.
- Timerów.
- Operacji sieciowych.
- Uchwytów plików.
- Subskrypcji zdarzeń.
- Operacji asynchronicznych.
- Sleep Inhibitors.
- Buforów udostępnionych przez SDK.

## 10.2. Uchwyty generacyjne

Każdy uchwyt powinien umożliwiać sprawdzenie:

- Typu zasobu.
- Identyfikatora.
- Generacji.
- Właściciela.

Nieaktualne uchwyty muszą być odrzucane.

## 10.3. Alokator aplikacyjny

SDK powinno udostępniać zarządzany interfejs alokacji, np.:

```c
void *rep_alloc(uint32_t size);
void rep_free(void *ptr);
```

Alokacje wykonywane przez ten interfejs powinny być przypisywane do aktywnej instancji aplikacji.

Możliwe jest również zapewnienie kontrolowanych wrapperów dla `malloc()` i `free()`, ale nie należy zakładać, że przechwycenie tych symboli automatycznie obejmuje wszystkie formy alokacji pamięci.

Wymaga to osobnej weryfikacji loadera, libc, relokacji, kontekstu executora i zachowania callbacków.

W MVP preferowany jest jawny, zarządzany alokator SDK.

## 10.4. Zamykanie aplikacji

Sekwencja:

1. Zmiana stanu na `STOPPING`.
2. Blokada przyjmowania nowych zdarzeń.
3. Zakaz tworzenia nowych zasobów.
4. Zlecenie anulowania operacji asynchronicznych.
5. Oczekiwanie na potwierdzenie zakończenia lub anulowania.
6. Zakończenie aktywnego callbacku.
7. Wywołanie `on_stop`.
8. Odłączenie subskrypcji i callbacków.
9. Zwolnienie zasobów SDK.
10. Unieważnienie uchwytów.
11. Zwolnienie obrazu ELF.

Próba utworzenia nowego zasobu po przejściu do `STOPPING` powinna zwracać odpowiednik `REP_ERR_STOPPING`.

Po zwolnieniu ELF nie może istnieć żadna aktywna ścieżka wywołania prowadząca do jego kodu.

# 11. Pamięć i PSRAM

## 11.1. Główne wymaganie

REP ma umożliwiać wykonywanie natywnych modułów ELF z odpowiednich obszarów pamięci, preferencyjnie z PSRAM, jeżeli potwierdzona zostanie stabilność takiego rozwiązania.

Nie należy zakładać, że zwykłe `malloc()` w PSRAM zapewnia pamięć wykonywalną.

## 11.2. Wpływ operacji Flash

ESP32-S3 współdzieli zasoby cache i magistrali pamięci z Flash oraz PSRAM.

W domyślnej konfiguracji ESP-IDF operacje SPI1 mogą powodować wyłączenie cache, wstrzymanie tasków i niedostępność pamięci zewnętrznej.

Wpływa to na:

- Zapis LittleFS.
- Zapis NVS.
- Instalację pakietów.
- OTA.
- Wykonywanie ELF.
- Rendering GUI.
- Transfery wymagające dostępu do PSRAM.

Nie należy traktować tych operacji jako pozbawionych wpływu na czas reakcji systemu.

## 11.3. XIP from PSRAM

W ESP-IDF 6.1 należy przetestować:

```ini
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
```

Opcja umożliwia przenoszenie odpowiednich sekcji kodu i danych tylko do odczytu firmware'u do PSRAM oraz ogranicza problem wyłączania cache podczas operacji Flash.

Nie gwarantuje jednak:

- Braku wszystkich opóźnień.
- Nieograniczonej współbieżności dostępu.
- Automatycznej obsługi dynamicznych ELF.
- Braku konfliktów z mapowaniem Flash.
- Dostępności całej PSRAM dla aplikacji.

## 11.4. Koszt XIP

Sekcje firmware'u przenoszone do PSRAM zajmują miejsce, którego nie można następnie wykorzystać na heap aplikacji.

Jeżeli sekcje wymagające przeniesienia zajmują przykładowo 2 MiB, dostępna pula PSRAM zostaje odpowiednio zmniejszona.

Budżet musi uwzględniać:

- Kod i rodata firmware'u w PSRAM.
- Pamięć GUI.
- Heap systemowy.
- Pamięć ELF.
- Bufory aplikacyjne.
- Bufory USB i sieciowe.
- Rezerwę na sytuacje awaryjne.

Rozmiar pliku firmware'u w Flash nie jest automatycznie równy ilości PSRAM zajmowanej przez XIP.

## 11.5. Profile testowe

W Etapie 3 należy porównać:

**Profile A — Standard Cache**

- Bez XIP from PSRAM.
- Mniejszy koszt rezerwacji PSRAM.
- Możliwe dłuższe zatrzymania pracy podczas Flash I/O.

**Profile B — XIP from PSRAM**

- Firmware przenosi odpowiednie sekcje do PSRAM.
- Większe stałe zużycie PSRAM.
- Potencjalnie lepsza współbieżność z operacjami Flash.

Pomiar musi obejmować:

- Średnie opóźnienia.
- Maksymalne opóźnienia.
- P95/P99.
- Zużycie pamięci.
- Fragmentację.
- Stabilność.
- Wpływ na GUI.
- Wpływ na ELF.

## 11.6. Obszary mapowane z Flash

Należy uwzględnić ograniczenia `spi_flash_mmap()` i flagi `SPI_FLASH_MMAP_FLAG_BLOCKS_WRITE`.

Nie należy tworzyć rozwiązania, które może doprowadzić do deadlocku wskutek jednoczesnego utrzymywania mapowania Flash i oczekiwania na zapis Flash.

## 11.7. Szczytowe zużycie pamięci podczas ładowania ELF

Loader może wymagać jednoczesnego przechowywania:

- Bufora wejściowego ELF.
- Przygotowanych sekcji kodu.
- Sekcji danych.
- Tablic relokacji.
- Struktur pomocniczych loadera.

W MVP należy przyjąć, że szczytowe zużycie pamięci może być istotnie większe od rozmiaru załadowanej aplikacji.

Dokładny koszt należy zmierzyć dla wybranej wersji loadera.

# 12. UI Framework

## 12.1. Round First

GUI ma być projektowane dla okrągłego ekranu 360 × 360.

Nie należy projektować prostokątnych formularzy, które dopiero później są przycinane do okręgu.

## 12.2. Komponenty

Planowane komponenty:

- `RoundList`
- `RoundDialog`
- `RadialMenu`
- `ArcSlider`
- `EdgeButton`
- `QuickSettings`
- `WatchFace`
- `AppLauncher`
- `PowerMenu`
- `NotificationView`
- `StatusIndicator`

Nazwy reprezentują koncepcje API, niekoniecznie odrębne biblioteki.

## 12.3. Geometria okrągłego ekranu

Dla promienia nominalnego `R = 180` pikseli dostępna szerokość koła na wysokości `y` wynosi:

$$
w(y)=2\sqrt{R^2-(y-R)^2}
$$

Dla komponentów o niezerowej wysokości należy uwzględnić minimalną dostępną szerokość w całym przedziale pionowym.

Dodatkowo należy uwzględnić:

- Margines bezpieczeństwa.
- Padding.
- Minimalny rozmiar interakcyjny.
- Rzeczywistą maskę LCD.
- Dostępność elementów przy krawędziach.

## 12.4. Asynchroniczne UI API

Komunikacja AppExecutor ↔ GUI Task ma być asynchroniczna.

W szczególności `ui_create_button()` nie może wymagać synchronicznego wywołania LVGL i oczekiwania na wynik.

Zaproponowany model:

1. AppExecutor przydziela logiczny uchwyt UI.
2. Polecenie tworzenia trafia do ograniczonej kolejki GUI.
3. GUI Task tworzy właściwy obiekt LVGL.
4. Sukces albo błąd zostaje zgłoszony aplikacji.
5. Kolejne operacje wykorzystują logiczny uchwyt.

Przydzielenie uchwytu oznacza przyjęcie żądania, a nie gwarancję pomyślnego utworzenia obiektu.

## 12.5. Kolejka poleceń UI

Kolejka musi posiadać:

- Ograniczoną pojemność.
- Kontrolę właściciela.
- Identyfikator instancji.
- Identyfikator operacji.
- Politykę przepełnienia.
- Możliwość anulowania lub odrzucenia nieaktualnych poleceń.

Nie należy blokować GUI Task ani AppExecutor przy przepełnieniu kolejki.

Niektóre operacje mogą zostać połączone, np. wielokrotne ustawienie tekstu tego samego widżetu.

Operacji wymagających zachowania kolejności nie wolno arbitralnie scalać.

## 12.6. Shadow State

SDK może udostępniać lustrzany stan komponentów UI.

Pozwala to odczytywać podstawowe wartości bez synchronicznego odpytywania LVGL.

Wymagane jest określenie:

- Który komponent jest właścicielem stanu.
- Kiedy stan zostaje aktualizowany.
- Czy getter zwraca wartość zatwierdzoną, czy oczekującą.
- Jak obsługiwane są błędy synchronizacji.

## 12.7. Renderowanie

Pełna klatka RGB565:

$$
360 \times 360 \times 2 = 259200\ \text{B}
$$

Czyli około 253 KiB.

Początkowa strategia:

- Dwa częściowe bufory LVGL.
- Bufory w pamięci zgodnej z wymaganiami transferu.
- Asynchroniczne przesyłanie danych do LCD.
- Ograniczanie kopiowania.
- Natywne animacje LVGL.

Nie należy zakładać dostępności sygnału TE.

Podwójne buforowanie samo w sobie nie gwarantuje eliminacji tearingu.

Docelową strategię należy wybrać na podstawie pomiarów.

## 12.8. Custom Drawing API

Poza MVP.

Docelowe zastosowania:

- Zaawansowane tarcze.
- Wykresy.
- Wizualizacje.
- Proste gry.
- Niestandardowe kontrolki.

Nie należy wystawiać prywatnych wskaźników `lv_obj_t *` w publicznym ABI.

# 13. PackageManager i AppRegistry

## 13.1. Rejestr aplikacji

`AppRegistry` przechowuje:

- Identyfikator aplikacji.
- Zainstalowane wersje.
- Wersję aktywną.
- Wersję kompatybilną z ABI.
- Metadane.
- Stan aplikacji.
- Informacje o błędach.

Nie odpowiada za ładowanie ELF.

## 13.2. Sesje instalacyjne

Model:

```text
BEGIN
  |
  v
STAGING
  |
  v
VERIFY
  |
  v
COMMIT / ABORT
```

## 13.3. Weryfikacja pakietu

Przed aktywacją należy sprawdzić:

- Integralność.
- Podpis, gdy jest wymagany przez aktualny profil bezpieczeństwa.
- Architekturę ELF.
- ABI.
- Relokacje.
- Importowane symbole.
- Wymagania pamięci.
- Zgodność wersji.
- Dostępne miejsce.

## 13.4. Aktywacja wersji

Nowa wersja nie może nadpisywać aktywnej.

Przełączenie powinno następować poprzez trwały rekord aktywacji.

Można wykorzystać LittleFS i atomowe `rename()` pojedynczego pliku.

Przykładowa procedura:

```text
write active.tmp
flush/sync
rename active.tmp -> active
```

Atomowość pojedynczego `rename()` nie oznacza transakcyjności całego zestawu plików.

Należy testować zachowanie po przerwaniu zasilania przed i po aktywacji.

## 13.5. Format manifestu

Do rozważenia:

- JSON.
- CBOR.
- Niewielki własny format binarny.

Format powinien być:

- Wersjonowany.
- Łatwy do walidacji.
- Odporny na niepoprawne dane.
- Niewymagający dużych alokacji.
- Niezależny od prywatnych struktur firmware'u.

W MVP dopuszcza się JSON z przypiętym parserem.

# 14. StorageManager

## 14.1. LittleFS

LittleFS jest głównym magazynem aplikacji i zasobów.

Wymagana konfiguracja:

```c
.format_if_mount_failed = false
```

Nie wolno automatycznie formatować systemu plików przy nieudanym montowaniu.

## 14.2. NVS

NVS przechowuje niewielkie dane systemowe.

W szczególności:

- Konfigurację.
- Wybrane metadane.
- Stan rejestru.
- Informacje o aktualizacjach.
- Trwałe informacje diagnostyczne.

## 14.3. Zakaz automatycznego kasowania

Nie należy kopiować wzorca z przykładów ESP-IDF, w którym błędy:

```c
ESP_ERR_NVS_NO_FREE_PAGES
ESP_ERR_NVS_NEW_VERSION_FOUND
```

są automatycznie obsługiwane przez `nvs_flash_erase()`.

W REP takie błędy powinny uruchamiać ścieżkę recovery.

Kasowanie danych wymaga jawnej decyzji użytkownika.

## 14.4. Safe Mode i NVS

Safe Mode musi być możliwy do uruchomienia mimo błędów NVS.

Nie może wymagać:

- Poprawnych danych Wi-Fi.
- Automatycznej inicjalizacji sieci.
- Dostępu do danych aplikacji.
- Poprawnego rejestru pakietów.

Wi-Fi w Safe Mode jest funkcją opcjonalną, zależną od poprawnego działania wymaganych usług.

# 15. Podział Flash

## 15.1. Założenia

Pamięć Flash wynosi 16 MiB.

Nie przewiduje się:

- Factory application.
- Recovery application.
- Dedykowanego drugiego bootloadera.

Wymagane są dwa sloty OTA.

## 15.2. Wstępny budżet

| Obszar | Budżet |
|---|---:|
| OTA A | 3,5 MiB |
| OTA B | 3,5 MiB |
| LittleFS | Do 8 MiB |
| Bootloader, partition table, NVS, otadata, coredump i rezerwa | Około 1 MiB |

Rozmiary są orientacyjne.

Ostateczna tabela partycji musi uwzględniać:

- Wyrównanie partycji aplikacyjnych.
- Faktyczny rozmiar bootloadera.
- Ewentualny wzrost bootloadera przez podpisywanie.
- `otadata`.
- NVS.
- Partycję coredump.
- Rezerwę techniczną.

## 15.3. Coredump

**Decyzja implementacyjna Milestone 2A (2026-10-18):** profil `rep-v1`
wybiera dwa sloty po 3,5 MiB i LittleFS 8,8125 MiB. Tabela oraz plan
migracji są opisane w [flash-migration.md](flash-migration.md), a zakres
StorageManager w [storage.md](storage.md). Układ jest zweryfikowany offline;
nie został wgrany na urządzenie. Rezerwa coredump 128 KiB jest tymczasowa,
zapis coredump pozostaje wyłączony do pomiarów. Nie zamyka to sprzętowej
bramki etapu 2 ani przyszłych testów OTA/recovery.

Należy przewidzieć dedykowaną partycję coredump albo inny zweryfikowany mechanizm trwałej diagnostyki panic.

Jej rozmiar zostanie dobrany po określeniu konfiguracji tasków i zakresu zapisywanych danych.

Nie wolno zakładać, że coredump obejmujący wszystkie dane PSRAM będzie ekonomicznie możliwy.

# 16. USB i MTP Responder

## 16.1. Cel

USB MTP ma umożliwiać zarządzanie plikami REP bez udostępniania hostowi surowej partycji Flash.

Firmware pozostaje właścicielem LittleFS.

## 16.2. USB Device

ESP32-S3 wykorzystuje natywne USB Full-Speed na GPIO19/GPIO20.

Preferowany stos:

- TinyUSB.
- `esp_tinyusb`.
- MTP Responder.

Obsługa MTP w konkretnej przypiętej wersji `esp_tinyusb` wymaga weryfikacji. Obecność funkcji w gałęzi rozwojowej nie stanowi potwierdzenia stabilności wydanej wersji komponentu.

## 16.3. Wirtualna struktura MTP

Proponowany widok:

```text
Round Embedded Platform/
    Applications/
    Install/
    Media/
    User Data/
    Diagnostics/
```

Są to katalogi logiczne.

Nie muszą odpowiadać bezpośrednio katalogom LittleFS.

## 16.4. Bezpieczeństwo zapisu

MTP nie może umożliwiać bezpośredniego nadpisywania aktywnych plików ELF.

Instalacja aplikacji przez USB musi przechodzić przez PackageManager.

Dane systemowe, NVS i metadane aktywacji nie powinny być bezpośrednio udostępniane do zapisu.

Należy przewidzieć kontrolę dostępu do danych użytkownika. MTP samo w sobie nie zapewnia odpowiednika izolacji aplikacji lub silnego uwierzytelniania hosta.

## 16.5. USB i debugowanie

ESP32-S3 współdzieli wewnętrzny PHY pomiędzy odpowiednimi funkcjami USB.

Uruchomienie TinyUSB przez USB-OTG może oznaczać utratę aktywnego interfejsu USB-Serial-JTAG używanego wcześniej do konsoli i automatycznego resetowania.

W trybie MTP nie należy zakładać dostępności automatycznego resetu przez esptool.

Procedura ROM Recovery musi działać z ręcznym wejściem w Download Mode.

## 16.6. USB Composite

Opcjonalnie:

- MTP.
- CDC-ACM.
- Funkcja diagnostyczna.

Należy uwzględnić ograniczenia liczby endpointów, buforów i zasobów USB.

Composite nie jest wymagane w pierwszym MTP PoC.

# 17. PowerManager

## 17.1. Stany

```text
ACTIVE
SCREEN_OFF
LIGHT_SLEEP
DEEP_SLEEP
POWER_OFF
```

`POWER_OFF` oznacza rzeczywiste wyłączenie sprzętowe.

REP nie deklaruje możliwości programowego przejścia do `POWER_OFF`.

## 17.2. Menu zasilania

Menu graficzne powinno udostępniać:

- Uśpij.
- Wygasz ekran.
- Uruchom ponownie.
- Tryb bezpieczny.
- Anuluj.

Opcja „Wyłącz” może zostać dodana dopiero po potwierdzeniu możliwości sprzętowego odcięcia zasilania.

Deep sleep nie powinien być błędnie prezentowany jako pełne wyłączenie zasilania.

## 17.3. Sleep Inhibitors

System wykorzystuje zarządzane blokady uśpienia.

Przykładowe typy:

- CPU Awake.
- Display Awake.
- Network Required.
- No Deep Sleep.

Każda blokada ma:

- Właściciela.
- Typ.
- Timeout.
- Uchwyt.
- Stan.

Blokady aplikacyjne są zwalniane przez ResourceManager.

## 17.4. Wybudzanie

Kandydaci:

- BOOT / GPIO0.
- Touch INT / GPIO4.
- RTC INT / GPIO6.

Należy przetestować:

- Dostępne poziomy sygnałów.
- Zachowanie kontrolera dotykowego podczas uśpienia.
- Wybudzanie z light sleep.
- Wybudzanie z deep sleep.
- Pobór prądu.
- Czas wznowienia.
- Wpływ akumulatora.

Przy wykorzystaniu BOOT trzeba uwzględnić, że GPIO0 jest pinem strappingowym. Jego stan podczas ponownego startu może wpływać na wybór ROM Download Mode.

# 18. Boot Architecture

## 18.1. Decyzja

REP wykorzystuje:

- ESP32-S3 Boot ROM.
- Standardowy ESP-IDF second-stage bootloader.
- Dwie partycje OTA.
- Application-controlled rollback.

Nie przewiduje się MCUboot ani własnego bootloadera.

## 18.2. Konfiguracja rollback

Dla ESP-IDF v6.1:

```ini
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Wersja 6.1 nie wymaga osobnej opcji `CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP`.

W tej wersji potwierdzenie wykonywane jest jawnie przez aplikację.

Nie należy kopiować opcji z nowszej gałęzi rozwojowej ESP-IDF bez sprawdzenia docelowego Kconfig.

## 18.3. Semantyka OTA

Po OTA nowy obraz przechodzi przez:

```text
NEW
 |
 v
PENDING_VERIFY
 |
 +---- success ----> VALID
 |
 +---- failure ----> INVALID / rollback
 |
 +---- reset ------> ABORTED / rollback
```

Nowy obraz posiada jedną próbę osiągnięcia punktu zatwierdzenia.

Każdy restart przed zatwierdzeniem, także planowany, może zakończyć się rollbackiem.

## 18.4. Self-test

Self-test powinien obejmować:

- Inicjalizację krytycznych usług.
- Podstawowe informacje o pamięci.
- Weryfikację podstawowej konfiguracji sprzętowej.
- Możliwość uruchomienia minimalnego środowiska graficznego, jeśli jest to wymagane do działania produktu.
- Sprawdzenie niezbędnych struktur systemowych.

Nie powinien zależeć od:

- Wi-Fi.
- NTP.
- MTP.
- LittleFS.
- Zewnętrznych ELF.
- MicroSD.

## 18.5. Potwierdzenie obrazu

Po pomyślnym self-teście:

```c
esp_ota_mark_app_valid_cancel_rollback();
```

W przypadku błędu krytycznego:

```c
esp_ota_mark_app_invalid_rollback_and_reboot();
```

Należy obsługiwać błędy powrotne tych funkcji.

Jeżeli nie ma poprzedniego poprawnego obrazu, rollback może być niemożliwy.

W takiej sytuacji należy uruchomić ograniczony Safe Mode, o ile podstawowy firmware pozostaje zdolny do działania.

## 18.6. Firmware wgrany przez USB

Obraz zaprogramowany bezpośrednio przez USB może nie posiadać standardowego stanu `NEW` / `PENDING_VERIFY` wygenerowanego przez procedurę OTA.

Nie należy zakładać automatycznego rollbacku dla każdego obrazu wgranego z komputera.

Mechanizm zatwierdzania należy opierać na rzeczywistym stanie partycji odczytanym z `otadata`.

## 18.7. Blokada OTA przed potwierdzeniem

OTAManager nie może rozpoczynać nowej aktualizacji, gdy aktywny obraz oczekuje na zatwierdzenie.

Normalna inicjalizacja usług aktualizacyjnych powinna następować dopiero po zakończeniu krytycznego self-testu.

# 19. Safe Mode

## 19.1. Założenia

Safe Mode jest integralną częścią firmware'u REP.

Nie stanowi osobnej partycji ani osobnego obrazu aplikacyjnego.

Nie może wymagać:

- LittleFS.
- ELF Loader.
- AppRegistry.
- Zewnętrznych zasobów.
- MTP.
- Wi-Fi.

## 19.2. Boot Guard

Early Boot Guard decyduje o trybie startu.

Proponowana kolejność:

```text
app_main
   |
   v
Read reset reason
   |
   v
Validate boot state
   |
   v
Check OTA state
   |
   v
Determine startup mode
   |
   +---- NORMAL
   |
   +---- SAFE
```

## 19.3. Licznik nieudanych uruchomień

Próg początkowy: trzy kolejne nieudane uruchomienia.

Mechanizm działa dla błędów firmware'u, który może być ponownie uruchamiany.

Nie zastępuje mechanizmu OTA `PENDING_VERIFY`, ponieważ niepotwierdzony obraz OTA zwykle nie otrzyma drugiej próby startu.

## 19.4. Przechowywanie Boot Guard

Należy rozważyć obszar RTC FAST o stałym adresie, rezerwowany za pomocą mechanizmu ESP-IDF:

```ini
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
```

Rozmiar i layout obszaru muszą być wspólne dla bootloadera i obu slotów OTA.

Trzeba także uwzględnić ustawienie odnoszące się do CRC zarezerwowanej pamięci.

To kandydat na mechanizm odporniejszy na zmiany layoutu zmiennych między kompilacjami niż swobodne wykorzystywanie `RTC_NOINIT_ATTR`.

Nie zapewnia jednak zachowania danych po utracie zasilania.

## 19.5. Walidacja danych Boot Guard

Dane powinny zawierać:

- Magic.
- Wersję struktury.
- CRC.
- Licznik błędów.
- Stan bootowania.
- Identyfikator obrazu firmware'u.
- Ostatnią aktywną aplikację.
- Informację o planowanym restarcie.

Należy rozróżniać:

- Panic.
- WDT.
- Brownout.
- Power-on reset.
- Software restart.
- Deep-sleep wakeup.
- Reset wywołany przez interfejs programowania USB, jeśli jest rozpoznawany przez daną wersję IDF.

Nie należy automatycznie kwalifikować wszystkich resetów jako awarii aplikacji.

## 19.6. Funkcje Safe Mode

Wymagane:

- Podstawowe GUI.
- Diagnostyka.
- Informacja o aktywnym slocie OTA.
- Dezaktywacja aplikacji.
- Reset wybranych ustawień.
- Wyłączenie autostartu.
- Restart normalny.
- Informacja o stanie aktualizacji.

Opcjonalnie:

- Eksport diagnostyki przez USB.
- Ograniczone MTP.
- Naprawa metadanych pakietów.
- Przywrócenie poprzedniej wersji aplikacji.

## 19.7. Ręczny wybór poprzedniego firmware'u

Safe Mode powinien umożliwiać wybranie drugiego slotu OTA, jeżeli zawiera poprawny obraz.

Operacja wymaga:

- Potwierdzenia obecności obrazu.
- Weryfikacji jego poprawności.
- Sprawdzenia stanu OTA.
- Weryfikacji ograniczeń anti-rollback.
- Ostrzeżenia o możliwej niezgodności danych.
- Wyraźnego potwierdzenia użytkownika.

Przełączenie można realizować mechanizmami ESP-IDF, w tym `esp_ota_set_boot_partition()`.

Nie należy gwarantować możliwości powrotu do dowolnego historycznego obrazu.

# 20. ROM Download Recovery

## 20.1. Założenia

Ostateczną ścieżką odzyskiwania jest ROM Download Mode mikrokontrolera ESP32-S3.

Nie wymaga on poprawnego firmware'u REP ani sprawnego second-stage bootloadera w Flash.

## 20.2. Procedura wejścia

Moduł nie posiada osobnego przycisku RESET.

Należy zweryfikować następującą procedurę:

1. Wyłączyć urządzenie przyciskiem PWR.
2. Przytrzymać BOOT.
3. Ponownie włączyć zasilanie lub podłączyć USB zgodnie z zachowaniem modułu.
4. Zwolnić BOOT po rozpoczęciu trybu pobierania.
5. Sprawdzić enumerację USB.

Osobno należy przetestować scenariusz z podłączonym akumulatorem.

## 20.3. Wpływ TinyUSB

Gdy normalny firmware korzysta z USB-OTG i TinyUSB, nie należy zakładać działania automatycznego resetowania przez USB-Serial-JTAG.

ROM Recovery musi mieć udokumentowaną ręczną procedurę wejścia.

## 20.4. Zestaw recovery

Każde wydanie powinno dostarczać:

- Bootloader.
- Partition table.
- Firmware.
- Manifest offsetów.
- Informację o wersji ESP-IDF.
- Wymagane obrazy inicjalizacyjne OTA.
- Sumy kontrolne.
- Instrukcję przywracania.

Należy unikać ręcznego wpisywania stałych offsetów niezależnie od konfiguracji projektu.

## 20.5. Recovery bez utraty danych

Tryb Preserve Data powinien zachowywać:

- LittleFS.
- NVS.
- Dane aplikacji.
- Inne niezależne partycje danych.

Musi jednak uwzględniać rzeczywisty stan `otadata`.

Samo zapisanie obrazu do `ota_0` nie gwarantuje, że bootloader uruchomi właśnie ten slot.

Należy zweryfikować różnice pomiędzy `idf.py flash` i `idf.py app-flash` dla docelowego projektu oraz sposób przygotowywania `ota_data_initial.bin`.

## 20.6. Pełne przywracanie

Full Reset może usuwać konfigurację i dane użytkownika.

Jest to operacja destrukcyjna i musi wymagać wyraźnego potwierdzenia.

Obraz LittleFS nie powinien być automatycznie dodawany do zwykłego programowania recovery, jeśli mogłoby to spowodować nadpisanie danych użytkownika.

## 20.7. Narzędzia

Dla ESP-IDF 6.1 należy stosować kompatybilną wersję esptool.

Przykładowo:

```bash
python -m esptool --chip esp32s3 \
    --port /dev/ttyACM0 flash-id
```

Polecenia esptool v5 wykorzystują nazwy z myślnikami, np. `write-flash`.

Dokładna ścieżka portu USB nie może być zakładana na sztywno.

# 21. Bezpieczeństwo

## 21.1. Model zaufania ELF

Natywne ELF działają we wspólnej przestrzeni adresowej firmware'u.

Nie posiadają izolacji procesu ani MMU.

Ograniczenie eksportowanych symboli i użycie SDK nie tworzy granicy bezpieczeństwa pamięci.

## 21.2. Podpisywanie firmware'u

Do rozważenia jest wykorzystanie:

```ini
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
```

Pozwala to weryfikować podpisy aktualizacji bez włączania sprzętowego Secure Boot.

Nie zapewnia ochrony przed osobą posiadającą fizyczny dostęp do niechronionego Flash i możliwość zastąpienia firmware'u.

Podpisany musi być również odpowiednio przygotowany obraz początkowy, jeśli wymagają tego ustawienia walidacji aplikacji.

Konfiguracja podpisywania i algorytm muszą zostać potwierdzone na ESP32-S3 w wybranej wersji IDF.

## 21.3. Podpisy aplikacji

Preferowane mechanizmy:

- ECDSA P-256 + SHA-256.
- Alternatywnie inny algorytm obsługiwany przez przypięty stos PSA Crypto.

Nie należy zakładać dostępności Ed25519 bez testu.

Podpis powinien obejmować manifest i wszystkie istotne pliki pakietu.

## 21.4. Tryb developerski

REP może umożliwiać instalowanie własnych, niepodpisanych aplikacji w jawnie aktywowanym trybie developerskim.

Tryb taki musi być jednoznacznie widoczny w systemie.

Nie należy traktować niepodpisanych pakietów jako bezpiecznych.

## 21.5. eFuse

Do czasu opracowania i przetestowania zabezpieczonej procedury recovery:

- Nie włączać Secure Boot.
- Nie włączać Flash Encryption.
- Nie wyłączać ROM Download Mode.
- Nie zmieniać nieodwracalnie konfiguracji USB PHY.
- Nie włączać sprzętowego anti-rollback.

# 22. Diagnostyka

## 22.1. Wymagane metryki

- Wolny SRAM.
- Największy wolny blok SRAM.
- Wolna PSRAM.
- Największy wolny blok PSRAM.
- High-water mark tasków.
- Czas ładowania ELF.
- Szczytowe zużycie pamięci loadera.
- Czas wykonywania callbacków.
- Liczba aktywnych zasobów.
- Długości kolejek.
- Czas renderowania.
- Czas transferów LCD.
- P95/P99 opóźnień GUI.
- Czas instalacji pakietów.
- Czas przejścia w stan uśpienia.
- Pobór prądu.

## 22.2. Symbolizacja awarii ELF

Runtime powinien przechowywać w diagnostyce:

- App ID.
- Wersję aplikacji.
- Hash obrazu ELF.
- Identyfikator instancji.
- Bazowy adres załadowanego kodu.
- Informację o mapowaniu sekcji.
- Wersję ABI.
- Wersję firmware'u.

Ma to umożliwiać symbolizowanie backtrace'ów odnoszących się do kodu ładowanego dynamicznie.

Nie należy zakładać, że standardowe narzędzia symbolizacji firmware'u automatycznie rozpoznają kod ELF w PSRAM.

## 22.3. Testy długotrwałe

Wymagane scenariusze:

- Tysiące cykli load/unload.
- Wielokrotne tworzenie i usuwanie GUI.
- Wymuszone błędy inicjalizacji.
- Zdarzenia opóźnione po unload.
- Anulowanie operacji przy zamykaniu.
- Fragmentacja pamięci.
- Instalacja pakietu podczas działania GUI.
- Flash I/O podczas wykonywania ELF.
- Restart w różnych etapach OTA.
- Uszkodzenie NVS.
- Uszkodzenie LittleFS.
- Awaria MTP.
- Watchdog.
- Brownout.
- ROM Recovery.

# 23. Roadmap

## Etap 0 — Hardware Audit

- Potwierdzenie rewizji PCB.
- Analiza schematu.
- Potwierdzenie PSRAM/Flash.
- Weryfikacja PWR.
- Identyfikacja źródeł wybudzania.
- Analiza LCD_TE.
- Potwierdzenie procedury ROM Download.

**Bramka:** znane są ograniczenia fizycznej platformy.

## Etap 1 — ESP-IDF 6.1 Bring-up

- Adaptacja BSP Waveshare.
- Budowanie bez globalnego wyłączania `-Werror`.
- Nowy I²C Master.
- LCD.
- Dotyk.
- RTC.
- BQ27220.
- Minimalny LVGL.
- Diagnostyka pamięci.

**Bramka:** sprzęt działa stabilnie na przypiętym ESP-IDF 6.1.

## Etap 2 — Storage, OTA i Recovery

- Tabela partycji.
- LittleFS.
- NVS.
- OTA A/B.
- Self-test.
- Rollback.
- Early Boot Guard.
- Minimalny Safe Mode.
- ROM Recovery.

**Bramka:** system można aktualizować i odzyskiwać bez destrukcyjnego kasowania danych.

## Etap 3 — ELF Proof of Concept

- Integracja ELF Loader.
- Ładowanie z LittleFS.
- Test Picolibc i wymaganych helperów.
- Analiza importowanych symboli.
- Test relokacji.
- Wykonywanie ELF z PSRAM.
- Pomiar szczytowego zużycia RAM.
- Wielokrotne load/unload.
- Testy współbieżnego Flash I/O.
- Porównanie XIP i standardowego cache.

**Bramka krytyczna:** potwierdzona wykonalność natywnych aplikacji dynamicznych.

Jeżeli bramka nie zostanie spełniona, należy przeprowadzić przegląd architektury przed dalszym rozwojem runtime'u.

## Etap 4 — Application Runtime

- AppManager.
- AppExecutor.
- EventDispatcher.
- ResourceManager.
- Lifecycle.
- ABI vtable.
- Reguły importowanych symboli.
- Crash diagnostics.

**Bramka:** aplikacja jest uruchamiana i bezpiecznie logicznie zamykana bez narastających wycieków zasobów SDK.

## Etap 5 — Round UI

- Launcher.
- Nawigacja.
- RoundList.
- RoundDialog.
- ArcSlider.
- EdgeButton.
- Standard UI API.
- Kolejka poleceń UI.
- Shadow State.

**Bramka:** aplikacja ELF tworzy i obsługuje interfejs bez bezpośredniego dostępu do LVGL.

## Etap 6 — PackageManager i SDK

- Format manifestu.
- Sesje instalacyjne.
- Walidacja ELF.
- Instalacja.
- Aktualizacja.
- Deinstalacja.
- SDK.
- Przykładowy kalkulator.

**Bramka:** aplikację można niezależnie zbudować, zainstalować, uruchomić i usunąć.

## Etap 7 — USB MTP

- Integracja esp_tinyusb.
- MTP Responder.
- Mapowanie katalogów logicznych.
- Transfer plików.
- Instalacja paczek przez MTP.
- Testy Linux/Windows.
- Test ręcznego ROM Recovery przy aktywnym TinyUSB.

**Bramka:** kopiowanie pakietu przez USB prowadzi do poprawnej instalacji bez restartu firmware'u.

## Etap 8 — Power Management

- Screen Off.
- Light Sleep.
- Deep Sleep.
- Sleep Inhibitors.
- Wybudzanie.
- Power Menu.
- Pomiary energii.

**Bramka:** zweryfikowany co najmniej jeden tryb uśpienia z niezawodnym wybudzaniem.

## Etap 9 — Stabilizacja

- Testy długotrwałe.
- Profilowanie.
- Testy recovery.
- Testy OTA.
- Optymalizacja pamięci.
- Dokumentacja.
- Przygotowanie MVP.

# 24. Definition of Done — MVP

MVP uważa się za ukończone, gdy:

### System i sprzęt

- [ ] Działa własny firmware REP.
- [ ] ESP-IDF 6.1 i BSP są zintegrowane.
- [ ] LCD, dotyk i podstawowe peryferia działają.
- [ ] GUI jest projektowane dla okrągłego wyświetlacza.
- [ ] Działa launcher i nawigacja.

### Firmware i recovery

- [ ] Działają dwa sloty OTA.
- [ ] Rollback został przetestowany.
- [ ] Self-test zatwierdza nowy obraz.
- [ ] Działa Safe Mode.
- [ ] Safe Mode nie wymaga LittleFS.
- [ ] Działa ROM Download Recovery.
- [ ] Można odtworzyć firmware bez kasowania danych.

### Dynamiczne aplikacje

- [ ] Działa ELF Loader.
- [ ] Znany jest model wykonywania z PSRAM.
- [ ] Importowane symbole są kontrolowane.
- [ ] Działa wersjonowane ABI.
- [ ] Działa Application Executor.
- [ ] Działa lifecycle.
- [ ] Działa ResourceManager.
- [ ] Aplikacje można wielokrotnie uruchamiać i zamykać.
- [ ] Brak narastających wycieków zasobów zarządzanych przez SDK w testach.

### Instalacja i USB

- [ ] Działa PackageManager.
- [ ] Instalacja nie nadpisuje aktywnej wersji.
- [ ] Działa aktualizacja aplikacji.
- [ ] Działa USB MTP.
- [ ] Można instalować aplikacje przez USB.
- [ ] Operacje MTP nie naruszają danych systemowych.

### Energia i diagnostyka

- [ ] Działa przynajmniej jeden zweryfikowany tryb uśpienia.
- [ ] Działa podstawowe menu zasilania.
- [ ] Dostępna jest diagnostyka pamięci i aplikacji.
- [ ] Można uzyskać dane do symbolizacji awarii ELF.

### Narzędzia

- [ ] Istnieje SDK.
- [ ] Istnieje przykładowa aplikacja.
- [ ] Istnieje dokumentacja ABI.
- [ ] Istnieje procedura ROM Recovery.
- [ ] Wersje krytycznych zależności są przypięte.

# 25. Otwarte decyzje

Następujące zagadnienia wymagają pomiarów lub osobnego rozstrzygnięcia:

1. Dokładny commit ESP-IDF 6.1.
2. Ostateczna wersja ELF Loader.
3. Picolibc czy Newlib jako trwała podstawa ABI.
4. Obsługa TLS i relokacji Xtensa.
5. Techniczna implementacja białej listy symboli.
6. Model alokacji pamięci aplikacji.
7. Konfiguracja wykonywania ELF z PSRAM.
8. Włączenie XIP from PSRAM.
9. Budżety SRAM i PSRAM.
10. Tabela Flash: układ `rep-v1` wybrany w Milestone 2A i zweryfikowany offline
    ([plan migracji](flash-migration.md)); migracja i walidacja sprzętowa pozostają otwarte.
11. Wielkość partycji coredump: rezerwa 128 KiB w `rep-v1`, finalny zakres i rozmiar wymagają pomiarów.
12. Format manifestu: profil developerski RPK1 wybrany w Milestone 3D
    ([format](package-format.md)); profil produkcyjny i podpisy pozostają otwarte.
13. Format pakietu aplikacji: developerski kontener RPK1 z jednym ELF,
    bez kompresji ([format](package-format.md)).
14. Algorytm podpisu pakietów.
15. Sposób przechowywania metadanych aktywacji: Milestone 3D używa rekordu RPA1
    z numerem slotu artefaktu i SHA-256, fsync i rename pojedynczego pliku
    ([transakcja i ograniczenia dowodów](package-manager.md)); odporność
    fizycznego urządzenia na utratę zasilania pozostaje PENDING.
16. Konkretny mechanizm przechowywania Boot Guard w RTC.
17. Zachowanie RTC po brownoucie.
18. Dostępność sygnału LCD_TE.
19. Docelowa konfiguracja buforowania LVGL.
20. Stabilność MTP w wybranej wersji esp_tinyusb.
21. Procedura ROM Recovery przy zasilaniu bateryjnym.
22. Zachowanie BOOT jako źródła wybudzania.
23. Dopuszczalne czasy callbacków.
24. Budżety i polityki kolejek.
25. Ostateczna polityka Sleep Inhibitors.

Nie należy uznawać tych decyzji za rozstrzygnięte wyłącznie na podstawie dokumentacji, jeżeli można je zweryfikować prostym eksperymentem na urządzeniu.

# 26. Zasady architektoniczne

## Native First

Aplikacje wykonywane są natywnie.

## Event Driven

Aplikacje otrzymują zdarzenia, a nie własne pętle systemowe.

## Explicit Ownership

Zasoby systemowego SDK posiadają jawnego właściciela.

## Stable ABI

Publiczne ABI jest oddzielone od prywatnych struktur firmware'u.

## Controlled Imports

Natywne aplikacje mogą importować jedynie zatwierdzone symbole i funkcje runtime'u.

## Round First

Geometria ekranu jest podstawowym założeniem projektu GUI.

## Recovery First

System musi zachowywać możliwość uruchomienia bez zewnętrznych aplikacji i odzyskania sprawności przez ROM.

## No Destructive Recovery by Default

Błędy konfiguracji i storage nie mogą automatycznie powodować kasowania danych.

## Asynchronous by Design

GUI i AppExecutor nie powinny blokować się wzajemnie.

## Measure Before Optimize

Decyzje wydajnościowe wymagają pomiarów.

## Simple Before Generic

Nie należy budować ogólnych frameworków bez rzeczywistej potrzeby.

## Hardware Reality

Ograniczenia fizycznego modułu mają pierwszeństwo przed założeniami oprogramowania.

## Testable Architecture

Logika niezależna od sprzętu powinna nadawać się do testowania na hoście.

## Compatibility by Contract

Zgodność aplikacji jest określona przez jawny ABI, toolchain, manifest i wymagania runtime'u.

---

# Appendix A — Zmiany względem v0.3

| Obszar | Zmiana |
|---|---|
| Hardware | Uzupełniono ESP32-S3R8, BQ27220, ES8311, ES7210 |
| PWR | Przyjęto sprzętowy charakter przycisku |
| ESP-IDF | Ustalono v6.1 jako bazę docelową |
| BSP | Dodano obowiązkową bramkę kompatybilności |
| libc | Uwzględniono Picolibc i ukryte ABI |
| ELF | Dodano politykę importów i toolchain SDK |
| Memory | Dodano XIP, cache i budżet pamięci |
| Runtime | Dodano stan FAILED |
| ResourceManager | Doprecyzowano anulowanie i alokator |
| GUI | Dodano asynchroniczne polecenia, uchwyty i shadow state |
| Storage | Zakazano automatycznego formatowania NVS/LittleFS |
| OTA | Poprawiono konfigurację i semantykę rollbacku 6.1 |
| Safe Mode | Dodano wybór poprzedniego slotu i RTC Boot Guard |
| ROM Recovery | Uwzględniono brak RESET i wpływ TinyUSB |
| USB | Zintegrowano MTP Responder |
| Security | Dodano opcjonalne podpisywanie OTA bez Secure Boot |
| Diagnostics | Dodano symbolizację ELF i partycję coredump |

# Appendix B — Rekomendowana struktura dokumentacji

```text
docs/
    architecture.md
    hardware.md
    esp-idf-compatibility.md
    boot-and-recovery.md
    memory-layout.md
    elf-loader.md
    application-abi.md
    application-runtime.md
    application-lifecycle.md
    event-model.md
    resource-management.md
    package-format.md
    package-manager.md
    ui-guidelines.md
    ui-api.md
    storage.md
    usb-mtp.md
    power-management.md
    ota.md
    security.md
    diagnostics.md
    testing.md

AGENTS.md
README.md
```

Dokumenty powinny powstawać stopniowo, wraz z implementacją odpowiednich komponentów.

Nie należy traktować zaplanowanych komponentów jako zaimplementowanych.

# Appendix C — Źródła techniczne

- [Waveshare ESP32-S3 Touch-LCD-1.85B](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B)
- [ESP-IDF 6.1 — OTA](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/ota.html)
- [ESP-IDF 6.1 — SPI Flash Concurrency](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/spi_flash/spi_flash_concurrency.html)
- [ESP-IDF 6.1 — External RAM](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/external-ram.html)
- [ESP-IDF 6.x — System Migration](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/system.html)
- [ESP-IDF 6.x — Security Migration](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/security.html)
- [ESP-IDF 6.x — Peripheral Migration](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/migration-guides/release-6.x/6.0/peripherals.html)
- [ESP-IDF — Kconfig Reference](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/kconfig-reference.html)
- [Espressif ELF Loader](https://components.espressif.com/components/espressif/elf_loader)
- [Espressif USB Components](https://github.com/espressif/esp-usb)

---

# 27. Konkluzja

Round Embedded Platform jest lekką, natywną platformą firmware dla ESP32-S3, zapewniającą:

- Modularny System Core.
- Dynamiczne aplikacje ELF.
- Stabilne SDK.
- Zarządzany lifecycle.
- Asynchroniczne GUI.
- Instalację pakietów.
- USB MTP.
- OTA A/B.
- Safe Mode.
- ROM Download Recovery.

Projekt pozostaje systemem embedded, nie pełnym systemem operacyjnym.

**Najważniejszą bramką techniczną jest potwierdzenie kompatybilności BSP i ELF Loader z ESP-IDF 6.1, a następnie niezawodnego wykonywania i zwalniania natywnych aplikacji przy rzeczywistych operacjach Flash, PSRAM i LCD.**

Dopiero po przejściu tych testów należy stabilizować ABI oraz implementować docelowy Application Runtime.

**REP v0.4 — koniec dokumentu.**
