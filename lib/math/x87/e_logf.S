/*
 * Written by J.T. Conklin <jtc@netbsd.org>.
 * Public domain.
 */

#include "asm.h"

ENTRY(logf)
	fldln2
	flds	4(%esp)
	fyl2x
	ret
