# esp-idf-lib/button

[![Build examples](https://github.com/esp-idf-lib/button/actions/workflows/build.yml/badge.svg)](https://github.com/esp-idf-lib/button/actions/workflows/build.yml)
[![Build docs](https://github.com/esp-idf-lib/button/actions/workflows/build-docs.yml/badge.svg)](https://github.com/esp-idf-lib/button/actions/workflows/build-docs.yml)
[![Validation](https://github.com/esp-idf-lib/button/actions/workflows/validate-component.yml/badge.svg)](https://github.com/esp-idf-lib/button/actions/workflows/validate-component.yml)

HW timer-based driver for GPIO buttons.

The driver debounces the contact, detects short and long presses and can repeat
a click while the button is held. Every button is an opaque handle with its own
timer, so several buttons work independently.

* [Documentation](https://esp-idf-lib.github.io/button/)
* [Repository](https://github.com/esp-idf-lib/button)
* [Issues](https://github.com/esp-idf-lib/button/issues)
* [Discussions and questions](https://github.com/esp-idf-lib/core/discussions)
* [Component page at the ESP Component Registry](https://components.espressif.com/components/esp-idf-lib/button)

## Features

- Any number of buttons, each with its own handle and timer.
- Debouncing: the samples taken right after a press are ignored
  (`dead_time_us`).
- Short click, long press and autorepeat events.
- Two detection modes: a periodic timer, or an interrupt that starts the timer.
- Internal pull-up or pull-down, or an external resistor.

## Events

| Event | When |
|-------|------|
| `BUTTON_PRESSED` | The button is pressed |
| `BUTTON_RELEASED` | The button is released |
| `BUTTON_CLICKED` | A short press ends, or an autorepeat step |
| `BUTTON_PRESSED_LONG` | The button is held longer than `long_press_time_us` |

Without autorepeat a short press produces `PRESSED`, `RELEASED`, `CLICKED`, and
a long press produces `PRESSED`, `PRESSED_LONG`, `RELEASED`. With autorepeat the
long press is disabled and holding the button produces `PRESSED`, a series of
`CLICKED`, then `RELEASED`.

## Modes

`BUTTON_MODE_POLL` starts a periodic timer and reads the pin every
`poll_interval_us` microseconds. It needs no interrupt service and works with
any pin, but it wakes the CPU on every period even when nobody touches the
button.

`BUTTON_MODE_INTERRUPT` waits for an edge on the pin; the interrupt then disables
itself and starts the same timer, which confirms the press and follows it until
the release. The interrupt service must be installed with
`gpio_install_isr_service()` before the first such button is created. A button
that is already held at boot is not detected until it is released and pressed
again.

A pin can serve one button at a time, in either mode: the GPIO driver keeps a
single interrupt handler per pin, so a second button on the same pin would take
the handler over and the first one would stop seeing presses. The second
`button_create()` fails with `ESP_ERR_INVALID_STATE`, and the pin becomes free
again after `button_delete()`.

## Configuration

The defaults come from `menuconfig` (`Component config` → `Button`) and are
available through `BUTTON_DEFAULT_CONFIG()`:

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_BUTTON_POLL_TIMEOUT` | 10 ms | Timer period and the unit of all timings |
| `CONFIG_BUTTON_DEAD_TIME` | 50 ms | Samples ignored right after a press |
| `CONFIG_BUTTON_LONG_PRESS_TIMEOUT` | 1000 ms | Long press threshold |
| `CONFIG_BUTTON_AUTOREPEAT_TIMEOUT` | 500 ms | Delay before autorepeat starts |
| `CONFIG_BUTTON_AUTOREPEAT_INTERVAL` | 250 ms | Autorepeat step |

The configuration is validated by `button_create()`: the GPIO must exist, the
mode must be known, the pressed level must be 0 or 1, the poll interval must be
non-zero and, when autorepeat is enabled, the repeat interval must be non-zero.

The event callback runs in the ESP timer task while the button mutex is held, so
it must be short and must not call the driver back: deleting the button from the
callback deadlocks.

## Wiring

A button connected between the GPIO and ground uses `pressed_level = 0` with the
internal pull-up, or an external 4.7-10 kOhm resistor to 3.3 V. A button
connected to 3.3 V uses `pressed_level = 1` with the internal pull-down.

## Example

See [examples/default](examples/default). It creates a button in interrupt mode
with a long press and a button in poll mode with autorepeat, and prints every
event.

## Installation

```sh
idf.py add-dependency esp-idf-lib/button
```

## Support

For questions and discussions about the component, please use
[Discussions](https://github.com/esp-idf-lib/core/discussions)
at [esp-idf-lib/core](https://github.com/esp-idf-lib/core).

## Contributing

Please read [CONTRIBUTING.md](https://github.com/esp-idf-lib/core/blob/main/CONTRIBUTING.md)
at [esp-idf-lib/core](https://github.com/esp-idf-lib/core).
