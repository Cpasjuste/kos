/* KallistiOS ##version##

   kernel/thread/thread.c
   Copyright (C) 2000, 2001, 2002, 2003 Dan Potter
   Copyright (C) 2010, 2016 Lawrence Sebald
*/

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <reent.h>
#include <errno.h>
#include <kos/thread.h>
#include <kos/dbgio.h>
#include <kos/sem.h>
#include <kos/rwsem.h>
#include <kos/cond.h>
#include <kos/genwait.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <arch/arch.h>

/*

This module supports thread scheduling in KOS. The timer interrupt is used
to re-schedule the processor HZ times per second in pre-emptive mode.
This is a fairly simplistic scheduler, though it does employ some
standard advanced OS tactics like priority scheduling and semaphores.

Some of this code ought to look familiar to BSD-heads; I studied the
BSD kernel quite a bit to get some ideas on priorities, and I am
also using their queue library verbatim (sys/queue.h).

*/

/*****************************************************************************/
/* Thread scheduler data */

/* Thread list. This includes all threads except dead ones. */
static struct ktlist thd_list;

/* Run queue. This is more like on a standard time sharing system than the
   previous versions. The top element of this priority queue should be the
   thread that is ready to run next. When a thread is scheduled, it will be
   removed from this queue. When it's de-scheduled, it will be re-inserted
   by its priority value at the end of its priority group. Note that right
   now this condition is being broken because sleeping threads are on the
   same queue. We deal with those in thd_switch below. */
static struct ktqueue run_queue;

/* The currently executing thread. This thread should not be on any queues. */
kthread_t *thd_current = NULL;

/* Thread mode: cooperative or pre-emptive. */
static int thd_mode = THD_MODE_NONE;

/* Reaper semaphore. Counts the number of threads waiting to be reaped. */
static semaphore_t thd_reap_sem;

/* Number of threads active in the system. */
static uint32 thd_count = 0;

/* The idle task */
static kthread_t *thd_idle_thd = NULL;

/*****************************************************************************/
/* Debug */

static const char *thd_state_to_str(kthread_t * thd) {
    switch(thd->state) {
        case STATE_ZOMBIE:
            return "zombie";
        case STATE_RUNNING:
            return "running";
        case STATE_READY:
            return "ready";
        case STATE_WAIT:

            if(thd->wait_msg)
                return thd->wait_msg;
            else
                return "wait";

        case STATE_FINISHED:
            return "finished";
        default:
            return "unknown";
    }
}

int thd_each(int (*cb)(kthread_t* thd, void* user_data), void* data) {
    kthread_t *cur;

    LIST_FOREACH(cur, &thd_list, t_list) {
        cb(cur, data);
    }

    return 0;
}

int thd_pslist(int (*pf)(const char *fmt, ...)) {
    kthread_t *cur;

    pf("All threads (may not be deterministic):\n");
    pf("addr\t\ttid\tprio\tflags\twait_timeout\tstate     name\n");

    LIST_FOREACH(cur, &thd_list, t_list) {
        pf("%08lx\t", CONTEXT_PC(cur->context));
        pf("%d\t", cur->tid);

        if(cur->prio == PRIO_MAX)
            pf("MAX\t");
        else
            pf("%d\t", cur->prio);

        pf("%08lx\t", cur->flags);
        pf("%ld\t\t", (uint32)cur->wait_timeout);
        pf("%10s", thd_state_to_str(cur));
        pf("%s\n", cur->label);
    }
    pf("--end of list--\n");

    return 0;
}

int thd_pslist_queue(int (*pf)(const char *fmt, ...)) {
    kthread_t *cur;

    pf("Queued threads:\n");
    pf("addr\t\ttid\tprio\tflags\twait_timeout\tstate     name\n");
    TAILQ_FOREACH(cur, &run_queue, thdq) {
        pf("%08lx\t", CONTEXT_PC(cur->context));
        pf("%d\t", cur->tid);

        if(cur->prio == PRIO_MAX)
            pf("MAX\t");
        else
            pf("%d\t", cur->prio);

        pf("%08lx\t", cur->flags);
        pf("%ld\t\t", (uint32)cur->wait_timeout);
        pf("%10s", thd_state_to_str(cur));
        pf("%s\n", cur->label);
    }

    return 0;
}

