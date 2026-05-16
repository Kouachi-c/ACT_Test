/*=========================================================================
 * @author : K Corneille EKON (a.k.a ekon_ihc)
 * @date   : 05/16/2026
 * @brief  : UART demo using Linux termios API.
 *
 * Configures a serial port, sends a test message, then listens for incoming
 * data using a select()-based timeout so the process never blocks indefinitely.
 *
 * Usage:
 *   ./uart_demo [device]   — open a real serial port (e.g. /dev/ttyUSB0)
 *   ./uart_demo -l         — loopback mode: virtual PTY pair, no hardware needed
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>

/*  Compile-time configuration  */

#define DEFAULT_DEVICE    "/dev/ttyUSB0" /* typical USB-serial adapter        */
#define BAUD_RATE         B115200        /* termios baud constant             */
#define DATA_BITS         8              /* character width: 5, 6, 7, or 8   */
#define PARITY            0              /* 0 = none, 1 = odd, 2 = even      */
#define STOP_BITS         1              /* 1 or 2                            */
#define READ_TIMEOUT_SEC  5              /* how long to wait for RX data      */
#define RX_BUF_SIZE       256
#define TEST_MESSAGE      "Hello from UART Demo!\r\n"

/*  UART context  */

/*
 * Bundling the fd and the saved termios together makes it easy to
 * restore the port's original settings no matter how we exit.
 */
typedef struct {
    int            fd;
    struct termios saved;   /* snapshot of termios before we touched it    */
} uart_ctx_t;

/*  Loopback echo thread  */

typedef struct {
    int          master_fd;
    _Atomic int  stop;
} echo_args_t;

/*  Forward declarations  */

static int  uart_open          (uart_ctx_t *ctx, const char *dev);
static int  uart_open_loopback (uart_ctx_t *ctx, int *master_out);
static int  uart_config        (uart_ctx_t *ctx, speed_t baud,
                                int data_bits, int parity, int stop_bits);
static int  uart_write         (uart_ctx_t *ctx, const char *buf, size_t len);
static int  uart_read          (uart_ctx_t *ctx, char *buf, size_t max,
                                int timeout_sec);
static void uart_close         (uart_ctx_t *ctx);
static void *echo_loop         (void *arg);

/* -------------------------------------------------------------------------
 * echo_loop
 *
 * Runs in a background thread during loopback mode.  Reads whatever the
 * UART write path sends to the PTY master and writes it straight back, so
 * the slave side sees its own transmission — a perfect loopback.
 * ------------------------------------------------------------------------- */
