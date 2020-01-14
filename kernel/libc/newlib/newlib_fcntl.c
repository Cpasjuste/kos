/* KallistiOS ##version##

   newlib_fcntl.c
   Copyright (C) 2012 Lawrence Sebald

*/

#include <sys/reent.h>
#include <sys/fcntl.h>
#include <stdarg.h>

#include <kos/fs.h>

int _fcntl_r(struct _reent *reent, int fd, int cmd, int arg) {
    (void)reent;
    return fs_fcntl(fd, cmd, arg);
}

extern int fs_vfcntl(file_t fd, int cmd, va_list ap);

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    int rv;

    va_start(ap, cmd);
    rv = fs_vfcntl(fd, cmd, ap);
    va_end(ap);
    return rv;
}