/*****************************************************************************/
/* Returns a fresh thread ID for each new thread */

/* Highest thread id (used when assigning next thread id) */
static tid_t tid_highest;

/* Return the next available thread id (assumes wraparound will not run
   into old processes). */
static tid_t thd_next_free() {
    int id;
    id = tid_highest++;
    return id;
}

/* Given a thread ID, locates the thread structure */
kthread_t *thd_by_tid(tid_t tid) {
    kthread_t *np;

    LIST_FOREACH(np, &thd_list, t_list) {
        if(np->tid == tid)
            return np;
    }

    return NULL;
}


/*****************************************************************************/
/* Thread support routines: idle task and start task wrapper */

/* An idle function. This function literally does nothing but loop
   forever. It's meant to be used for an idle task. */
static void *thd_idle_task(void *param) {
    /* Uncomment these if you want some debug for deadlocking */
    /*  int old = irq_disable();
    #ifndef NDEBUG
        thd_pslist();
        printf("Inside idle task now\n");
    #endif
        irq_restore(old); */
    (void)param;

    for(;;) {
        arch_sleep();   /* We can safely enter sleep mode here */
    }

    /* Never reached */
    abort();
}

/* Reaper function. This function is here to reap old zombie threads as they are
   created. */
static void *thd_reaper(void *param) {
    kthread_t *thd;

    (void)param;

    for(;;) {
        /* Wait til we have something to reap */
        sem_wait(&thd_reap_sem);

        /* Find the first zombie thread and reap it (only do one at a time so
           that the semaphore stays current) */
        LIST_FOREACH(thd, &thd_list, t_list) {
            if(thd->state == STATE_ZOMBIE) {
                thd_destroy(thd);
                break;
            }
        }
    }

    /* Never reached */
    abort();
}

/* Thread execution wrapper; when the thd_create function below
   adds a new thread to the thread chain, this function is the one
   that gets called in the new context. */
static void thd_birth(void * (*routine)(void *param), void *param) {
    /* Call the thread function */
    void *rv = routine(param);

    /* Die */
    thd_exit(rv);
}

/* Terminate the current thread */
void thd_exit(void *rv) {
    /* The thread's never coming back so we don't need to bother saving the
       interrupt state at all. Disable interrupts just to make sure nothing
       changes underneath us while we're doing our thing here */
    irq_disable();

    /* Set the return value of the thread */
    thd_current->rv = rv;

    /* Call newlib's thread cleanup function */
    _reclaim_reent(&thd_current->thd_reent);

    if(thd_current->flags & THD_DETACHED) {
        /* Call Dr. Kevorkian; after this executes we could be killed
           at any time. */
        thd_current->state = STATE_ZOMBIE;
        sem_signal(&thd_reap_sem);
    }
    else {
        /* Mark the thread as finished and wake up anyone that has tried to join
           with it */
        thd_current->state = STATE_FINISHED;
        genwait_wake_all(thd_current);
    }

    /* Manually reschedule */
    thd_block_now(&thd_current->context);

    /* not reached */
    abort();
}


/*****************************************************************************/
/* Thread creation and deletion */

/* Enqueue a process in the runnable queue; adds it right after the
   process group of the same priority (front_of_line==0) or
   right before the process group of the same priority (front_of_line!=0).
   See thd_schedule for why this is helpful. */
