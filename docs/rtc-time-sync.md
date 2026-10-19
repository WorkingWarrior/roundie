# RTC UTC synchronization and clock diagnostics

The original Milestone 1 synchronization/retention record below is historical.
For firmware 0.2.4/0.2.5, see [clock investigation](#clock-investigation--2026-10-19).

## Policy and scope

**Implementation decision:** PCF85063 stores UTC, with years 2000–2099
inclusive (`946684800..4102444799` Unix seconds). Presentation uses
`gmtime_r()`; neither the driver nor the utility changes the global timezone.
The explicit development command trusts the host clock; it does not
authenticate time cryptographically. No build-time timestamp, NTP, Wi-Fi,
TinyUSB class, persistent setting, or production TimeService is added.

`CONFIG_ROUNDIE_DEV_TIME_SYNC` defaults to **off** in Kconfig and is explicitly
enabled by this diagnostic image's `sdkconfig.defaults`. It depends on the
existing secondary USB Serial/JTAG console. Disable it before a production
release: anyone with serial access can overwrite time or request a restart.
An import allowlist or native application trust policy would not secure this
development interface.

## Linux utility

Python 3 and pyserial are required. On CachyOS/Arch:

```sh
sudo pacman -S python python-pyserial
```

The [Arch package](https://archlinux.org/packages/extra/any/python-pyserial/)
is `python-pyserial`; validation here used pyserial 3.5. Alternatively:

```sh
python -m venv /tmp/roundie-time-venv
/tmp/roundie-time-venv/bin/python -m pip install pyserial
```

From the repository, with no other serial monitor holding the port:

```sh
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_28:84:85:87:98:7C-if00
python tools/roundie_time.py --port "$PORT" status
python tools/roundie_time.py --port "$PORT" sync
python tools/roundie_time.py --port "$PORT" verify
python tools/roundie_time.py --port "$PORT" verify --restart
```

Use the actual device's by-id path; `/dev/ttyACM0` is also accepted. `status`
reports availability, validity, OS, RTC/system UTC, device error and approximate
host difference. Invalid calendar contents are not presented as trusted UTC.
`sync` samples timezone-aware host UTC immediately before transmission, checks
the acknowledgment, then performs an independent GET. `verify` never writes
RTC registers. `verify --restart` explicitly performs one `esp_restart()` and
checks retained time and restored wall clock with bounded reconnection.

Normal requests have a five-second deadline, 100 ms serial reads and a 500 ms
write timeout. Restart reconnection has a 15-second budget, one-second request
deadlines and 250 ms retry spacing (one final attempt can finish after the
budget). No unbounded `flush()` or automatic SET retries are used. Exit codes:
0 = status delivered or requested validation passed; 2 = serial/framing/
timeout/permission error (also argparse usage errors); 3 = RTC/time validation
failure. `status` intentionally exits 0 even if it reports an invalid RTC.

The utility requests exclusive access and keeps DTR and RTS both asserted,
matching Linux CDC ACM's open state, with hardware flow control disabled.
**Device-verified:** forcing each line low through separate pyserial calls
caused a USB reset on this ESP32-S3. Keeping both asserted avoided resets over
repeated opens. This is a tested Linux USB Serial/JTAG behavior, not a promise
about other adapters or operating systems. Permission failures name the port;
check its ownership/ACL and membership of its device group rather than running
the utility as root. It never invokes esptool or writes Flash.

## Transport and protocol RT1

**Source-verified:** pinned ESP-IDF v6.1 commit
`fff9895c82d744c7237be8847347bdd1b07c6643` selects UART0 as primary and
USB Serial/JTAG as secondary console. Secondary VFS `/dev/secondary` is
registered at startup. Its nonblocking read checks the standard driver's RX
ring; without installing that driver it never observes input in this revision.
The sensor worker installs the standard USB Serial/JTAG interrupt driver with
256-byte TX/RX rings, selects driver VFS and LF input, and opens that existing
VFS nonblocking. No USB device class or additional task is introduced.

Requests are printable ASCII and LF terminated (not CRLF). The host sends an
extra LF first to discard a prior incomplete frame. Empty lines are ignored.
The request identifier is exactly eight lowercase hexadecimal digits:

```text
!RT1 1234abcd GET
!RT1 1234abcd SET 1792314126
!RT1 1234abcd RESTART
```

GET samples RTC freshly. SET accepts decimal Unix seconds only (1–10 digits,
no signs, suffixes, spaces, or fractions), then checks the supported range.
RESTART is solely the development retention-test command; it restarts only
after an OK response, allowing up to 100 ms TX completion plus 100 ms delay.

Each reply starts with an extra LF, then a single identified record:

```text
!RT1 1234abcd OK state=0 os=0 rtc=1792314127 wall=1792314127 err=0
```

The entire record is enqueued in one bounded (100 ms) driver write. The same
driver's TX mutex/ring is used by console logging; logs cannot appear inside
that record. The host accepts only full, strictly parsed records with its own
identifier. Other IDs and logs are ignored. A truncated/lost reply yields a
transport failure, never assumed success. The host bounds reply lines at 256
bytes. Keep one request outstanding; this is a development link, not a
lossless high-rate command queue.

Firmware input capacity is 63 bytes before LF plus a NUL terminator. Overflow,
nonprintable input, or a partial frame older than two seconds discards through
the next LF. A suffix cannot execute as SET. Unsupported framing/version or a
bad ID uses response ID `00000000` if a safe matching ID cannot be extracted.

| Result | Meaning |
|---|---|
| `OK` | GET completed, SET validated and synchronized, or restart accepted |
| `BAD_COMMAND`, `FRAME`, `RANGE` | Syntax, framing, or timestamp rejection; no RTC write |
| `RTC_SET` | Existing calendar-setting API failed, including mode, I²C, OS or immediate readback rejection |
| `RTC_READ`, `RTC_INVALID` | Fresh RTC read failed or calendar/OS invalid |
| `RTC_VERIFY` | Frozen/backward/implausible time, or progression observation exceeded its bound |
| `CLOCK` | Valid RTC could not initialize the system clock |

`err` is the underlying ESP-IDF operation error (0 for pure protocol
rejections). `state` uses existing sensor states: 0 valid, 1 not found,
2 communication failure, 3 invalid, 4 unavailable (5/6 are battery-only).
`os` is 0/1 after a completed RTC register read, otherwise -1 (unknown).
`rtc=-1` means no valid RTC UTC; `wall=-1` means explicitly unsynchronized.
Protocol and transport errors are separate from RTC operation errors.

## Calendar, OS and system clock

**Source-verified:** the [NXP PCF85063A Rev. 7.3 datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf),
§§7.3.1.1 and 7.4, defines OS and the coherent seconds-through-years access.
The existing setting API writes all seven BCD calendar registers in one I²C
transaction with OS=0 in Seconds; no control, alarm, offset or gauge registers
are written. STOP, 12-hour, and external-test modes are rejected without
changing them. Every I²C transaction has a 100 ms timeout (below the one-second
calendar-access limit). OS can remain asserted if oscillation is not stable;
the readback must reject that condition. OS decoding is independent of
calendar validity, so an invalid date does not hide the flag.

Software validates BCD, month/day lengths, Gregorian leap years, weekday/date
agreement, hours/minutes/seconds, and years. Year register 00 means 2000,
99 means 2099; no century rollover is inferred. A defect in the old Unix
conversion's month-length initializer (missing November/December entries)
was corrected and regression tested over every supported date.

SET requires immediate valid readback in `[requested, requested+1]` seconds,
then a second valid sample after yielding the worker for 1200 ms. Both samples
must have OS clear. The measured monotonic interval must be 1.1–3 seconds,
and the RTC must advance by 1 through `floor(interval)+1` whole seconds
(normally 1 or 2). This tolerates a second/minute/day boundary without accepting
a frozen clock. Synchronization near the end of 2099 can fail when a required
readback crosses the supported year boundary; 2100 is never accepted.

Only after these checks does `settimeofday()` set ESP-IDF wall time. Pinned
IDF uses signed 64-bit `time_t`; a target static assertion enforces its size.
The pure conversions do not use `mktime()`, localtime, TZ, DST or libc calendar
layout. `esp_timer_get_time()` remains monotonic. Valid RTC at boot also
initializes wall time; OS/invalid/failed reads cannot do so. Any observed RTC
failure marks the synchronization state false. A failed explicit SET latches
unsynchronized for that boot until another successful SET; a later valid poll
cannot hide that failure. This latch is not persistent across a restart.

Expected RTC/system agreement is at most one second from whole-second sample
and set phase. The host accepts at most three seconds absolute RTC/host
difference with round trip at most three seconds; the normal 1.2-second SET
progression wait is included. These are seconds-resolution checks, not
millisecond accuracy, drift calibration, or host clock authentication.

## Ownership, GUI and cost

The existing priority-4, 4096-byte sensor task retains RTC/gauge ownership
after the startup reads. It polls serial every 50 ms and sensors every five
seconds; the previous double five-second delay was corrected. SET's 1200 ms
wait delays only that worker. GUI/touch/LVGL stay in the GUI task and use the
existing managed new-driver I²C bus. No new concurrent RTC caller exists.
RX frame (64 bytes), TX response (192 bytes) and calendar samples are bounded
stack/local data; the IDF driver's rings and synchronization objects are
allocated once for firmware lifetime. No per-request heap allocations occur.
Actual worker stack margin has not been measured; no overflow/watchdog fault
was observed during the short device checks.

The GUI gives current RTC failures precedence over an old wall-clock value:
unavailable/not-found, invalid OS/calendar, and I²C error remain distinct;
a valid synchronized clock displays `Synced UTC` and the date/time. A failed
SET with valid calendar still shows unsynchronized. Battery labels, memory
diagnostics, swatches, animation and touch paths are retained. Serial startup
confirms drivers, not visual appearance or physical touch response.

## Validation record, 2026-10-18

Results below concern this milestone only, not the complete hardware gate.

| Evidence tier | Result |
|---|---|
| Host C logic | C11 and C23 codec/parser executables all exited 0. Tests cover all 36,525 dates at four seconds per date, BCD/OS, leap/month/year boundaries, invalid ranges and weekdays, immediate/progression tolerances, malformed/overflow/expired frames. |
| Host Python mocks | All 15 tests passed, exit 0: strict and truncated replies, correlation, deadlines, disconnection, no-reset line configuration, simulated transient I²C error replies/recovery, restart reconnect, OS rejection, seconds-boundary tolerances and exit codes. These do not inject real firmware I²C faults. |
| Target compilation | Pinned ESP-IDF v6.1 build passed; current app image 587,088 bytes. Final image SHA-256 `f9fa9d6b8584fd1b1fb3e95d7aea48c29ff3655a774d786a4e0f308c80ee40cf`; ELF SHA-256 `27c13741c6ffc7e6e44be7948eda1068c7b495d88f009e95b110e924a89823f9`. |
| Deployment | Read-only identification found ESP32-S3 rev 0.2, MAC `28:84:85:87:98:7c`, 16 MB Flash, embedded 8 MB PSRAM. Each write used `tools/flash_bringup.py --write`, which rechecked the preserved backup, live partition table, erased OTA metadata and compatible bootloader, then wrote only factory application offset `0x20000`; esptool hash verification passed. No other region write was requested; no redundant Flash readback was performed. |
| Device sync | Initially OS=1, invalid RTC, unsynchronized wall clock. Explicit host SET cleared OS, confirmed advancement, and initialized system UTC. Final utility sync at 09:02:07 UTC returned RTC/system `1792314127`, RTT 1.221 s and approximately -0.07 s host difference. Independent GET passed. Host reported NTP-synchronized; this is not an external accuracy measurement. |
| Device restart | `verify --restart` passed: RTC advanced 2 s over approximately 1.87 s, OS=0, system restored within one second. A separate serial capture of the same restart path recorded `rst:0xc (RTC_SW_CPU_RST)`, valid RTC startup and wall-clock initialization. Normal repeated port opens with both modem lines asserted did not reset the board. |
| Device progression (Test A) | RTC `1792314258` → `1792314263`, +5 s over 5.0105 s monotonic elapsed, OS=0, final GET RTT 10.3 ms; RTC/system agreement within one second. |
| User-performed physical retention (Test C) | **PASSED — approximately 90 minutes, one physical test.** USB disconnected, battery connected, hardware PWR off; after restoration and USB reconnection, `status` without resynchronization reported valid RTC, OS=0 and restored system UTC. [Exact conditions, output and limits](#test-c--physical-retention-passed). |
| Device rejection/recovery | Seven negative requests returned expected codes: extra GET argument and negative timestamp (`BAD_COMMAND`), below/above range and oversized integer (`RANGE`), overlong line and 2.3-second partial frame (`FRAME`). Following GET remained valid, OS=0 and within one second of system time; no malformed SET wrote RTC. |
| Device GUI/sensors | Boot identified the installed ELF prefix `27c13741c`, LCD variant 2, CST816S driver OK, RTC and gauge ACK, battery present/3971 mV/estimated SoC 84%. No panic/watchdog error appeared in restart captures. Visual layout/touch and gauge accuracy remain unverified for this change. |

Test B's final same-connection capture also recorded RTC `1792314266` before
restart and `1792314270` afterward, OS=0 and wall time `1792314270`, with the
software-reset reason and valid startup messages. Earlier candidates exposed
the missing RX driver and reset-on-open sequence; their timeouts were failures,
not successful synchronization.

Reproduce host checks (outputs under `/tmp`, no serial device needed):

```sh
for STANDARD in c11 c23; do
    cc -std="$STANDARD" -Wall -Wextra -Werror \
      -Icomponents/roundie_board/include -Icomponents/roundie_board \
      components/roundie_board/sensor_codec.c tests/test_sensor_codec.c \
      -o /tmp/roundie-sensor-tests
    /tmp/roundie-sensor-tests
    cc -std="$STANDARD" -Wall -Wextra -Werror -Imain \
      -Icomponents/roundie_board/include -Icomponents/roundie_board \
      components/roundie_board/sensor_codec.c main/time_protocol.c \
      tests/test_time_protocol.c -o /tmp/roundie-time-protocol-tests
    /tmp/roundie-time-protocol-tests
done
python -m unittest discover -s tests -p 'test_roundie_time.py' -v
```

## Test C — physical retention: PASSED

**User-performed physical hardware test, 2026-10-18:** RTC battery-backed
retention **PASSED — approximately 90 minutes, one physical test**. This
closes the previously pending hardware PWR-off retention check for these
conditions, independently of the earlier software restart test.

The user confirmed the following sequence:

1. RTC had previously been synchronized with the host computer.
2. USB was disconnected; the battery remained connected.
3. The device was switched off using its hardware PWR button.
4. Approximately 90 minutes elapsed.
5. The device was powered on and reconnected to USB.
6. The user ran the RTC `status` command **without another synchronization**.

Actual command output supplied by the user:

```text
RTC available: yes; state: valid
RTC UTC: 2026-10-18T11:21:04+00:00; OS: 0
System UTC: 2026-10-18T11:21:03+00:00
Response: OK; device error: 0; round trip: 0.017s
RTC minus host UTC: approximately -1.27s (seconds resolution)
```

**Device evidence and conclusion:** PCF85063 remained accessible, its stored
time remained valid, and OS was still clear after the battery-backed hardware
power-off interval. The observation supports continued RTC timekeeping from
battery-backed power. REP restored the system wall clock from valid RTC;
the reported system time was one second behind RTC, within the documented
tolerance. No post-test SET preceded the observation.

**Evidence limits:** The reported -1.27-second host comparison is the
approximate offset at observation, not a calibrated measurement of drift
accumulated over 90 minutes. No oscillator accuracy in ppm can be derived
from this single result. The interval is approximate, and this was one test,
not a reliability series. It does not establish long-term oscillator accuracy,
depleted-battery behavior, or retention with the battery physically
disconnected. Active GUI operation on battery, brownout, current draw, real
I²C fault injection and endurance remain separate pending checks. No repeat
test, reset, synchronization, build or Flash operation was performed to record
this user-provided evidence. Milestone 1 remains open; see the [remaining
validation gates](bringup.md#remaining-milestone-1-validation-gates).

## Clock investigation — 2026-10-19

### Historical finding and scope

**Verified historical evidence:** the 2C.1 long-uptime read-only record
`redundancy-restore-2026-10-19/readonly-time-diagnostic.json` contains RTC/system
seconds `1792392371/1792392373`, `1792392372/1792392374`, and
`1792392373/1792392375`. All three were valid, OS=0, error=0; host round trips
were 12.65, 9.69 and 10.11 ms. The unchanged integer-second agreement test
**FAILED by two seconds**. RTC advanced, and agreement passed after restart
without SET. This remains a failure, not a retrospectively corrected PASS.
The earlier approximately −3.8 s host comparison and the user-performed
approximately 90-minute battery-backed PWR-off retention PASS are separate
observations; neither measures RTC/system frequency disagreement.

**Inference:** flooring both clocks can hide almost a second in either
direction. With a short transaction and uninterrupted wall clock, a two-second
integer difference cannot be explained entirely by simultaneous quantization.
The historical short USB round trips also argue against a one-second worker
stall inside those replies. However, the record lacks fractional timestamps,
device uptime, synchronization count and a comparable start-of-boot sample.
It cannot establish a drift rate, identify which oscillator is more accurate,
or exclude an earlier clock/source discontinuity.

### Exact production path and pinned IDF findings

**Source-verified:** `roundie_board_rtc_read()` reads Control_1, then the seven
calendar bytes starting at 0x04 in one `i2c_master_transmit_receive()` burst on
the existing 100 kHz master. Transactions have 100 ms driver timeouts. Calendar
validation uses integer UTC conversion, year 2000–2099, BCD/range/leap-day and
weekday checks. OS or unsupported control bits reject time. Communication
failures preserve their error/state; an optional error probe is included in
the new diagnostic bracket. No driver change was warranted.
[NXP PCF85063A Rev. 7.3 §7.4](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf)
specifies that calendar updates are blocked during the single burst and any
pending tick is applied afterward; access must finish within one second.
Thus the calendar is coherent, but its snapshot instant is somewhere within
the operation, not the host receipt time.

`sync_wall_clock()` is the only firmware `settimeofday()` call. Startup reads
RTC, then gauge/logs, and sets system time to the sampled RTC **whole second,
microseconds zero**. This does not align fractional phase: system time can
initially lag RTC by its unknown phase plus sample age. It does not inherently
explain the historical system-ahead direction. After publication only
`system_worker` owns RTC operations. The existing five-second worker and RT1
GET can reinitialize wall time when a valid RTC returns after failure, unless
an explicit SET failed; successful host SET also synchronizes system time.
There is no continuous discipline, SNTP, other application wall-clock writer
or clock-to-RTC writeback. GUI reads system `time()`/`gmtime_r()`, not I²C.
The new `sync` counter and `at` monotonic timestamp expose successful wall
initializations during this boot. They are diagnostic RAM, not NVS state.

Pinned ESP-IDF is v6.1 commit `fff9895c82d744c7237be8847347bdd1b07c6643`,
toolchain `esp-15.2.0_20251204`, Picolibc with Newlib compatibility and 64-bit
`time_t`. Generated REP configuration selects
`CONFIG_LIBC_TIME_SYSCALL_USE_RTC_HRT=y`, both
`CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y` and `...USE_ESP_TIMER=y`,
`CONFIG_ESP_TIMER_IMPL_SYSTIMER=y`, internal RC RTC slow clock, 40 MHz XTAL,
and disabled power management. These refer to the **ESP's internal RTC**,
not the I²C PCF85063.

Inspected pinned sources: `components/esp_libc/src/time.c`, `timekeeping.c`,
`port/esp_time_impl.c`, `components/esp_timer/src/system_time.c`, and
`esp_timer_impl_systimer.c`. Active `gettimeofday()`/CLOCK_REALTIME is boot
offset plus high-resolution SYSTIMER progression; CLOCK_MONOTONIC uses system
elapsed time. `esp_system_get_time()` is `esp_timer_get_time()` plus a fixed
startup correction. Internal RTC establishes the timer offset at reset/light
sleep and continuity during shutdown; it is not continuously polled as the
awake wall-clock frequency source. No application sleep path was found.
PCF85063 and SYSTIMER are independent clocks; their relative accuracy was not
calibrated by this inspection.

`adjtime()` is supported: realtime single-shot offset, applied at 1/64 of
elapsed microseconds, replacing an outstanding correction, with magnitude
validation in `esp_libc_timekeeping_adjtime_apply()` (2146 s on this target).
`settimeofday()` stops an outstanding slew. CLOCK_MONOTONIC is not adjusted;
frequency adjustment is unsupported. Availability does not justify enabling
corrections from second-resolution observations.

**Timing audit:** OTA transfer/confirmation deadlines, BootGuard stability,
USB frame expiry/restart delays, LCD wait/watchdog, sensor polling and GUI
scheduling use `esp_timer_get_time()`, passed monotonic timestamps or FreeRTOS
ticks/delays. Wall time is used for UTC display and RT1 diagnostics. No
discipline, cadence, correction policy or clock-source change is introduced.
OTA confirmation still does not depend on RTC agreement; Safe Mode can start
without I²C or valid RTC.

### Read-only measurement contract

Request `!RT1 <eight-lowercase-hex-id> DIAG` uses the existing printable-line
framer and system worker. It returns `DIAG state=… os=… rtc=… err=… valid=…
wb=… wa=… mb=… ma=… usable=… lo=… hi=… sync=… at=…` on one line.
`wb/wa` are system UTC microseconds before/after the complete RTC read;
`mb/ma` are enclosing `esp_timer_get_time()` microseconds. `ma` is uptime,
and `ma-mb` is the **whole read duration**, including Control_1, bus contention,
decoding, timestamp overhead and any error probe; it is not bus-wire time.
`rtc` is decoded Unix seconds, −1 if decoding fails; only usable samples are
trusted. `valid` reports the service's existing wall-clock trust state;
state/OS/error report this actual read. `sync/at` identify the last successful
system initialization. Unusable `lo/hi` zeros are placeholders, never offsets.

At the unknown snapshot instant, RTC UTC is in `[R, R+1)` seconds. Assuming
the system wall clock progresses without a step during the read, a
conservative closed enclosure for **RTC minus system** is:

```text
lo = R * 1,000,000       - wa
hi = (R + 1) * 1,000,000 - wb
width = 1,000,000 + (wa - wb) microseconds
```

Bounds have microsecond endpoints, not microsecond RTC accuracy. No midpoint
is treated as true phase. OS=1, invalid calendar, read failure, untrusted wall
time, out-of-domain timestamps, reversed monotonic/wall samples, read duration
over one second, or wall/monotonic duration mismatch over 2 ms make the sample
unusable. The 2 ms rejection gate allows timestamp-call overhead; it is not a
new agreement tolerance. The sole clock-setting owner cannot set time inside
this handler. Undetectable out-and-back external steps are not ruled out by
two endpoint observations alone. Signed 64-bit arithmetic is domain-checked.

`DIAG` returns before legacy GET's recovery synchronization or cached-status
publication. It does not SET RTC/system time, calibrate, modify NVS, trigger
OTA or restart. It performs one requested read, creates no task or permanent
polling. It uses bounded stack storage and existing USB rings; the time-only
configuration reserves a 512-byte TX ring (REP already reserves 2048 bytes).
Physical fault injection and precise worker stack margin remain unmeasured.
Existing explicit `roundie_time.py sync` semantics are unchanged.

From the repository, with the actual serial path:

```sh
python tools/roundie_clock.py --port "$PORT"
python tools/roundie_clock.py --port "$PORT" --at 0 30 120 300 --json /tmp/roundie-clock.json
```

Firmware 0.2.4 DIAG is required. The companion reuses existing serial framing
and no-reset line settings. It accepts 1–16 increasing sample offsets within
0–600 s, has a three-second request deadline, and exclusively creates JSON
(never overwrites). Partial observations are retained if a later query fails.
Exit 2 denotes transport/output failure; 3 denotes unusable measurements.
An unresolved trend is a valid diagnostic outcome, exit 0.

The JSON retains device endpoints, bounds, uptime, whole-read duration, host
wall/monotonic nanosecond brackets, USB round trip and observable timedatectl
synchronization state. RTC-minus-host bounds use the **host TX/RX interval**
and a separate second-resolution enclosure. Host NTP state is not a guarantee
of UTC accuracy. Within-request host steps over 2 ms invalidate that reference;
between-request excess over max(2 ms, 1000 ppm elapsed) rejects a series rate.
Device restart, sync-count/last-sync changes, device wall/monotonic steps,
overlapping or invalid samples likewise reject a rate.

Rate analysis intersects constraints from all sample pairs, allowing each
snapshot to occur anywhere in its measured bracket. It reports a feasible
**constant relative-rate interval**, not a regression point or statistical
confidence level. A zero-containing interval means unresolved drift; an empty
intersection means that constant-rate model does not fit. No ppm accuracy is
inferred from a single host offset. With whole-second observations, a 300 s
window generally cannot resolve ordinary tens-of-ppm disagreement. The model
does not prove frequency is constant outside the measured window.

The existing `abs(rtc_seconds-wall_seconds) <= 1` startup/RT1 check remains
unchanged. New interval diagnostics supplement it; overlap with ±1 s does
not prove the entire possible physical offset is within ±1 s. Host-UTC
validation remains separate and is not silently resynchronized.

### Validation and physical installation

**Verified host:** `python -m unittest discover -s tests -v` under the pinned
IDF environment passed **61 tests**, exit 0. This includes production C
offset arithmetic, existing exhaustive calendar codec and RT1 parser in C11
and C23 with ASan/UBSan, synthetic positive/negative/stable rate intervals,
quantization/carry boundaries, non-atomic/slow reads, invalid RTC/OS/read errors,
reference steps, restart/sync identity changes and long-uptime/epoch bounds.
Existing mocked OTA, storage and BootGuard suites passed. The first invocation
without `IDF_PATH` failed to import two existing partition-generator tests;
rerunning in the documented exported environment resolved those errors.
No physical I²C fault injection is claimed from host tests.

**Verified target:** `idf.py -B build-rep -DROUNDIE_PROFILE=rep
-DPROJECT_VER=0.2.4 build` passed, exit 0, no compiler warnings/errors.
`tools/validate_storage_layout.py --build build-rep` passed, exit 0:
app 681,632 bytes, slot capacity 3,670,016 bytes, headroom 2,988,384 bytes.
The application SHA-256 is
`848915c9e35c7802b9d6d78faeba75be8643e0429988d4abff088d36a0ae28cf`;
embedded full ELF identity is
`0b961f9788d8f240dce8f37ffda4f5ca10eb3a5eaeeab7eac51e9b1cc06d33a8`.
Firmware implementation is commit `3733d32`. Build artifacts are not hardware
validation and no generated bootloader/table was installed.

**Verified physical baseline:** first read-only queries of installed 0.2.3 B
found VALID/normal/STABLE, no failures/latch/injection, VALID 0.2.1 A fallback,
RTC valid/OS=0 and three equal RTC/system second readings. Host reported
NTP=yes and NTPSynchronized=yes. The later saved baseline independently
preserves another three observations before any preflight reset, all RTC one
integer second ahead of system; these are
integer-second observations, not frequency measurements. No RTC SET occurred.

**Verified physical preflight:** USB identity, ESP32-S3 chip identity, 16 MiB
Flash, original backup availability, installed bootloader and exact REP table
matched the known unit. Reads were limited to the 64 KiB prefix and two
288-byte app headers, not full Flash. Both CRC-valid metadata entries were
VALID (sequences 9/A and 10/B); headers matched original A 0.2.1 and B 0.2.3
identities. The expected ROM preflight restart returned to B, with fresh RTC
validity and unchanged one-second startup agreement passing.

**Verified initial physical OTA (0.2.4):** one genuine RO1 upload from running B to
inactive A, READY/digest/identity validation, activation and acknowledged
restart. A logged PENDING_VERIFY at 1160 ms and VALID at 3280 ms following
core self-test ESP_OK. This 2.120 s interval is from application logs, not
the complete bootloader pending period. B's confirmed 0.2.3 ELF remained
`9bedb988fe27a4655c26622fe23a048df24f8830b6728bb9fd22bc21f7b8acac` and
eligible as rollback fallback. No slot switch back to B was performed.

The first postboot host harness **failed** an assertion requiring BootGuard
STABLE after a 12-second capture. It did not retain that exact RS1 response;
the ten-second stability period following core health makes an early query
plausible, but its original stage is not asserted as measured. No OTA/reset
was repeated. Subsequent independent read-only RO1/RS1 checks passed:
A 0.2.4 VALID, B 0.2.3 VALID/compatible, normal/STABLE, failures/latch/trial/
test_build/fault_remaining=0. This distinction preserves the failed harness
result rather than relabeling the entire original script PASS.

NVS initialized ESP_OK without erase fallback, LittleFS mounted with
9,240,576 total / 8,192 used bytes and no provisioning/remount write test,
RTC startup was valid/OS=0, gauge responded, LCD variant 2 initialized and
GUI/core DMA health passed. The captured boot had no panic, brownout,
watchdog or stack-canary failure. An initial touch probe NACK was followed
by successful CST816S identification/driver initialization. These are live
service observations; human visual/touch, calibrated gauge, bytewise user-data
retention and physical failure injection were not retested.

The first inspected DIAG at uptime 83.444 s reported RTC−system
`[+0.665378, +1.667351] s`, whole read 2031 µs, host RTT 9.980 ms,
valid/OS=0, sync=1. Independent RT1 returned RTC one integer second ahead,
passing the unchanged criterion. Unknown startup fractional phase can explain
this direction/magnitude; that observation alone does not demonstrate a
startup defect or a frequency slope. In particular, a broad upper bound above
one second is not evidence that the true offset exceeds one second.

**Verified physical five-minute series on 0.2.4:** requested offsets 0, 30,
120 and 300 s. Host reported NTP synchronization at the series start;
endpoint timestamps detected no reference discontinuity. All four samples
were usable, RTC valid/OS=0/error=0,
sync=1 and last-sync uptime 1.114846 s. No RTC SET or later system correction
occurred during the series. The identical sync identity and wall/monotonic
consistency checks passed; no host reference discontinuity was detected.

| Requested offset | Device uptime | RTC−system enclosure | Whole read | USB RTT |
|---|---|---|---|---|
| 0 s | 120.464 s | +0.645025 … +1.647139 s | 2184 µs | 9.040 ms |
| 30 s | 150.464 s | +0.645362 … +1.647324 s | 2017 µs | 8.570 ms |
| 120 s | 240.464 s | +0.645236 … +1.647326 s | 2150 µs | 9.298 ms |
| 300 s | 420.464 s | +0.645362 … +1.647324 s | 2017 µs | 9.993 ms |

Each enclosure is about 1.002 s wide. All retain the existing one-second
integer agreement criterion; the possible physical offset is not thereby
proven entirely inside ±1 s. The feasible constant relative-rate interval is
approximately **[−3339.3, +3341.1] ppm**, containing zero: **drift unresolved**,
not a measured oscillator ppm estimate. Stable-looking bounds do not prove
zero drift. RTC advanced 300 seconds over approximately 300 device-monotonic
seconds, passing the unchanged two-second progression check. Separate
RTC-minus-host enclosures ranged over −7.394264 … −6.384205 s, including
second resolution and USB round-trip uncertainty. They are not an RTC ppm
estimate or proof of host UTC accuracy.

**Conclusion: Case D — insufficient evidence for the historical frequency
cause; no clock discipline justified.** Quantization and startup phase are
demonstrated limitations of the old diagnostic. Independent frequency drift
is a plausible explanation for the historical system-ahead result, but this
short series cannot establish it, assign oscillator accuracy, or reproduce
that long-uptime condition. There is no demonstrated conversion, snapshot or
startup-alignment bug to fix. This goal does **not** claim the RTC drift is
fixed or long-term agreement validated. The historical −2 s failure remains
open; many-hour agreement and absolute UTC accuracy are PENDING.

At that checkpoint independent RO1/RS1/RT1 queries passed: **A 0.2.4 VALID/running;
B 0.2.3 VALID/fallback**, normal/STABLE, failures/latch/trial/injection=0,
RTC valid/OS=0, unchanged one-second integer agreement. Genuine transfer
through activation took 70.221 host-monotonic seconds. No rollback fault
experiment, endurance run, RTC register write,
bootloader/table replacement, data erase or whole-Flash readback was performed.

Evidence is outside Git at
`/mnt/Dane/Development/roundie-backups/rtc-investigation-2026-10-19/`:
`run_once.py`, `baseline-0.2.3.json`, `preflight-passed.json`, `preflight.log`,
`before-upload.json`, `ready.json`, `activation.json`, `restart-ack.json`,
`ota-timing.json`, `ota-protocol.log`, `diagnostic-a-boot.log`, `install.log`
(retained failed harness), `postboot-independent.json`,
`independent-verification.log`, `image-info.log`, `five-minute-series.json`,
`five-minute-series.log`, `final-device-state.json` and `acceptance.json`.
Build/test/layout logs are `/tmp/roundie-rtc-{build,tests,layout}.log`.
Artifacts, raw identity data and generated evidence are not committed.

### Publication guard correction — 0.2.5

**Verified source finding:** final review found that a DIAG arriving before
board publication could read startup's synchronization counter/timestamp while
startup was writing them. A 64-bit diagnostic timestamp on the 32-bit target
must not be sampled concurrently without ownership transfer. Such a sample
already had untrusted wall time and could never drive a correction; nonetheless
the new diagnostic's early-startup state access required correction.

Commit `72bf6a9` reads these fields under the existing publication lock **only
after board_ready**; before publication it reports sync=0/at=0 without reading
startup-owned fields. After publication the system worker remains sole owner.
No clock-setting behavior, interval arithmetic, calibration, task or cadence
changed. The 0.2.4 five-minute measurements above were after publication and
are unaffected by this source-level finding.

**Verified host/target:** the complete 61-test suite passed again, exit 0,
as did pinned v6.1 REP build, layout validation and esptool image validation.
Final candidate 0.2.5 is 681,664 bytes; app SHA-256
`390fb8e02a1ce0f5af9668075211c2d7cd3673f6e64e6441f4f1333a16110277`;
full embedded ELF
`1fcfa1092406928118bbe28971415fca99419770eb47ca2a71f7b05a99e754c7`.
This correction justifies one additional A-only OTA; no duplicate full-Flash
preflight/readback or five-minute endurance-style repeat is needed for the
unchanged measurement arithmetic. The final deployment's short startup/normal
observations are recorded separately below.

**Verified final physical deployment:** runtime/USB identity and both VALID
images were checked again; the previously physically verified table/bootloader
were not reread. A 0.2.4 entered explicit Safe Mode solely to use guarded
selection of exact B 0.2.3. The internal recovery latch was cleared, B was
selected through the existing validated API and reconfirmed PENDING_VERIFY →
VALID in normal mode. B application bytes were not rewritten. From B, a
second genuine RO1 upload installed 0.2.5 only in inactive A; READY/full-image
digest/ELF, activation, restart, core self-test and A PENDING_VERIFY → VALID
all passed. Transfer through activation took 70.301 host-monotonic seconds.
No rollback fault injection was repeated. This was the only extra OTA and
was for the concrete ownership defect, not an added log statement.

A four-second, explicitly bounded early-startup query sequence retained
three pre-driver response timeouts, then **six pre-publication replies** at
uptimes 0.492–1.052 s with valid=0, usable=0, sync=0/at=0. These are deliberate
unusable samples, not I²C failures or fabricated valid time. Sixteen later
replies were valid/OS=0, sync=1, last-sync uptime 1.115373 s. The first valid
sample at uptime 1.164139 s enclosed RTC−system in
**[−0.054433, +0.947612] s**, wholly within the unchanged one-second criterion.
The worker serialized these requests; no permanent polling was added.

Two normal-state DIAG samples at uptime 15.154 and 45.174 s gave respectively
**[−0.044277, +0.957798] s** and **[−0.064413, +0.937686] s**, read durations
2144/2170 µs and USB RTT 10.522/10.434 ms. Both were valid/OS=0, sync=1 with
unchanged last-sync timestamp; their rate interval also includes zero. The
final independent RT1 had equal RTC/system seconds, error=0 and 9.839 ms RTT.
This short check validates startup/normal diagnostics on the corrected build;
the five-minute evidence remains the explicitly identified 0.2.4 series.
Neither establishes many-hour agreement.

**Final state:** A **0.2.5 VALID/running**, B **0.2.3 VALID/fallback** with its
original full ELF identity, verified compatible and rollback eligible.
BootGuard normal/STABLE, safe=0, latch/failures/trial/test_build/fault_remaining=0.
NVS ESP_OK/no erase fallback, unchanged LittleFS mount/capacity, valid RTC/OS=0,
gauge and GUI/core DMA services passed again. No RTC register SET, calibration,
data erase, bootloader/table write or additional ROM/raw Flash operation was
performed during this correction. Clock discipline remains disabled and the
historical discrepancy remains **Case D / unresolved**, not fixed.

Additional evidence in the same external directory: `guard_once.py`,
`guard-plan-once.json`, `guard-safe-*.json`, `guard-confirmed-b-boot.log`,
`guard-live-preflight.json`, `guard-ready.json`, `guard-activation.json`,
`guard-restart-ack.json`, `guard-ota-timing.json`, `guard-ota-protocol.log`,
`guard-early-boot.log`, `guard-early-samples.json`, `guard-normal-a-boot.log`,
`guard-postboot.json`, `guard-final-device-state.json`, `guard-install.log`
and `guard-image-info.log`; final build/test/layout logs are
`/tmp/roundie-rtc-guard-{build,tests,layout}.log`. No generated evidence or
device identifiers are committed. Stop here; no subsequent milestone begins.

### Diagnosis boundary and decision

| Hypothesis | Evidence and remaining limit |
|---|---|
| Independent oscillator frequencies | Source establishes independent clocks; a short second-resolution series cannot identify ordinary ppm disagreement. |
| Quantization / fractional phase / rounding | Both legacy values floor seconds; startup explicitly sets fraction zero. This explains substantial apparent offset but not the entire historical −2 s under a short, uninterrupted sample. |
| Non-atomic sampling / worker or I²C latency | DIAG encloses the full read. Current approximately 2 ms reads are far below one second; historical USB RTT was under 13 ms. Earlier, unrecorded stalls remain unknown. |
| UTC conversion / calendar coherence | Existing production codec passes supported-date tests, uses no local timezone conversion, and driver follows NXP's single calendar burst. No demonstrated bug. |
| ESP-IDF time source | Awake wall clock follows high-resolution SYSTIMER, not continuous internal RC frequency. No application sleep path or added slew. |
| Initial alignment defect | Whole-second initialization leaves phase unresolved; current positive offset is compatible with that design. No demonstrated extra startup error requiring a correction. |
| Unexpected later clock setting | Existing RTC recovery and explicit host SET are real paths. New boot-local counter detects them; the historical record had no counter. |

**Architectural decision for this investigation:** retain the existing clock
hierarchy and startup behavior. A valid external RTC persists UTC; system
wall time serves firmware; monotonic time controls elapsed durations. Do not
introduce periodic `settimeofday()`, `adjtime()`, calibration or clock-to-RTC
writeback without demonstrated drift and an agreed authority/accuracy basis.
No evidence makes the external oscillator automatically more accurate.

**Pending targeted experiment:** retain a same-boot DIAG baseline and later
read after ordinary many-hour uptime, without SET/restart, preserving sync
count, last-sync timestamp and both clock brackets. Acceptance for a relative
trend is a consistent, nonzero rate interval that excludes zero and no
reference discontinuity; failure/invalid samples must remain visible. Ordinary
tens-of-ppm rates require many hours with the present one-second resolution,
or a separately designed bounded second-edge measurement. No such extended
run, high-frequency background task or new hardware experiment is performed
by this goal. Absolute UTC accuracy additionally needs an assessed reference.
