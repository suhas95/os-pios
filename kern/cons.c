/*
 * Main console driver for PIOS, which manages lower-level console devices
 * such as video (dev/video.*), keyboard (dev/kbd.*), and serial (dev/serial.*)
 *
 * Copyright (c) 2010 Yale University.
 * Copyright (c) 1993, 1994, 1995 Charles Hannum.
 * Copyright (c) 1990 The Regents of the University of California.
 * See section "BSD License" in the file LICENSES for licensing terms.
 *
 * This code is derived from the NetBSD pcons driver, and in turn derived
 * from software contributed to Berkeley by William Jolitz and Don Ahn.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/stdio.h>
#include <inc/stdarg.h>
#include <inc/x86.h>
#include <inc/string.h>
#include <inc/assert.h>
#if LAB >= 2
#include <inc/syscall.h>
#endif

#include <kern/cpu.h>
#include <kern/cons.h>
#include <kern/mem.h>
#if LAB >= 2
#include <kern/spinlock.h>
#endif
#if LAB >= 4
#include <kern/file.h>
#endif

#include <dev/video.h>
#include <dev/kbd.h>
#include <dev/serial.h>

void cons_intr(int (*proc)(void));
static void cons_putc(int c);

#if LAB >= 2
spinlock cons_lock;	// Spinlock to make console output atomic
#endif

/***** General device-independent console code *****/
// Here we manage the console input buffer,
// where we stash characters received from the keyboard or serial port
// whenever the corresponding interrupt occurs.

#define CONSBUFSIZE 512

static struct {
	uint8_t buf[CONSBUFSIZE];
	uint32_t rpos;
	uint32_t wpos;
} cons;

#if SOL >= 4
static int cons_outsize;	// Console output already written by root proc
#endif

// called by device interrupt routines to feed input characters
// into the circular console input buffer.
void
cons_intr(int (*proc)(void))
{
	int c;

#if LAB >= 2
	spinlock_acquire(&cons_lock);
#endif
	while ((c = (*proc)()) != -1) {
		if (c == 0)
			continue;
		cons.buf[cons.wpos++] = c;
		if (cons.wpos == CONSBUFSIZE)
			cons.wpos = 0;
	}
#if LAB >= 2
	spinlock_release(&cons_lock);

#if LAB >= 4
	// Wake the root process
	file_wakeroot();
#endif
#endif
}

// return the next input character from the console, or 0 if none waiting
int
cons_getc(void)
{
	int c;

	// poll for any pending input characters,
	// so that this function works even when interrupts are disabled
	// (e.g., when called from the kernel monitor).
	serial_intr();
	kbd_intr();

	// grab the next character from the input buffer.
	if (cons.rpos != cons.wpos) {
		c = cons.buf[cons.rpos++];
		if (cons.rpos == CONSBUFSIZE)
			cons.rpos = 0;
		return c;
	}
	return 0;
}

// output a character to the console
static void
cons_putc(int c)
{
	serial_putc(c);
	video_putc(c);
}

// initialize the console devices
void
cons_init(void)
{
	if (!cpu_onboot())	// only do once, on the boot CPU
		return;

#if LAB >= 2
	spinlock_init(&cons_lock);
#endif
	video_init();
	kbd_init();
	serial_init();

	if (!serial_exists)
		warn("Serial port does not exist!\n");
}

#if LAB >= 4
// Enable console interrupts.
void
cons_intenable(void)
{
	if (!cpu_onboot())	// only do once, on the boot CPU
		return;

	kbd_intenable();
	serial_intenable();
}
#endif // LAB >= 4

// `High'-level console I/O.  Used by readline and cprintf.
void
cputs(const char *str)
{
#if LAB >= 2
	if (read_cs() & 3)
		return sys_cputs(str);	// use syscall from user mode

	// Hold the console spinlock while printing the entire string,
	// so that the output of different cputs calls won't get mixed.
	// Implement ad hoc recursive locking for debugging convenience.
	bool already = spinlock_holding(&cons_lock);
	if (!already)
		spinlock_acquire(&cons_lock);

#endif
	char ch;
	while (*str)
		cons_putc(*str++);
#if LAB >= 2

	if (!already)
		spinlock_release(&cons_lock);
#endif
}

#if LAB >= 4
// Synchronize the root process's console special files
// with the actual console I/O device.
bool
cons_io(void)
{
#if SOL >= 4
	spinlock_acquire(&cons_lock);
	bool didio = 0;

	// Console output from the root process's console output file
	fileinode *outfi = &files->fi[FILEINO_CONSOUT];
	const char *outbuf = FILEDATA(FILEINO_CONSOUT);
	assert(cons_outsize <= outfi->size);
	while (cons_outsize < outfi->size) {
		cons_putc(outbuf[cons_outsize++]);
		didio = 1;
	}

	// Console input to the root process's console input file
	fileinode *infi = &files->fi[FILEINO_CONSIN];
	char *inbuf = FILEDATA(FILEINO_CONSIN);
	int amount = cons.wpos - cons.rpos;
	if (infi->size + amount > FILE_MAXSIZE)
		panic("cons_io: root process's console input file full!");
	assert(amount >= 0 && amount <= CONSBUFSIZE);
	if (amount > 0) {
		memmove(&inbuf[infi->size], &cons.buf[cons.rpos], amount);
		infi->size += amount;
		cons.rpos = cons.wpos = 0;
		didio = 1;
	}

	spinlock_release(&cons_lock);
	return didio;
#else
	// Lab 4: your console I/O code here.
	warn("cons_io() not implemented");
	return 0;	// 0 indicates no I/O done
#endif	// SOL >= 4
}
#endif	// LAB >= 4

