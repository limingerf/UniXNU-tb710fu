/* SPDX-License-Identifier: MIT
 * Actual _cnputc/_cnputs/console_write/console_write_unbuffered bodies are
 * extracted from the fully patched native source into production-console.inc.
 * Stub console services record capture and hardware delivery independently. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define XNU_TB710FU_LIVE_LOG 1
#define __improbable(value) (value)
#define CPU_CONS_BUF_SIZE 256
#define SERIAL_CONS_OPS 0
#define VC_CONS_OPS 1
static int disableConsoleOutput, kernel_debugger_entry_count;
static bool console_mirror, console_suspended;
static unsigned active = 1, initialized, ramoops_count, cdc_count, hardware_count, xnu_count;
static char ramoops_bytes[1024], cdc_bytes[1024], hardware_bytes[1024], xnu_bytes[1024], ring[1024];
static bool route_via_pe;
void serial_putc_options(char c, bool poll);
static char *ring_write;
static unsigned ring_used;
static void hardware_putc(char c, bool poll)
{
    (void)poll;
    if (route_via_pe) serial_putc_options(c, poll);
    else hardware_bytes[hardware_count++] = c;
}
static void uart_putc(char c) { hardware_bytes[hardware_count++] = c; }
static void uart_putc_options(char c, bool poll) { (void)poll; uart_putc(c); }
static struct { void (*putc)(char, bool); } cons_ops[] = {{hardware_putc}, {hardware_putc}};
static unsigned get_cons_ops_index(void) { return VC_CONS_OPS; }
static int boot_progress_active(void) { return 0; }
static void boot_progress_putc(char c) { (void)c; assert(0); }
static int tb710fu_live_log_active(void) { return (int)active; }
static void tb710fu_ramoops_putc(char c) { ramoops_bytes[ramoops_count++] = c; }
static void tb710fu_live_log_putc(char c) { xnu_bytes[xnu_count++] = c; }
void tb710fu_cdc_log_tap_putc(char c) { cdc_bytes[cdc_count++] = c; }
static int is_console_initialized(void) { return (int)initialized; }
static int pmap_in_ppl(void) { return 0; }
static int console_io_allowed(void) { return 1; }
static int get_preemption_level(void) { return 0; }
static char *console_ring_reserve_space(int size)
{
    assert(size > 0 && ring_used + (unsigned)size < sizeof(ring));
    ring_write = ring + ring_used;
    ring_used += (unsigned)size;
    return ring_write;
}
static void console_ring_put(char **at, char c) { *(*at)++ = c; }
static void console_ring_unreserve_space(void) { }
static void console_ring_try_empty(bool);
#include "production-console.inc"
static void console_ring_try_empty(bool ignored)
{
    (void)ignored;
    _cnputs(ring, (int)ring_used, false);
    ring_used = 0;
}
static void reset(unsigned disabled, unsigned live_active, unsigned buffered)
{
    disableConsoleOutput = (int)disabled;
    active = live_active;
    initialized = buffered;
    ramoops_count = cdc_count = hardware_count = xnu_count = ring_used = 0;
    route_via_pe = false;
}
static void verify(const char *xnu, const char *ramoops, const char *cdc, const char *hardware)
{
    assert(xnu_count == strlen(xnu));
    assert(ramoops_count == strlen(ramoops));
    assert(cdc_count == strlen(cdc));
    assert(hardware_count == strlen(hardware));
    assert(memcmp(ramoops_bytes, ramoops, ramoops_count) == 0);
    assert(memcmp(cdc_bytes, cdc, cdc_count) == 0);
    assert(memcmp(hardware_bytes, hardware, hardware_count) == 0);
    assert(memcmp(xnu_bytes, xnu, xnu_count) == 0);
}
int main(void)
{
    for (unsigned buffered = 0; buffered < 2; ++buffered) {
        reset(1, 1, buffered);
        console_write("A\r\nB", 4);
        verify("A\nB", "A\nB", "A\r\nB", "");
        reset(1, 0, buffered);
        console_write("A\r\nB", 4);
        verify("A\nB", "", "A\r\nB", "");
        reset(0, 1, buffered);
        console_write("A\nB", 3);
        verify("A\nB", "A\nB", "A\r\nB", "A\r\nB");
        reset(0, 1, buffered);
        route_via_pe = true;
        console_write("A\nB", 3);
        verify("A\nB", "A\nB", "A\r\nB", "A\r\nB");
        reset(0, 0, buffered);
        console_write("A\nB", 3);
        verify("A\nB", "A\nB", "A\r\nB", "A\r\nB");
    }
    reset(1, 1, 0);
    console_write_unbuffered('X');
    console_write_unbuffered('\r');
    console_write_unbuffered('\n');
    verify("X\n", "X\n", "X\r\n", "");
    reset(0, 1, 0);
    console_write_unbuffered('X');
    console_write_unbuffered('\n');
    verify("X\n", "X\n", "X\r\n", "X\r\n");
    reset(1, 1, 0);
    _cnputs("raw\n", 4, true);
    verify("raw\n", "raw\n", "raw\n", "");
    reset(1, 0, 0);
    console_write_unbuffered('X');
    _cnputs("raw\n", 4, true);
    verify("Xraw\n", "", "Xraw\n", "");
    reset(0, 0, 0);
    serial_putc('P'); serial_putc('\r'); serial_putc('\n');
    verify("P\n", "P\n", "P\r\n", "P\r\n");
    reset(0, 0, 0);
    serial_putc_crlf('P'); serial_putc_crlf('\n');
    verify("P\n", "P\n", "P\n", "P\r\n");
    reset(0, 1, 0);
    serial_putc_options('Q', true);
    verify("", "", "", "Q");
    reset(1, 1, 0);
    tb710fu_live_capture_disabled(NULL, 10);
    verify("", "", "", "");
    puts("PASS: 18 production serial/PE capture, preinit and no-duplicate cases");
    return 0;
}
