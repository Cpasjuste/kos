/* KallistiOS ##version##

   once.c
   Copyright (C) 2009 Lawrence Sebald
*/

#include <malloc.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include <kos/once.h>
#include <kos/mutex.h>
#include <arch/irq.h>

/* The lock used to make sure multiple threads don't try to run the same routine
   at the same time. */
static mutex_t lock = RECURSIVE_MUTEX_INITIALIZER;

int kthread_once(kthread_once_t *once_control, void (*init_routine)(void)) {
    assert(once_control);

    /* Lock the lock. */
    if(mutex_lock(&lock) == -1) {
        return -1;
    }

    /* If the function has already been run, unlock the lock and return. */
    if(once_control->run) {
        mutex_unlock(&lock);
        return 0;
    }

    /* Run the function, set the control, and unlock the lock. */
    once_control->run = 1;
    init_routine();
    mutex_unlock(&lock);

    return 0;
}