static void *echo_loop(void *arg)
{
    echo_args_t   *a = arg;
    char           buf[RX_BUF_SIZE];
    struct timeval tv;
    fd_set         rfds;

    while (!a->stop) {
        FD_ZERO(&rfds);
        FD_SET(a->master_fd, &rfds);
        tv = (struct timeval){ .tv_sec = 0, .tv_usec = 50000 }; /* 50 ms poll */

        if (select(a->master_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            ssize_t n = read(a->master_fd, buf, sizeof buf);
            if (n > 0 && write(a->master_fd, buf, n) < 0)
                break; /* slave was closed — stop echoing */
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * uart_open
 *
 * Opens the serial device and saves its current termios settings so they
 * can be restored when we close the port.
 *
 * Flags used:
 *   O_RDWR    — we need both transmit and receive
 *   O_NOCTTY  — don't make this our controlling terminal (avoids signals
 *               like SIGHUP if the modem hangs up)
 *   O_NDELAY  — don't block waiting for DCD (carrier detect)
 *
 * Returns 0 on success, -1 on error.
 * ------------------------------------------------------------------------- */
static int uart_open(uart_ctx_t *ctx, const char *dev)
{
    ctx->fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
    if (ctx->fd < 0) {
        if (errno == EACCES)
            fprintf(stderr,
                "[uart] open(\"%s\"): permission denied.\n"
                "       Fix: sudo usermod -aG dialout $USER && newgrp dialout\n",
                dev);
        else if (errno == ENOENT)
            fprintf(stderr,
                "[uart] open(\"%s\"): device not found.\n"
                "       Fix: plug in a USB-serial adapter or run with -l for loopback.\n",
                dev);
        else
            fprintf(stderr, "[uart] open(\"%s\"): %s\n", dev, strerror(errno));
        return -1;
    }

    /* Snapshot current settings — restored verbatim in uart_close(). */
    if (tcgetattr(ctx->fd, &ctx->saved) != 0) {
        if (errno == EIO)
            fprintf(stderr,
                "[uart] tcgetattr: no UART hardware at \"%s\" (EIO).\n"
                "       The device node exists but no physical port is present.\n"
                "       Fix: use /dev/ttyUSB0 (USB-serial adapter) or run with -l.\n",
                dev);
        else
            fprintf(stderr, "[uart] tcgetattr: %s\n", strerror(errno));
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    /*
     * O_NDELAY made open() non-blocking; switch the fd back to blocking
     * mode so that write() and read() behave predictably.  We use select()
     * for the receive timeout instead of relying on O_NONBLOCK semantics.
     */
    int flags = fcntl(ctx->fd, F_GETFL, 0);
    if (flags == -1 || fcntl(ctx->fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        fprintf(stderr, "[uart] fcntl: %s\n", strerror(errno));
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * uart_open_loopback
 *
 * Creates a pseudo-terminal pair (master + slave).  The slave is used as
 * the UART fd in ctx; the caller runs echo_loop() on master so every byte
 * written to the slave comes back to it — no real hardware required.
 *
 * Returns 0 on success, -1 on error.  *master_out is set on success and
 * must be closed by the caller after the echo thread has been stopped.
 * ------------------------------------------------------------------------- */
static int uart_open_loopback(uart_ctx_t *ctx, int *master_out)
{
    int master, slave;

    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        fprintf(stderr, "[uart] openpty: %s\n", strerror(errno));
        return -1;
    }

    ctx->fd = slave;

    if (tcgetattr(slave, &ctx->saved) != 0) {
        fprintf(stderr, "[uart] tcgetattr (loopback): %s\n", strerror(errno));
        close(master);
        close(slave);
        ctx->fd = -1;
        return -1;
    }

    *master_out = master;
    return 0;
}

/* -------------------------------------------------------------------------
 * uart_config
 *
 * Applies baud rate, data-bits, parity, and stop-bits to the open port.
 *
 * We build a completely fresh termios struct (zeroed) rather than modifying
 * the saved one, so no stale flags from a previous session can bleed through.
 * This is equivalent to calling cfmakeraw() and then layering our settings
 * on top, but done field-by-field so each choice is explicit.
 *
 * Returns 0 on success, -1 on error.
 * ------------------------------------------------------------------------- */
static int uart_config(uart_ctx_t *ctx, speed_t baud,
                       int data_bits, int parity, int stop_bits)
{
    struct termios tty;
    memset(&tty, 0, sizeof tty);

    /*  Baud rate  */
    if (cfsetospeed(&tty, baud) != 0 || cfsetispeed(&tty, baud) != 0) {
        fprintf(stderr, "[uart] cfsetspeed: %s\n", strerror(errno));
        return -1;
    }

    /*  Character size (data bits)  */
    tty.c_cflag &= ~CSIZE;
    switch (data_bits) {
        case 5:  tty.c_cflag |= CS5; break;
        case 6:  tty.c_cflag |= CS6; break;
        case 7:  tty.c_cflag |= CS7; break;
        case 8:
        default: tty.c_cflag |= CS8; break;
    }

    /*  Parity  */
    switch (parity) {
        case 1:                             /* odd parity                   */
            tty.c_cflag |=  PARENB | PARODD;
            tty.c_iflag |=  INPCK;          /* enable input parity check    */
            break;
        case 2:                             /* even parity                  */
            tty.c_cflag |=  PARENB;
            tty.c_cflag &= ~PARODD;
            tty.c_iflag |=  INPCK;
            break;
        default:                            /* no parity (most common)      */
            tty.c_cflag &= ~PARENB;
            tty.c_iflag &= ~INPCK;
            break;
    }

    /*  Stop bits  */
    if (stop_bits == 2)
        tty.c_cflag |=  CSTOPB;
    else
        tty.c_cflag &= ~CSTOPB;

    /*   Control flags   */
    tty.c_cflag |= CREAD;       /* enable the receiver                      */
    tty.c_cflag |= CLOCAL;      /* ignore modem-control lines (no DCD)      */

    /*   Input flags: raw mode, no software flow control    */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);    /* disable XON/XOFF         */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK  /* no special break handling */
                   | ISTRIP | INLCR | IGNCR     /* no byte-stripping or      */
                   | ICRNL);                    /*   CR/LF translation       */

    /*   Output flags: no post-processing    */
    tty.c_oflag &= ~OPOST;      /* send bytes exactly as written            */

    /*   Local flags: no echo, no canonical mode, no signals     */
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /*
     * VMIN=0, VTIME=0: read() returns immediately with whatever bytes are
     * already in the kernel buffer (possibly zero).  The actual receive
     * timeout is implemented with select() in uart_read(), which is more
     * flexible and doesn't tie up the port.
     */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    /* Apply settings immediately (TCSANOW). */
    if (tcsetattr(ctx->fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[uart] tcsetattr: %s\n", strerror(errno));
        return -1;
    }

    /* Discard any stale bytes sitting in the hardware RX/TX FIFOs. */
    tcflush(ctx->fd, TCIOFLUSH);
    return 0;
}

/* -------------------------------------------------------------------------
 * uart_write
 *
 * Transmits exactly len bytes from buf, retrying on short writes (which can
 * happen if the kernel TX buffer is momentarily full) and on EINTR.
 * tcdrain() blocks until the hardware FIFO has shifted out all bytes.
 *
 * Returns the total bytes written, or -1 on a hard error.
 * ------------------------------------------------------------------------- */
static int uart_write(uart_ctx_t *ctx, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(ctx->fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;       /* interrupted by signal    */
            fprintf(stderr, "[uart] write: %s\n", strerror(errno));
            return -1;
        }
        sent += (size_t)n;
    }

    /* Wait until all bytes have physically left the UART shift register. */
    if (tcdrain(ctx->fd) != 0) {
        fprintf(stderr, "[uart] tcdrain: %s\n", strerror(errno));
        return -1;
    }

    return (int)sent;
}

/* -------------------------------------------------------------------------
 * uart_read
 *
 * Waits up to timeout_sec seconds for data to arrive, then reads up to
 * (max-1) bytes and NUL-terminates the result.
 *
 * select() is used instead of setting VTIME so we can distinguish between
 * "nothing arrived" (timeout → return 0) and a genuine read error (-1).
 *
 * Returns:
 *   >0  bytes received (buf is NUL-terminated)
 *    0  timeout — no data within timeout_sec
 *   -1  error
 * ------------------------------------------------------------------------- */
static int uart_read(uart_ctx_t *ctx, char *buf, size_t max, int timeout_sec)
{
    fd_set rfds;
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };

    FD_ZERO(&rfds);
    FD_SET(ctx->fd, &rfds);

    int ready = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
    if (ready < 0) {
        if (errno == EINTR) return 0;   /* interrupted by signal — not fatal */
        fprintf(stderr, "[uart] select: %s\n", strerror(errno));
        return -1;
    }
    if (ready == 0) {
        printf("[uart] Read timeout (%d s) — no data received.\n", timeout_sec);
        return 0;
    }

    ssize_t n = read(ctx->fd, buf, max - 1);
    if (n < 0) {
        fprintf(stderr, "[uart] read: %s\n", strerror(errno));
        return -1;
    }

    buf[n] = '\0';  /* NUL-terminate so callers can use printf("%s", buf)  */
    return (int)n;
}

/* -------------------------------------------------------------------------
 * uart_close
 *
 * Restores the port's original termios settings and closes the file
 * descriptor.  Safe to call even if uart_open() failed (fd == -1).
 * ------------------------------------------------------------------------- */
static void uart_close(uart_ctx_t *ctx)
{
    if (ctx->fd < 0) return;

    /*
     * Restore saved settings so the terminal/device is left in the same
     * state we found it — polite and avoids surprising other programs that
     * open the same port later.
     */
    if (tcsetattr(ctx->fd, TCSANOW, &ctx->saved) != 0)
        fprintf(stderr, "[uart] tcsetattr (restore): %s\n", strerror(errno));

    close(ctx->fd);
    ctx->fd = -1;
}

/* ==========================================================================
 * main
 * ========================================================================== */
int main(int argc, char *argv[])
{
    int         loopback = (argc > 1 && strcmp(argv[1], "-l") == 0);
    const char *device   = loopback          ? "loopback (virtual PTY)"
                         : (argc > 1)        ? argv[1]
                         :                     DEFAULT_DEVICE;

    uart_ctx_t  ctx    = { .fd = -1 };
    char        rx_buf[RX_BUF_SIZE];
    int         master_fd = -1;
    int         rc = 0;

    /*  Open  */
    printf("[uart] Opening device : %s\n", device);

    if (loopback) {
        if (uart_open_loopback(&ctx, &master_fd) != 0)
            return EXIT_FAILURE;
    } else {
        if (uart_open(&ctx, device) != 0)
            return EXIT_FAILURE;
    }

    /*  Start echo thread in loopback mode  */
    pthread_t   echo_tid;
    echo_args_t echo_args = { .master_fd = master_fd, .stop = 0 };

    if (loopback) {
        printf("[uart] Loopback mode  : echo thread active\n");
        pthread_create(&echo_tid, NULL, echo_loop, &echo_args);
    }

    /*  Configure  */
    printf("[uart] Config         : 115200 baud | %d data bits | %s parity | %d stop bit(s)\n",
           DATA_BITS,
           PARITY == 0 ? "none" : PARITY == 1 ? "odd" : "even",
           STOP_BITS);

    if (uart_config(&ctx, BAUD_RATE, DATA_BITS, PARITY, STOP_BITS) != 0)
        goto cleanup;

    /*  Transmit  */
    printf("[uart] TX → \"%.*s\"\n",
           (int)(strlen(TEST_MESSAGE) - 2), TEST_MESSAGE); /* strip trailing \r\n */

    rc = uart_write(&ctx, TEST_MESSAGE, strlen(TEST_MESSAGE));
    if (rc < 0) goto cleanup;
    printf("[uart] Sent %d byte(s).\n", rc);

    /*  Receive  */
    printf("[uart] Waiting up to %d s for incoming data…\n", READ_TIMEOUT_SEC);
    rc = uart_read(&ctx, rx_buf, sizeof rx_buf, READ_TIMEOUT_SEC);

    if (rc > 0)
        printf("[uart] RX (%d byte(s)) → \"%.*s\"\n",
               rc, rc - 1, rx_buf); /* strip trailing \r\n for display */
    else if (rc < 0)
        goto cleanup;
    /* rc == 0 means timeout; uart_read() already printed a message */

cleanup:
    uart_close(&ctx);

    if (loopback) {
        echo_args.stop = 1;
        pthread_join(echo_tid, NULL);
        close(master_fd);
    }

    printf("[uart] Done.\n");
    return (rc < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