void thd_add_to_runnable(kthread_t *t, int front_of_line) {
    kthread_t *i;
    int done;

    if(t->flags & THD_QUEUED)
        return;

    done = 0;

    if(!front_of_line) {
        /* Look for a thread of lower priority and insert
           before it. If there is nothing on the run queue, we'll
           fall through to the bottom. */
        TAILQ_FOREACH(i, &run_queue, thdq) {
            if(i->prio > t->prio) {
                TAILQ_INSERT_BEFORE(i, t, thdq);
                done = 1;
                break;
            }
        }
    }
    else {
        /* Look for a thread of the same or lower priority and
           insert before it. If there is nothing on the run queue,
           we'll fall through to the bottom. */
        TAILQ_FOREACH(i, &run_queue, thdq) {
            if(i->prio >= t->prio) {
                TAILQ_INSERT_BEFORE(i, t, thdq);
                done = 1;
                break;
            }
        }
    }

    /* Didn't find one, put it at the end */
    if(!done)
        TAILQ_INSERT_TAIL(&run_queue, t, thdq);

    t->flags |= THD_QUEUED;
}

/* Removes a thread from the runnable queue, if it's there. */
int thd_remove_from_runnable(kthread_t *thd) {
    if(!(thd->flags & THD_QUEUED)) return 0;

    thd->flags &= ~THD_QUEUED;
    TAILQ_REMOVE(&run_queue, thd, thdq);
    return 0;
}

/* New thread function; given a routine address, it will create a
   new kernel thread with the given attributes. When the routine
   returns, the thread will exit. Returns the new thread struct. */
kthread_t *thd_create_ex(kthread_attr_t *attr, void * (*routine)(void *param),
                         void *param) {
    kthread_t *nt = NULL;
    tid_t tid;
    uint32 params[4];
    int oldirq = 0;
    kthread_attr_t real_attr = { 0, THD_STACK_SIZE, NULL, PRIO_DEFAULT, NULL };

    if(attr)
        real_attr = *attr;

    /* Look through the attributes and see what we have. If any are set to 0,
       then default them now to save ourselves trouble later. */
    if(real_attr.stack_ptr && !real_attr.stack_size) {
        errno = EINVAL;
        return NULL;
    }

    if(!real_attr.stack_size)
        real_attr.stack_size = THD_STACK_SIZE;

    if(!real_attr.prio)
        real_attr.prio = PRIO_DEFAULT;

    oldirq = irq_disable();

    /* Get a new thread id */
    tid = thd_next_free();

    if(tid >= 0) {
        /* Create a new thread structure */
        nt = malloc(sizeof(kthread_t));

        if(nt != NULL) {
            /* Clear out potentially unused stuff */
            memset(nt, 0, sizeof(kthread_t));

            /* Create a new thread stack */
            if(!real_attr.stack_ptr) {
                nt->stack = (uint32*)malloc(real_attr.stack_size);

                if(!nt->stack) {
                    free(nt);
                    irq_restore(oldirq);
                    return NULL;
                }
            }
            else {
                nt->stack = (uint32*)real_attr.stack_ptr;
            }

            nt->stack_size = real_attr.stack_size;

            /* Populate the context */
            params[0] = (uint32)routine;
            params[1] = (uint32)param;
            params[2] = 0;
            params[3] = 0;
            irq_create_context(&nt->context,
                               ((uint32)nt->stack) + nt->stack_size,
                               (uint32)thd_birth, params, 0);

            nt->tid = tid;
            nt->prio = real_attr.prio;
            nt->flags = THD_DEFAULTS;
            nt->state = STATE_READY;

            if(!real_attr.label) {
                strcpy(nt->label, "[un-named kernel thread]");
            }
            else {
                strncpy(nt->label, real_attr.label, 255);
                nt->label[255] = 0;
            }

            if(thd_current)
                strcpy(nt->pwd, thd_current->pwd);
            else
                strcpy(nt->pwd, "/");

            _REENT_INIT_PTR((&(nt->thd_reent)));

            /* Should we detach the thread? */
            if(real_attr.create_detached)
                nt->flags |= THD_DETACHED;

            /* Initialize thread-local storage. */
            LIST_INIT(&nt->tls_list);

            /* Insert it into the thread list */
            LIST_INSERT_HEAD(&thd_list, nt, t_list);

            /* Add it to our count */
            ++thd_count;

            /* Schedule it */
            thd_add_to_runnable(nt, 0);
        }
    }

    irq_restore(oldirq);
    return nt;
}

