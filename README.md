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
- `libutil` and `libpthread` (standard on all major distros)
- A serial device for real-hardware mode: `/dev/ttyUSB0`, `/dev/ttyACM0`, etc.

---

## Build

```bash
make clean && make
```

Or manually:

```bash
gcc -Wall -Wextra -std=c11 -O2 -o uart_demo uart_demo.c -lutil -lpthread
```

---

## Run

### Software loopback (no hardware required)

```bash
./uart_demo -l
```

Creates a virtual PTY pair and runs an echo thread internally — no serial
adapter or physical wiring needed.

Expected output:

```
[uart] Opening device : loopback (virtual PTY)
[uart] Loopback mode  : echo thread active
[uart] Config         : 115200 baud | 8 data bits | none parity | 1 stop bit(s)
[uart] TX → "Hello from UART Demo!"
[uart] Sent 23 byte(s).
[uart] Waiting up to 5 s for incoming data…
[uart] RX (23 byte(s)) → "Hello from UART Demo!"
[uart] Done.
```

### Real serial device

```bash
# Default device (/dev/ttyUSB0)
./uart_demo

# Specify a device explicitly
./uart_demo /dev/ttyACM0
```

> **Permission denied?**  Add your user to the `dialout` group so `sudo` is not
> required:
> ```bash
> sudo usermod -aG dialout $USER
> newgrp dialout          # apply immediately without logging out
> ./uart_demo /dev/ttyUSB0
> ```

### Hardware loopback (TX shorted to RX)

Short the **TX** pin to the **RX** pin on a USB-serial adapter, then run
normally. Every byte transmitted will be echoed straight back by the wire.

---

## Configuration

All parameters are compile-time constants at the top of `uart_demo.c`:

| Constant | Default | Options |
|---|---|---|
| `DEFAULT_DEVICE` | `/dev/ttyUSB0` | Any device path |
| `BAUD_RATE` | `B115200` | Any termios `Bxxx` constant |
| `DATA_BITS` | `8` | `5`, `6`, `7`, `8` |
| `PARITY` | `0` (none) | `0` = none, `1` = odd, `2` = even |
| `STOP_BITS` | `1` | `1`, `2` |
| `READ_TIMEOUT_SEC` | `5` | Seconds before giving up on RX |
| `TEST_MESSAGE` | `"Hello from UART Demo!\r\n"` | Any string |

---

## Error Handling

| Scenario | Behaviour |
|---|---|
| Device not found | `open()` prints path and suggests using `-l` or plugging in an adapter |
| Permission denied (`EACCES`) | Prints the exact `usermod` command to fix group membership |
| No hardware at port (`EIO`) | `tcgetattr` explains the cause and suggests `/dev/ttyUSB0` or `-l` |
| `tcsetattr` failure | Prints error, restores original settings, exits |
| Write failure | Prints error, restores original settings, exits |
| No data within timeout | Prints timeout notice, exits cleanly |
| Read failure | Prints error, restores original settings, exits |

In all failure paths the original `termios` settings are restored before the
process exits, leaving the port in the same state it was found.

---

## Implementation Notes

- **`-l` loopback mode** — uses `openpty()` to create a master/slave PTY pair;
  a `pthread` echo thread reads from the master and writes it back, so the slave
  side sees its own transmission without any external wiring.
- **`select()` for RX timeout** — more portable and flexible than `VTIME`; cleanly
  distinguishes between "nothing arrived" and a real read error.
- **Zeroed `termios` struct** — built from scratch in `uart_config()` so no stale
  flags from a prior session can bleed in.
- **`tcdrain()`** — called after `write()` to block until the hardware FIFO has
  shifted out all bytes before the program moves on.
- **`O_NOCTTY`** — prevents the port from becoming the process's controlling
  terminal, avoiding unwanted `SIGHUP` delivery.
