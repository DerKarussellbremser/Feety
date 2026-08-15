# Feety

A project to transmit data via radio from pressure sensors in the foot or in the sole
of a prosthesis to a vibration module on the body. This way, a person with a prosthesis
can know when their foot is properly placed on the ground.
Also thinkable for arm prosthesis with gloves or support from a manufacturer.

3D files for printing the cases and a detailed build plan for the hardware are still coming.

---

## Repository layout

| Path | What it is |
| --- | --- |
| `ESP32_Foot/` | Sensor unit in the prosthesis. Reads four pressure sensors and transmits them. |
| `ESP32_Hip/` | Feedback unit on the belt. Receives the readings and drives four vibration motors. |
| `legacy/` | The original Bluetooth Serial sketches, kept for reference. |

Each sketch folder is a self-contained Arduino sketch. `feety_config.h` holds everything
you would want to change for your own build; `feety_protocol.h` defines the radio packets
and **must stay identical in both folders** (see the note at the top of that file).

Tested with Arduino ESP32 core **2.0.17** and **3.3.11** — both compile without warnings.

## Hardware notes

Don't use cheap boards. On no-name ESP32 modules the readings were unusable as soon as
more than one sensor was in use. Boards with a proper regulator and decoupling fixed it
completely — the [uPesy](https://www.upesy.com/) boards were excellent here, and they
come with Li-Ion charging and battery operation already designed in.

**All sensor inputs must be on ADC1 (GPIO 32–39).** ESP-NOW runs on the WiFi radio, and
the WiFi driver takes exclusive ownership of ADC2 — `analogRead()` on GPIO 0, 2, 4,
12–15 or 25–27 returns garbage while the radio is up. Digital use of those pins (buttons,
PWM outputs) is fine; only analog reads are affected.

GPIO 34–39 are input-only and have no internal pull resistors, which is what you want for
a sensor divider: nothing loads it and shifts the reading.

### Pin assignment

**Foot unit**

| Function | GPIO | Note |
| --- | --- | --- |
| Sensors 1–4 | 32, 33, 34, 35 | ADC1 |
| Battery sense | 39 | ADC1, via divider |
| Status LED | 2 | on-board LED on most boards |
| Power button | 27 | active HIGH, RTC GPIO (wakes from deep sleep) |

**Hip unit**

| Function | GPIO | Note |
| --- | --- | --- |
| Motors 1–4 | 14, 27, 26, 25 | PWM out, 20 kHz |
| Battery sense | 35 | ADC1, via divider |
| Status LED | 32 | |
| Button | 4 | active HIGH, RTC GPIO (wakes from deep sleep) |

### Wiring

Everything on one unit shares a common ground. The two units share nothing — each has its
own board and its own cell.

**Pressure sensor → foot unit** (one per sensor, on GPIO 32, 33, 34, 35)

```
   3V3 ──[ FSR ]──┬── GPIO 32        FSR = pressure sensor in the sole
                  │
                [10k]                fixed resistor
                  │
                 GND
```

More pressure lowers the FSR's resistance and raises the voltage at the junction, so
higher pressure gives a higher reading — which is the direction the calibration expects.
Pick the fixed resistor near the FSR's resistance in the middle of your working range
(10k is a good starting point); it sets where the useful part of the curve sits. Don't add
your own pull resistor — the firmware deliberately leaves these pins unpulled so nothing
loads the divider.

**Vibration motor → hip unit** (one per motor, on GPIO 14, 27, 26, 25)

A motor must never hang directly on a GPIO. An ERM coin motor draws roughly 60–100 mA, an
ESP32 pin is rated for 20 mA. Each motor gets a logic-level N-channel MOSFET
(AO3400, 2N7002 for small motors, IRLZ44N if you like them chunky):

```
   VBAT ──┬──────────┐
          │          │
         [M]        [D]  flyback diode (1N4148 / SS14),
          │          │   cathode to VBAT
          └────┬─────┘
               │
               D
 GPIO ─[220R]─ G   MOSFET
               S
          ┌────┴────┐
        [10k]      GND
          │
         GND
```

Three parts that are not optional:

- The **flyback diode** across the motor. Without it the inductive kick when the PWM
  switches off will eventually kill the MOSFET, and in the meantime it puts noise on the
  supply that shows up as glitches everywhere else.
- The **10k gate pulldown**. GPIOs float during reset and boot, and GPIO 14 in particular
  pulses while the bootloader runs. Without the pulldown the motors twitch on every reset.
- **Motor power from the battery, not from the board's 3V3 pin.** Four motors at once are
  around 400 mA; that browns out the on-board regulator and resets the ESP32. This is
  worth checking first if a board seems to misbehave under load.

A 100 nF capacitor directly across the motor terminals suppresses brush noise and is
cheap insurance.

**Battery sense → both units** (GPIO 39 on the foot, GPIO 35 on the hip)

A full Li-Ion cell sits at 4.2 V and the ADC only takes 3.3 V, so it needs a divider:

```
   VBAT ──[ R1 ]──┬── GPIO 39 (foot) / 35 (hip)
                  │
                [ R2 ]
                  │
                 GND
```

Use something like R1 = 47k, R2 = 100k. Then set `FEETY_VBAT_DIVIDER = (R1 + R2) / R2` in
`feety_config.h` — 1.47 for those values. Measure the cell with a multimeter and compare
against the reported voltage (`raw` on the hip console shows the foot's), then trim the
factor until it matches. Resistor tolerances alone are worth a tenth of a volt here.

If your board already brings the battery out on a divided pin — several uPesy boards do —
use that pin instead and set the factor the board's documentation gives.

**Button → both units** (GPIO 27 on the foot, GPIO 4 on the hip)

```
   3V3 ──[ button ]── GPIO 4
```

That's all: the firmware enables the internal pulldown, and keeps it enabled through deep
sleep so the pin can't float and wake the board on its own. With long leads to the button,
add a 10k pulldown at the pin anyway.

**Status LED → both units** (GPIO 2 on the foot, GPIO 32 on the hip)

```
   GPIO 2 ──[330R]──▶|── GND
```

GPIO 2 is the on-board LED on most boards, so the foot unit usually needs nothing here.

## First run

1. Flash both sketches. Each unit prints its own MAC address over serial at 115200 baud.
2. Paste the hip's MAC into `ESP32_Foot/feety_config.h` (`FEETY_PEER_MAC`) and the foot's
   MAC into `ESP32_Hip/feety_config.h`, then reflash.

Out of the box both units use the broadcast address so a fresh pair talks immediately —
but broadcast means a second Feety in range would feed your hip unit too, and broadcast
frames cannot be encrypted. Set the real addresses before using the system for real.

`FEETY_WIFI_CHANNEL` must be the same on both units.

## Calibration

Open the serial monitor on the hip unit at 115200 baud and type `help`:

| Command | Effect |
| --- | --- |
| `show` | print the current calibration |
| `raw` | toggle a live stream of sensor values and motor duty — use this while stepping |
| `set <ch> <thr> <sat> <pmin> <pmax>` | set one channel |
| `save` | store in NVS (survives reboot and reflash) |
| `reset` | back to compiled-in defaults |
| `test <ch>` | run one motor for a second |

Per channel: below `thr` the motor stays off, at `sat` it runs at `pmax`, in between the
duty is interpolated from `pmin` to `pmax`. `pmin` is the lowest duty that still moves
your motor. This replaces the hardcoded offsets and step ladder of the original sketch —
no reflash needed to fit a new sole, and it is the same interface an app would later use.

## Operation

**Button (hip):** short press mutes/unmutes the vibration, holding it for 3 s puts both
units into deep sleep. Pressing it again wakes the hip unit up.

**Status LED:** steady = linked and active · slow blink = no data from the foot ·
fast blink = a battery is low · occasional flash = muted.

**Safety:** if no packet arrives for 300 ms the motors are switched off. Vibration is
never driven from stale readings — a wearer reading old data as ground contact is the
failure mode that matters here.

## Power

Both units check their cell continuously and shut down at 3.2 V to protect it. The foot
unit also goes to sleep on its own after 5 minutes without hearing from the hip, and
follows the hip into sleep when the hip's button is held. Adjust `FEETY_VBAT_DIVIDER` to
your own resistors — measure with a multimeter, the ADC cannot know your values.

## To do

- [x] tidy up the code
- [x] power saving
- [x] replace the delays with millis, so the main loop keeps running
- [x] stable radio link with automatic reconnect (moved from Bluetooth SPP to ESP-NOW:
      lower latency, no pairing, and it recovers on its own after a dropout)
- [ ] encrypted link (ESP-NOW supports it, but it needs real peer MACs instead of broadcast)
- [ ] phone app for the calibration, using the same commands as the serial console
- [ ] 3D printable cases
- [ ] documented hardware build
