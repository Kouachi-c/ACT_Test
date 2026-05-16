# UART Demo — Linux termios Interface

A C program that configures and communicates over a serial (UART) port on Linux
using the `termios` API. It opens a device, applies user-defined line settings,
transmits a test message, and reads back incoming data with a `select()`-based
timeout.

---

## Files

| File | Description |
|---|---|
| `uart_demo.c` | Main program |
| `Makefile` | Build rule |

---

## Requirements

- Linux (kernel 3.x+)
- GCC or any C11-compatible compiler
- A serial device: `/dev/ttyS0`, `/dev/ttyUSB0`, `/dev/ttyACM0`, etc.

---

## Build

```bash
make
```

Or manually:

```bash
gcc -Wall -Wextra -std=c11 -O2 -o uart_demo uart_demo.c
```

---

## Run

```bash
# Default device (/dev/ttyS0)
sudo ./uart_demo

# Specify a device
sudo ./uart_demo /dev/ttyUSB0
```

> **Permission denied?**  Add your user to the `dialout` group so `sudo` is not
> required:
> ```bash
> sudo usermod -aG dialout $USER
> # Log out and back in, then:
> ./uart_demo /dev/ttyUSB0
> ```

---

## Configuration

All parameters are compile-time constants at the top of `uart_demo.c`:

| Constant | Default | Options |
|---|---|---|
| `DEFAULT_DEVICE` | `/dev/ttyS0` | Any device path |
| `BAUD_RATE` | `B115200` | Any termios `Bxxx` constant |
| `DATA_BITS` | `8` | `5`, `6`, `7`, `8` |
| `PARITY` | `0` (none) | `0` = none, `1` = odd, `2` = even |
| `STOP_BITS` | `1` | `1`, `2` |
| `READ_TIMEOUT_SEC` | `5` | Seconds before giving up on RX |
| `TEST_MESSAGE` | `"Hello from UART Demo!\r\n"` | Any string |

---

## Loopback Test (no external device needed)

Short the **TX** pin to the **RX** pin on a USB-serial adapter, then run the
program. Everything transmitted will be echoed straight back.

```bash
# Terminal 1 — monitor the port with minicom
minicom -D /dev/ttyUSB0 -b 115200

# Terminal 2 — run the demo
sudo ./uart_demo /dev/ttyUSB0
```

Expected output:

```
[uart] Opening device : /dev/ttyUSB0
[uart] Config         : 115200 baud | 8 data bits | none parity | 1 stop bit(s)
[uart] TX → "Hello from UART Demo!"
[uart] Sent 23 byte(s).
[uart] Waiting up to 5 s for incoming data…
[uart] RX (23 byte(s)) → "Hello from UART Demo!
"
[uart] Done.
```

---

## Error Handling

| Scenario | Behaviour |
|---|---|
| Invalid / missing device path | `open()` fails → prints `errno` message and exits |
| Permission denied | Same as above — run with `sudo` or fix group membership |
| `tcsetattr` failure | Prints error, restores original settings, exits |
| Write failure | Prints error, restores original settings, exits |
| No data within timeout | Prints timeout notice, exits cleanly |
| Read failure | Prints error, restores original settings, exits |

In all failure paths the original `termios` settings are restored before the
process exits, leaving the port in the same state it was found.

---

## Implementation Notes

- **`select()` for RX timeout** — more portable and flexible than `VTIME`; cleanly
  distinguishes between "nothing arrived" and a real read error.
- **Zeroed `termios` struct** — built from scratch in `uart_config()` so no stale
  flags from a prior session can bleed in.
- **`tcdrain()`** — called after `write()` to block until the hardware FIFO has
  shifted out all bytes before the program moves on.
- **`O_NOCTTY`** — prevents the port from becoming the process's controlling
  terminal, avoiding unwanted `SIGHUP` delivery.
