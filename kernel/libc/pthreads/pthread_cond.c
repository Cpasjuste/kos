#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <sys/time.h>

/* Condition Variable Initialization Attributes, P1003.1c/Draft 10, p. 96 */

int pthread_condattr_init(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared) {
    (void)attr;
    (void)pshared;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared) {
    (void)attr;
    (void)pshared;
    return 0;
}

/* Initializing and Destroying a Condition Variable, P1003.1c/Draft 10, p. 87 */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    assert(cond);

    return cond_init(cond);
}

int pthread_cond_destroy(pthread_cond_t * cond) {
    assert(cond);

    cond_destroy(cond);

    return 0;
}

/* Broadcasting and Signaling a Condition, P1003.1c/Draft 10, p. 101 */

int pthread_cond_signal(pthread_cond_t *cond) {
    assert(cond);

    cond_signal(cond);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    assert(cond);

    cond_broadcast(cond);
    return 0;
}

/* Waiting on a Condition, P1003.1c/Draft 10, p. 105 */

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    assert(cond);
    assert(mutex);

    return cond_wait(cond, mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    int tmo;
    struct timeval ctv;

    assert(cond);
    assert(mutex);
    assert(abstime);

    // We have to calculate this...
    gettimeofday(&ctv, NULL);

    // Milliseconds, Microseconds, Nanoseconds, oh my!
    tmo = abstime->tv_sec - ctv.tv_sec;
    tmo += abstime->tv_nsec / (1000 * 1000) - ctv.tv_usec / 1000;
    assert(tmo >= 0);

    if(tmo == 0)
        return ETIMEDOUT;

    return cond_wait_timed(cond, mutex, tmo);
}
