/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "async.h"
#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "macro.h"
#include "process-util.h"
#include "signal-util.h"

int asynchronous_job(void* (*func)(void *p), void *arg) {
        sigset_t ss, saved_ss;
        pthread_attr_t a;
        pthread_t t;
        int r, k;

        /* It kinda sucks that we have to resort to threads to implement an asynchronous close(), but well, such is
         * life. */

        r = pthread_attr_init(&a);
        if (r > 0)
                return -r;

        r = pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (r > 0) {
                r = -r;
                goto finish;
        }

        assert_se(sigfillset(&ss) >= 0);

        /* Block all signals before forking off the thread, so that the new thread is started with all signals
         * blocked. This way the existence of the new thread won't affect signal handling in other threads. */

        r = pthread_sigmask(SIG_BLOCK, &ss, &saved_ss);
        if (r > 0) {
                r = -r;
                goto finish;
        }

        r = pthread_create(&t, &a, func, arg);

        k = pthread_sigmask(SIG_SETMASK, &saved_ss, NULL);

        if (r > 0)
                r = -r;
        else if (k > 0)
                r = -k;
        else
                r = 0;

finish:
        pthread_attr_destroy(&a);
        return r;
}

int asynchronous_sync(pid_t *ret_pid) {
        int r;

        /* This forks off an invocation of fork() as a child process, in order to initiate synchronization to
         * disk. Note that we implement this as helper process rather than thread as we don't want the sync() to hang our
         * original process ever, and a thread would do that as the process can't exit with threads hanging in blocking
         * syscalls. */

        r = safe_fork("(sd-sync)", FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS, ret_pid);
        if (r < 0)
                return r;
        if (r == 0) {
                /* Child process */
                sync();
                _exit(EXIT_SUCCESS);
        }

        return 0;
}

/* We encode the fd to close in the userdata pointer as an unsigned value. The highest bit indicates whether
 * we need to fork again */
#define NEED_DOUBLE_FORK (1U << (sizeof(unsigned) * 8 - 1))

static int close_func(void *p) {
        unsigned v = PTR_TO_UINT(p);

        (void) prctl(PR_SET_NAME, (unsigned long*) "(close)");

        /* Note: 💣 This function is invoked in a child process created via glibc's clone() wrapper. In such
         *       children memory allocation is not allowed, since glibc does not release malloc mutexes in
         *       clone() 💣 */

        if (v & NEED_DOUBLE_FORK) {
                pid_t pid;

                v &= ~NEED_DOUBLE_FORK;

                /* This inner child will be reparented to the subreaper/PID 1. Here we turn on SIGCHLD, so
                 * that the reaper knows when it's time to reap. */
                pid = clone_with_nested_stack(close_func, SIGCHLD|CLONE_FILES, UINT_TO_PTR(v));
                if (pid >= 0)
                        return 0;
        }

        close((int) v); /* no assert() here, we are in the child and the result would be eaten up anyway */
        return 0;
}

int asynchronous_close(int fd) {
        unsigned v;
        pid_t pid;
        int r;

        /* This is supposed to behave similar to safe_close(), but actually invoke close() asynchronously, so
         * that it will never block. Ideally the kernel would have an API for this, but it doesn't, so we
         * work around it, and hide this as a far away as we can.
         *
         * It is important to us that we don't use threads (via glibc pthread) in PID 1, hence we'll do a
         * minimal subprocess instead which shares our fd table via CLONE_FILES. */

        if (fd < 0)
                return -EBADF; /* already invalid */

        PROTECT_ERRNO;

        v = (unsigned) fd;

        /* We want to fork off a process that is automatically reaped. For that we'd usually double-fork. But
         * we can optimize this a bit: if we are PID 1 or a subreaper anyway (the systemd service manager
         * process qualifies as this), we can avoid the double forking, since the double forked process would
         * be reparented back to us anyway. */
        r = reaper_process();
        if (r < 0)
                log_debug_errno(r, "Cannot determine if we are a reaper process, assuming we are not: %m");
        if (!r)
                v |= NEED_DOUBLE_FORK;

        pid = clone_with_nested_stack(close_func, CLONE_FILES | ((v & NEED_DOUBLE_FORK) ? 0 : SIGCHLD), UINT_TO_PTR(v));
        if (pid < 0)
                assert_se(close_nointr(fd) != -EBADF); /* local fallback */
        else if (v & NEED_DOUBLE_FORK) {

                /* Reap the intermediate child. Key here is that we specify __WCLONE, since we didn't ask for
                 * any signal to be sent to us on process exit, and otherwise waitid() would refuse waiting
                 * then. */
                for (;;) {
                        siginfo_t dummy = {};

                        if (waitid(P_PID, pid, &dummy, WEXITED|__WCLONE) >= 0 || errno != EINTR)
                                break;
                }
        }

        return -EBADF; /* return an invalidated fd */
}
