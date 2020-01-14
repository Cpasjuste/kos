/* KallistiOS ##version##

   pthread_tls.c
   Copyright (C) 2011 Lawrence Sebald
*/

#include <pthread.h>

/* Dynamic Package Initialization */
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    return kthread_once(once_control, init_routine);
}

/* Thread-Specific Data Key Create, P1003.1c/Draft 10, p. 163 */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    return kthread_key_create(key, destructor);
}

/* Thread-Specific Data Management, P1003.1c/Draft 10, p. 165 */

int pthread_setspecific(pthread_key_t key, const void *value) {
    return kthread_setspecific(key, value);
}

void * pthread_getspecific(pthread_key_t key) {
    return kthread_getspecific(key);
}

/* Thread-Specific Data Key Deletion, P1003.1c/Draft 10, p. 167 */

int pthread_key_delete(pthread_key_t key) {
    return kthread_key_delete(key);
}