kthread_t *thd_create(int detach, void *(*routine)(void *), void *param) {
    kthread_attr_t attrs = { detach, 0, 0, 0, 0 };
    return thd_create_ex(&attrs, routine, param);
}

/* Given a thread id, this function removes the thread from
   the execution chain. */
int thd_destroy(kthread_t *thd) {
    int oldirq = 0;
    kthread_tls_kv_t *i, *i2;

    /* Make sure there are no ints */
    oldirq = irq_disable();

    /* If any threads were waiting on this one, then go ahead
       and unblock them. */
    genwait_wake_all(thd);

    /* De-schedule the thread if it's scheduled and free the
       thread structure */
    thd_remove_from_runnable(thd);
    LIST_REMOVE(thd, t_list);

    /* Clean up any thread-local data */
    LIST_FOREACH(i, &thd->tls_list, kv_list) {
        if(i->destructor) {
            i->destructor(i->data);
        }
    }

    i = LIST_FIRST(&thd->tls_list);

    while(i != NULL) {
        i2 = LIST_NEXT(i, kv_list);
        free(i);
        i = i2;
    }

    /* Free its stack */
    free(thd->stack);

    /* Free the thread */
    free(thd);

    /* Remove it from the count */
    --thd_count;

    /* Put ints back the way they were */
    irq_restore(oldirq);

    return 0;
}

/*****************************************************************************/
/* Thread attribute functions */

/* Set a thread's priority */
int thd_set_prio(kthread_t *thd, prio_t prio) {
    /* Set the new priority */
    thd->prio = prio;
    return 0;
}

/*****************************************************************************/
/* Scheduling routines */

/* Thread scheduler; this function will find a new thread to run when a
   context switch is requested. No work is done in here except to change
   out the thd_current variable contents. Assumed that we are in an
   interrupt context.

   In the normal operation mode, the current thread is pushed back onto
   the run queue at the end of its priority group. This implements the
   standard round robin scheduling within priority groups. If you set the
   front_of_line parameter to non-zero, then this behavior is modified:
   the current thread is pushed onto the run queue at the _front_ of its
   priority group. The effect is that no context switching is done, but
   priority groups are re-checked. This is useful when returning from an
   IRQ after doing something like a sem_signal, where you'd ideally like
   to make sure the priorities are all straight before returning, but you
   don't want a full context switch inside the same priority group.
*/
void thd_schedule(int front_of_line, uint64 now) {
    int dontenq;
    kthread_t *thd;

    if(now == 0)
        now = timer_ms_gettime64();

    /* We won't re-enqueue the current thread if it's NULL (i.e., the
       thread blocked itself somewhere) or if it's a zombie (below) */
    dontenq = !thd_current;

    /* If there's only two thread left, it's the idle task and the reaper task:
       exit the OS */
    if(thd_count == 2) {
        dbgio_printf("\nthd_schedule: idle tasks are the only things left; exiting\n");
        arch_exit();
    }

    /* If the current thread is supposed to be in the front of the line, and it
       did not die, re-enqueue it to the front of the line now. */
    if(front_of_line && !dontenq && thd_current->state == STATE_RUNNING) {
        thd_current->state = STATE_READY;
        thd_add_to_runnable(thd_current, front_of_line);
    }

    /* Look for timed out waits */
    genwait_check_timeouts(now);

    /* Search downwards through the run queue for a runnable thread; if
       we don't find a normal runnable thread, the idle process will
       always be there at the bottom. */
    TAILQ_FOREACH(thd, &run_queue, thdq) {
        /* Is it runnable? If not, keep going */
        if(thd->state == STATE_READY)
            break;
    }

    /* If we didn't already re-enqueue the thread and we are supposed to do so,
       do it now. */
    if(!front_of_line && !dontenq && thd_current->state == STATE_RUNNING) {
        thd_current->state = STATE_READY;
        thd_add_to_runnable(thd_current, front_of_line);

        /* Make sure we have a thread, just in case we couldn't find anything
           above. */
        if(thd == NULL || thd == thd_idle_thd)
            thd = thd_current;
    }

    /* Didn't find one? Big problem here... */
    if(thd == NULL) {
        thd_pslist(printf);
        arch_panic("couldn't find a runnable thread");
    }

    /* We should now have a runnable thread, so remove it from the
       run queue and switch to it. */
    thd_remove_from_runnable(thd);

    thd_current = thd;
    _impure_ptr = &thd->thd_reent;
    thd->state = STATE_RUNNING;

    /* Make sure the thread hasn't underrun its stack */
    if(thd_current->stack && thd_current->stack_size) {
        if(CONTEXT_SP(thd_current->context) < (ptr_t)(thd_current->stack)) {
            thd_pslist(printf);
            thd_pslist_queue(printf);
            assert_msg(0, "Thread stack underrun");
        }
    }

    irq_set_context(&thd_current->context);
}

