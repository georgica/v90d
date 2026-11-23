/* Linked into our programs for the OBi200 only: its kernel is Linux 2.6.30,
 * older than the one our C library (uClibc-ng 2023) assumes. Where the
 * library reaches for a newer system call, this file puts the old way in
 * its place. Statically linked, these definitions win over the library's.
 *
 *   rename   the library uses renameat2 (Linux 3.15); the old rename call
 *            is still there on ARM
 *   accept4  arrived on ARM in 2.6.36: accept, then set the flags */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int rename(const char *from, const char *to)
{
    return (int)syscall(__NR_rename, from, to);
}

int accept4(int fd, struct sockaddr *sa, socklen_t *len, int flags)
{
    int s = (int)syscall(__NR_accept4, fd, sa, len, flags);
    if (s >= 0 || errno != ENOSYS) return s;
    s = (int)syscall(__NR_accept, fd, sa, len);
    if (s < 0) return s;
    if (flags & SOCK_CLOEXEC) fcntl(s, F_SETFD, FD_CLOEXEC);
    if (flags & SOCK_NONBLOCK) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}
