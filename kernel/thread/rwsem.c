/* KallistiOS ##version##

   rwsem.c
   Copyright (C) 2008, 2012 Lawrence Sebald
*/

/* Defines reader/writer semaphores */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <kos/rwsem.h>
#include <kos/genwait.h>

/* Allocate a new reader/writer semaphore */
rw_semaphore_t *rwsem_create() {
    rw_semaphore_t *s;

    dbglog(DBG_WARNING, "Creating reader/writer semaphore with deprecated "
           "rwsem_create(). Please update your code!\n");

    if(!(s = (rw_semaphore_t *)malloc(sizeof(rw_semaphore_t)))) {
        errno = ENOMEM;
        return NULL;
    }

    s->initialized = 2;
    s->read_count = 0;
    s->write_lock = NULL;
    s->reader_waiting = NULL;

    return s;
}

int rwsem_init(rw_semaphore_t *s) {
    s->initialized = 1;
    s->read_count = 0;
    s->write_lock = NULL;
    s->reader_waiting = NULL;

    return 0;
}

/* Destroy a reader/writer semaphore */
int rwsem_destroy(rw_semaphore_t *s) {
    int rv = 0, old;

    old = irq_disable();

    if(s->read_count || s->write_lock) {
        errno = EBUSY;
        rv = -1;
    }
    else if(s->initialized == 2) {
        free(s);
    }
    else {
        s->initialized = 0;
    }

    irq_restore(old);
    return rv;
}

/* Lock a reader/writer semaphore for reading */
int rwsem_read_lock_timed(rw_semaphore_t *s, int timeout) {
    int old, rv = 0;

    if(irq_inside_int()) {
        dbglog(DBG_WARNING, "rwsem_read_lock_timed: called inside interrupt\n");
        errno = EPERM;
        return -1;
    }

    if(timeout < 0) {
        errno = EINVAL;
        return -1;
    }

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        irq_restore(old);
        errno = EINVAL;
        return -1;
    }

    /* If the write lock is not held, let the thread proceed */
    if(!s->write_lock) {
        ++s->read_count;
    }
    else {
        /* Block until the write lock is not held any more */
        rv = genwait_wait(s, timeout ? "rwsem_read_lock_timed" :
                          "rwsem_read_lock", timeout, NULL);

        if(rv < 0) {
            rv = -1;
            if(errno == EAGAIN)
                errno = ETIMEDOUT;
        }
        else {
            ++s->read_count;
        }
    }

    irq_restore(old);
    return rv;
}

int rwsem_read_lock(rw_semaphore_t *s) {
    return rwsem_read_lock_timed(s, 0);
}

/* Lock a reader/writer semaphore for writing */
int rwsem_write_lock_timed(rw_semaphore_t *s, int timeout) {
    int old, rv = 0;

    if(irq_inside_int()) {
        dbglog(DBG_WARNING, "rwsem_write_lock_timed: called inside "
               "interrupt\n");
        errno = EPERM;
        return -1;
    }

    if(timeout < 0) {
        errno = EINVAL;
        return -1;
    }

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        irq_restore(old);
        errno = EINVAL;
        return -1;
    }

    /* If the write lock is not held and there are no readers in their critical
       sections, let the thread proceed. */
    if(!s->write_lock && !s->read_count) {
        s->write_lock = thd_current;
    }
    else {
        /* Block until the write lock is not held and there are no readers
           inside their critical sections */
        rv = genwait_wait(&s->write_lock, timeout ? "rwsem_write_lock_timed" :
                          "rwsem_write_lock", timeout, NULL);

        if(rv < 0) {
            rv = -1;
            if(errno == EAGAIN)
                errno = ETIMEDOUT;
        }
        else {
            s->write_lock = thd_current;
        }
    }

    irq_restore(old);
    return rv;
}

int rwsem_write_lock(rw_semaphore_t *s) {
    return rwsem_write_lock_timed(s, 0);
}

/* Unlock a reader/writer semaphore from a read lock. */
int rwsem_read_unlock(rw_semaphore_t *s) {
    int old;

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        irq_restore(old);
        errno = EINVAL;
        return -1;
    }

    if(!s->read_count) {
        irq_restore(old);
        errno = EPERM;
        return -1;
    }

    --s->read_count;

    /* If this was the last reader, attempt to wake any writers waiting. */
    if(!s->read_count) {
        if(s->reader_waiting) {
            genwait_wake_thd(&s->write_lock, s->reader_waiting, 0);
            s->reader_waiting = NULL;
        }
        else {
            genwait_wake_one(&s->write_lock);
        }
    }

    irq_restore(old);

    return 0;
}