/* Temporary priority boosting function: call this from within an interrupt
   to boost the given thread to the front of the queue. This will cause the
   interrupt return to jump back to the new thread instead of the one that
   was executing (unless it was already executing). */
void thd_schedule_next(kthread_t *thd) {
    /* Make sure we're actually inside an interrupt */
    if(!irq_inside_int())
        return;

    /* Can't boost a blocked thread */
    if(thd->state != STATE_READY)
        return;

    /* Unfortunately we have to take care of this here */
    if(thd_current->state == STATE_ZOMBIE) {
        sem_signal(&thd_reap_sem);
    }
    else if(thd_current->state == STATE_RUNNING) {
        thd_current->state = STATE_READY;
        thd_add_to_runnable(thd_current, 0);
    }

    thd_remove_from_runnable(thd);
    thd_current = thd;
    _impure_ptr = &thd->thd_reent;
    thd_current->state = STATE_RUNNING;
    irq_set_context(&thd_current->context);
}

/* See kos/thread.h for description */
irq_context_t * thd_choose_new() {
    uint64 now = timer_ms_gettime64();

    //printf("thd_choose_new() woken at %d\n", (uint32)now);

    /* Do any re-scheduling */
    thd_schedule(0, now);

    /* Return the new IRQ context back to the caller */
    return &thd_current->context;
}

/*****************************************************************************/

/* Timer function. Check to see if we were woken because of a timeout event
   or because of a pre-empt. For timeouts, just go take care of it and sleep
   again until our next context switch (if any). For pre-empts, re-schedule
   threads, swap out contexts, and sleep. */
static void thd_timer_hnd(irq_context_t *context) {
    /* Get the system time */
    uint64 now = timer_ms_gettime64();

    (void)context;

    //printf("timer woke at %d\n", (uint32)now);

    thd_schedule(0, now);
    timer_primary_wakeup(1000 / HZ);
}

/*****************************************************************************/

/* Thread blocking based sleeping; this is the preferred way to
   sleep because it eases the load on the system for the other
   threads. */
void thd_sleep(int ms) {
    if(thd_mode == THD_MODE_NONE) {
        timer_spin_sleep(ms);
        return;
    }

    /* A timeout of zero is the same as thd_pass() and passing zero
       down to genwait_wait() causes bad juju. */
    if(!ms) {
        thd_pass();
        return;
    }

    /* We can genwait on a non-existant object here with a timeout and
       have the exact same effect; as a nice bonus, this collapses both
       sleep cases into a single case, which is nice for scheduling
       purposes. 0xffffffff definitely doesn't exist as an object, so we'll
       use that for straight up timeouts. */
    genwait_wait((void *)0xffffffff, "thd_sleep", ms, NULL);
}

