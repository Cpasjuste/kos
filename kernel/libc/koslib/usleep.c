/* KallistiOS ##version##

   usleep.c
   Copyright (C)2001,2004 Dan Potter

*/

#include <kos/thread.h>

/* usleep() */
void usleep(unsigned long usec) {
    thd_sleep(usec / 1000);
}