/* Unlock a reader/writer semaphore from a write lock. */
int rwsem_write_unlock(rw_semaphore_t *s) {
    int old, woken;

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        irq_restore(old);
        errno = EINVAL;
        return -1;
    }

    if(s->write_lock != thd_current) {
        irq_restore(old);
        errno = EPERM;
        return -1;
    }

    s->write_lock = NULL;

    /* Give writers priority, attempt to wake any writers first. */
    woken = genwait_wake_cnt(&s->write_lock, 1, 0);

    if(!woken) {
        /* No writers were waiting, wake up any readers. */
        genwait_wake_all(s);
    }

    irq_restore(old);

    return 0;
}

int rwsem_unlock(rw_semaphore_t *s) {
    int old, rv;

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        errno = EINVAL;
        rv = -1;
    }
    else if(!s->write_lock && !s->read_count) {
        errno = EPERM;
        rv = -1;
    }
    /* Is this thread holding the write lock? */
    else if(s->write_lock == thd_current) {
        rv = rwsem_write_unlock(s);
    }
    /* Not holding the write lock, assume its holding the read lock... */
    else {
        rv = rwsem_read_unlock(s);
    }

    irq_restore(old);
    return rv;
}

/* Attempt to lock a reader/writer semaphore for reading, but do not block. */
int rwsem_read_trylock(rw_semaphore_t *s) {
    int old, rv;

    old = irq_disable();
    
    if(s->initialized != 1 && s->initialized != 2) {
        rv = -1;
        errno = EINVAL;
    }
    /* Is the write lock held? */
    else if(s->write_lock) {
        rv = -1;
        errno = EWOULDBLOCK;
    }
    else {
        rv = 0;
        ++s->read_count;
    }

    irq_restore(old);
    return rv;
}

/* Attempt to lock a reader/writer semaphore for writing, but do not block. */
int rwsem_write_trylock(rw_semaphore_t *s) {
    int old, rv;

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        rv = -1;
        errno = EINVAL;
    }
    /* Are there any readers in their critical sections, or is the write lock
       already held, if so we can't do anything about that now. */
    else if(s->read_count || s->write_lock) {
        rv = -1;
        errno = EWOULDBLOCK;
    }
    else {
        rv = 0;
        s->write_lock = thd_current;
    }

    irq_restore(old);
    return rv;
}

/* "Upgrade" a read lock to a write lock. */
int rwsem_read_upgrade_timed(rw_semaphore_t *s, int timeout) {
    int old, rv = 0;

    if(irq_inside_int()) {
        dbglog(DBG_WARNING, "rwsem_read_upgrade_timed: called inside "
               "interrupt\n");
        errno = EPERM;
        return -1;
    }

    if(timeout < 0) {
        errno = EINVAL;
        return -1;
    }

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        rv = -1;
        errno = EINVAL;
    }
    /* If there are still other readers, see if any other readers have tried to
       upgrade or not... */
    else if(s->read_count > 1) {
        if(s->reader_waiting) {
            /* We've got someone ahead of us, so there's really not anything
               that can be done at this point... */
            rv = -1;
            errno = EBUSY;
        }
        else {
            --s->read_count;
            s->reader_waiting = thd_current;
            rv = genwait_wait(&s->write_lock, timeout ?
                              "rwsem_read_upgrade_timed" : "rwsem_read_upgrade",
                              timeout, NULL);

            if(rv < 0) {
                /* The only way we can error out is if there are still readers
                   with the lock, so we can safely re-grab the lock here. */
                ++s->read_count;
                rv = -1;

                if(errno == EAGAIN)
                    errno = ETIMEDOUT;
            }
            else {
                s->write_lock = thd_current;
            }
        }
    }
    else {
        s->read_count = 0;
        s->write_lock = thd_current;
    }

    irq_restore(old);
    return rv;
}

int rwsem_read_upgrade(rw_semaphore_t *s) {
    return rwsem_read_upgrade_timed(s, 0);
}

/* Attempt to upgrade a read lock to a write lock, but do not block. */
int rwsem_read_tryupgrade(rw_semaphore_t *s) {
    int old, rv;

    old = irq_disable();

    if(s->initialized != 1 && s->initialized != 2) {
        rv = -1;
        errno = EINVAL;
    }
    else if(s->reader_waiting) {
        rv = -1;
        errno = EBUSY;
    }
    else if(s->read_count != 1) {
        rv = -1;
        errno = EWOULDBLOCK;
    }
    else {
        rv = 0;
        s->read_count = 0;
        s->write_lock = thd_current;
    }

    irq_restore(old);
    return rv;
}

/* Return the current reader count */
int rwsem_read_count(rw_semaphore_t *s) {
    return s->read_count;
}

/* Return the current status of the write lock */
int rwsem_write_locked(rw_semaphore_t *s) {
    return !!s->write_lock;
}