/* Manually cause a re-schedule */
void thd_pass() {
    /* Makes no sense inside int */
    if(irq_inside_int()) return;

    /* Pass off control manually */
    thd_block_now(&thd_current->context);
}

/* Wait for a thread to exit */
int thd_join(kthread_t * thd, void **value_ptr) {
    int old, rv;
    kthread_t * t = NULL;

    /* Can't scan for NULL threads */
    if(thd == NULL)
        return -1;

    if(irq_inside_int()) {
        dbglog(DBG_WARNING, "thd_join(%p) called inside an interrupt!\n",
               (void *)thd);
        return -1;
    }

    old = irq_disable();

    /* Search the thread list and make sure that this thread hasn't
       already died and been deallocated. */
    LIST_FOREACH(t, &thd_list, t_list) {
        if(t == thd)
            break;
    }

    /* Did we find anything? */
    if(t != thd) {
        rv = -2;
    }
    else if(thd->flags & THD_DETACHED) {
        /* Can't join a detached thread */
        rv = -3;
    }
    else {
        if(thd->state != STATE_FINISHED) {
            /* Wait for the target thread to die */
            genwait_wait(thd, "thd_join", 0, NULL);
        }

        /* Ok, we're all clear */
        rv = 0;

        if(value_ptr)
            *value_ptr = thd->rv;

        /* The thread can be destroyed now */
        thd_destroy(thd);
    }

    irq_restore(old);
    return rv;
}

/* Detach a joinable thread */
int thd_detach(kthread_t *thd) {
    int old, rv = 0;
    kthread_t * t = NULL;

    /* Can't scan for NULL threads */
    if(thd == NULL)
        return -1;

    old = irq_disable();

    /* Search the thread list and make sure that this thread hasn't
       already died and been deallocated. */
    LIST_FOREACH(t, &thd_list, t_list) {
        if(t == thd)
            break;
    }

    /* Did we find anything? */
    if(t != thd) {
        rv = -2;
    }
    else if(thd->flags & THD_DETACHED) {
        /* Can't detach an already detached thread */
        rv = -3;
    }
    else if(thd->state == STATE_FINISHED) {
        /* If the thread is already finished, deallocate it now */
        thd_destroy(thd);
    }
    else {
        /* Set the detached flag and return */
        thd->flags |= THD_DETACHED;
    }

    irq_restore(old);
    return rv;
}


/*****************************************************************************/
/* Retrive / set thread label */
const char *thd_get_label(kthread_t *thd) {
    return thd->label;
}

void thd_set_label(kthread_t *thd, const char *label) {
    strncpy(thd->label, label, sizeof(thd->label) - 1);
}

/* Find the current thread */
kthread_t *thd_get_current() {
    return thd_current;
}

/* Retrieve / set thread pwd */
const char *thd_get_pwd(kthread_t *thd) {
    return thd->pwd;
}

void thd_set_pwd(kthread_t *thd, const char *pwd) {
    strncpy(thd->pwd, pwd, sizeof(thd->pwd) - 1);
}

int * thd_get_errno(kthread_t * thd) {
    return &thd->thd_errno;
}

struct _reent * thd_get_reent(kthread_t *thd) {
    return &thd->thd_reent;
}

/*****************************************************************************/

/* Change threading modes */
int thd_set_mode(int mode) {
    int old = thd_mode;

    /* Nothing to change? */
    if(thd_mode == mode)
        return thd_mode;

    if(thd_mode == THD_MODE_COOP) {
        /* Schedule our first pre-emption wakeup */
        timer_primary_wakeup(1000 / HZ);
    }

    thd_mode = mode;

    return old;
}

int thd_get_mode(void) {
    return thd_mode;
}

/* Delete a TLS key. Note that currently this doesn't prevent you from reusing
   the key after deletion. This seems ok, as the pthreads standard states that
   using the key after deletion results in "undefined behavior".
   XXXX: This should really be in tls.c, but we need the list of threads to go
   through, so it ends up here instead. */
