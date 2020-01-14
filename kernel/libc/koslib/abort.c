/* KallistiOS ##version##

   abort.c
   (c)2001 Dan Potter

*/

#include <stdlib.h>
#include <arch/arch.h>

/* This is probably the closest mapping we've got for abort() */
void abort() {
    arch_exit();
}

