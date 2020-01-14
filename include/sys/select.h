/* KallistiOS ##version##

   sys/select.h
   Copyright (C) 2012 Lawrence Sebald

*/

/** \file   select.h
    \brief  Definitions for the select() function.

    This file contains the definitions needed for using the select() function,
    as directed by the POSIX 2008 standard (aka The Open Group Base
    Specifications Issue 7). Currently the functionality defined herein only
    really works for sockets, and that is likely how it will stay for some time.
 
    \author Lawrence Sebald
*/

#ifndef __SYS_SELECT_H
#define __SYS_SELECT_H

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#include <time.h>
#include <sys/time.h>

/* <sys/types.h> defines fd_set and friends for us, so there's really not much
   that we have to do here... */

/** \brief  Wait for activity on a group of file descriptors.

    This function will check the specfied group of file descriptors for activity
    and wait for activity (up to the timeout specified) if there is not any
    pending events already.

    \param  nfds        The maximum fd specified in any of the sets, plus 1.
    \param  readfds     File descriptors to check for the ability to read
                        without blocking.
    \param  writefds    File descriptors to check for the ability to write
                        without blocking.
    \param  errorfds    File descriptors to check for error/exceptional
                        conditions.
    \param  timeout     Maximum amount of time to block. Passing a 0 timeout
                        will make the funciton not block, Passing NULL here will
                        make the function block indefinitely.
    \return             -1 on error (sets errno as appropriate), or the number
                        of bits set in the fd sets on success (this may be 0 if
                        the timeout expires).
*/
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds,
           struct timeval *timeout);

__END_DECLS

#endif /* !__SYS_SELECT_H */