int kthread_key_delete(kthread_key_t key) {
    int old = irq_disable();
    kthread_t *cur;
    kthread_tls_kv_t *i;

    /* Make sure the key is valid. */
    if(key >= kthread_key_next() || key < 1) {
        irq_restore(old);
        errno = EINVAL;
        return -1;
    }

    /* Make sure we can actually use free below. */
    if(!malloc_irq_safe())  {
        irq_restore(old);
        errno = EPERM;
        return -1;
    }

    /* Go through each thread searching for (and removing) the data. */
    LIST_FOREACH(cur, &thd_list, t_list) {
        LIST_FOREACH(i, &cur->tls_list, kv_list) {
            if(i->key == key) {
                LIST_REMOVE(i, kv_list);
                free(i);
                break;
            }
        }
    }

    kthread_key_delete_destructor(key);

    irq_restore(old);
    return 0;
}

/*****************************************************************************/
/* Init/shutdown */

/* Init */
int thd_init(int mode) {
    kthread_t *kern, *reaper;

    /* Make sure we're not already running */
    if(thd_mode != THD_MODE_NONE)
        return -1;

    /* Setup our mode as appropriate */
    thd_mode = mode;

    /* Initialize handle counters */
    tid_highest = 1;

    /* Initialize the thread list */
    LIST_INIT(&thd_list);

    /* Initialize the run queue */
    TAILQ_INIT(&run_queue);

    /* Start off with no "current" thread */
    thd_current = NULL;

    /* Init thread-local storage. */
    kthread_tls_init();

    /* Reinitialize thread counter */
    thd_count = 0;

    /* Setup a kernel task for the currently running "main" thread */
    kern = thd_create(0, NULL, NULL);
    strcpy(kern->label, "[kernel]");
    kern->state = STATE_RUNNING;

    /* De-scehdule the thread (it's STATE_RUNNING) */
    thd_remove_from_runnable(kern);

    /* Setup an idle task that is always ready to run, in case everyone
       else is blocked on something. */
    thd_idle_thd = thd_create(0, thd_idle_task, NULL);
    strcpy(thd_idle_thd->label, "[idle]");
    thd_set_prio(thd_idle_thd, PRIO_MAX);
    thd_idle_thd->state = STATE_READY;

    /* Set up a thread to reap old zombies */
    sem_init(&thd_reap_sem, 0);
    reaper = thd_create(0, thd_reaper, NULL);
    strcpy(reaper->label, "[reaper]");
    thd_set_prio(reaper, 1);

    /* Main thread -- the kern thread */
    thd_current = kern;
    irq_set_context(&kern->context);

    /* Initialize thread sync primitives */
    genwait_init();

    /* Setup our pre-emption handler */
    timer_primary_set_callback(thd_timer_hnd);

    /* If we're in pre-emptive mode, then schedule the first context switch */
    if(thd_mode == THD_MODE_PREEMPT) {
        /* Schedule our first wakeup */
        timer_primary_wakeup(1000 / HZ);

        printf("thd: pre-emption enabled, HZ=%d\n", HZ);
    }
    else
        printf("thd: pre-emption disabled\n");

    return 0;
}

/* Shutdown */
void thd_shutdown() {
    kthread_t *n1, *n2;

    /* Disable pre-emption, if neccessary */
    if(thd_mode == THD_MODE_PREEMPT) {
        timer_primary_set_callback(NULL);
    }

    /* Kill remaining live threads */
    n1 = LIST_FIRST(&thd_list);

    while(n1 != NULL) {
        n2 = LIST_NEXT(n1, t_list);
        free(n1->stack);
        free(n1);
        n1 = n2;
    }

    sem_destroy(&thd_reap_sem);

    /* Shutdown thread sync primitives */
    genwait_shutdown();

    kthread_tls_shutdown();

    /* Not running */
    thd_mode = THD_MODE_NONE;
    thd_count = 0;

    // XXX _impure_ptr is borked
}
