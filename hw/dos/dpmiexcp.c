/* dos.c
 *
 * Code to detect the surrounding DOS/Windows environment and support routines to work with it
 * (C) 2009-2012 Jonathan Campbell.
 * Hackipedia DOS library.
 *
 * This code is licensed under the LGPL.
 * <insert LGPL legal text here>
 */

#ifdef TARGET_WINDOWS
# include <windows.h>
#endif

#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/cpu/cpu.h>
#include <hw/dos/dos.h>
#include <hw/dos/doswin.h>
#include <hw/dos/dosntvdm.h>

#if !defined(TARGET_WINDOWS) && TARGET_MSDOS == 16

/* NTS: This only allows for exception interrupts 0x00-0x1F */
void far *dpmi_getexhandler(unsigned char n) {
	unsigned short s=0,o=0;

	__asm {
		mov	ax,0x202
		mov	bl,n
		xor	cx,cx
		xor	dx,dx
		int	31h
		mov	s,cx
		mov	o,dx
	}

	return MK_FP(s,o);
}

/* NTS: This only allows for exception interrupts 0x00-0x1F */
int dpmi_setexhandler(unsigned char n,void far *x) {
	unsigned short s=FP_SEG(x),o=FP_OFF(x);
	int c=1;

	__asm {
		mov	ax,0x203
		mov	bl,n
		mov	cx,s
		mov	dx,o
		int	31h
		jnc	ok
		mov	c,0
ok:
	}

	return c;
}

#endif

#if !defined(TARGET_WINDOWS) && TARGET_MSDOS == 32

/* NTS: This only allows for exception interrupts 0x00-0x1F */
void far *dpmi_getexhandler(unsigned char n) {
	unsigned short s=0;
	unsigned int o=0;

	__asm {
		mov	ax,0x202
		mov	bl,n
		xor	cx,cx
		xor	dx,dx
		int	31h
		mov	s,cx
		mov	o,edx
	}

	return MK_FP(s,o);
}

/* NTS: This only allows for exception interrupts 0x00-0x1F */
int dpmi_setexhandler(unsigned char n,void far *x) {
	unsigned short s=FP_SEG(x);
	unsigned int o=FP_OFF(x);
	int c=1;

	__asm {
		mov	ax,0x203
		mov	bl,n
		mov	cx,s
		mov	edx,o
		int	31h
		jnc	ok
		mov	c,0
ok:
	}

	return c;
}

#endif

