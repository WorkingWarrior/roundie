# RTC host UTC synchronization (Milestone 1)

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

## Test C — physical retention: PENDING

No cables or buttons were operated autonomously. Software restart retention
does **not** prove battery-only operation, hardware power-off retention, or
retention across loss of the RTC's supply. With a charged battery attached,
a future supervised test should:

1. Run `sync`, then `verify`; record host UTC, RTC UTC, OS and wall validity.
2. Disconnect USB while leaving the battery connected. Wait at least 60 s.
   Observe whether the diagnostic display continues on battery; record that
   behavior separately from RTC retention. Reconnect USB, run `verify` before
   any `sync`, and record continued UTC progression and OS.
3. Disconnect USB again. Hold PWR approximately two seconds to request the
   board's hardware power-off; observe actual shutdown. Wait at least 60 s,
   restore power with PWR, reconnect USB, and run `status`/`verify` **without
   resynchronizing first**. Record timestamps, elapsed host interval, OS,
   startup validity and restored system time. USB may keep some rails powered,
   so a USB-connected PWR test is not a substitute for this sequence.
4. Pass only if OS remains clear, RTC advances by elapsed host time within
   the documented three-second short-test tolerance, and boot restores wall
   time within one second of RTC. Record exact power state, durations and
   observations. If invalid or unavailable, preserve that evidence before
   issuing another explicit `sync`.

Battery removal, brownout, current draw, long-term drift, real I²C fault
injection and endurance remain separate pending experiments. Do not infer
their results from compilation or the software restart test.
