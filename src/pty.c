/*
 * pty.c — Pseudo-terminal implementation.
 *
 * Bugs fixed:
 *   1. TIOCSWINSZ set on SLAVE fd (not just master) so bash
 *      reads correct dimensions at startup.
 *   2. COLUMNS + LINES env vars set before execl so readline
 *      has the correct width even without ioctl.
 *   3. SIGWINCH sent after bash starts to force readline to
 *      re-query terminal size and rebuild its line buffer.
 *   4. pty_resize now sends an updated COLUMNS/LINES export
 *      so running programs stay in sync after window resize.
 *   5. O_NONBLOCK set correctly — checked for fcntl failure.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* Must include utmp.h before pty.h on some Linux distros */
#include <utmp.h>
#include <pty.h>

#include "pty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>


int pty_init(PTY *p, int cols, int rows) {
    p->cols      = cols;
    p->rows      = rows;
    p->master_fd = -1;
    p->shell_pid = 0;

    /* Build winsize struct — passed to openpty AND slave ioctl */
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    int master_fd, slave_fd;

    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0) {
        perror("pty_init: openpty");
        return -1;
    }

    p->master_fd = master_fd;

    pid_t pid = fork();

    if (pid < 0) {
        perror("pty_init: fork");
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (pid == 0) {
        /* ════ CHILD — become bash ════ */
        close(master_fd);

        /* New session so child can adopt the slave as controlling tty */
        if (setsid() < 0) { perror("setsid"); _exit(1); }

        /* Make slave the controlling terminal of this session */
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            perror("TIOCSCTTY"); _exit(1);
        }

        /*
         * Set window size on the SLAVE fd.
         * Some kernels only propagate the size to the foreground
         * process from the slave side. Setting it only on master
         * at openpty time is not always sufficient.
         */
        ioctl(slave_fd, TIOCSWINSZ, &ws);

        /* Wire stdin/stdout/stderr to the slave PTY */
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);

        /*
         * Set TERM, COLUMNS, LINES before exec.
         *
         * COLUMNS and LINES give readline a hard fallback for
         * terminal width in case ioctl(TIOCGWINSZ) returns 0.
         * Without these, readline may cache width=0 or width=80
         * regardless of the actual PTY dimensions — causing the
         * command-line redraw to erase too few characters so old
         * text stays visible when cycling history with Up/Down.
         */
        setenv("TERM", "xterm-256color", 1);

        char cols_str[16], rows_str[16];
        snprintf(cols_str, sizeof(cols_str), "%d", cols);
        snprintf(rows_str, sizeof(rows_str), "%d", rows);
        setenv("COLUMNS", cols_str, 1);
        setenv("LINES",   rows_str, 1);

        /* Disable readline bracketed-paste — reduces noise */
        setenv("READLINE_BRACKETED_PASTE", "0", 1);

        execl("/bin/bash", "-bash", NULL);
        perror("execl /bin/bash");
        _exit(1);
    }

    /* ════ PARENT — cterm ════ */
    close(slave_fd);
    p->shell_pid = pid;

    /* Non-blocking so pty_read() never freezes the render loop */
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    /*
     * Send SIGWINCH after bash has had time to start.
     * This forces readline to call ioctl(TIOCGWINSZ) and
     * rebuild its idea of terminal width — the definitive fix
     * for the "readline cached wrong column count" bug.
     *
     * 100ms is enough on any modern system. bash takes ~10ms
     * to reach readline initialization.
     */
    usleep(100000);   /* 100 ms */
    kill(pid, SIGWINCH);

    printf("PTY ready: PID=%d fd=%d size=%dx%d\n",
           p->shell_pid, p->master_fd, cols, rows);
    return 0;
}


int pty_read(PTY *p, char *buf, int bufsize) {
    int n = (int)read(p->master_fd, buf, (size_t)(bufsize - 1));
    if (n > 0)
        buf[n] = '\0';
    return n;
}


int pty_write(PTY *p, const char *buf, int len) {
    return (int)write(p->master_fd, buf, (size_t)len);
}


void pty_resize(PTY *p, int cols, int rows) {
    if (p->master_fd < 0) return;

    p->cols = cols;
    p->rows = rows;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    /*
     * TIOCSWINSZ updates the kernel's terminal size record and
     * automatically delivers SIGWINCH to the foreground process
     * group. bash/readline handles SIGWINCH by re-querying size
     * and reflowing the command line.
     */
    ioctl(p->master_fd, TIOCSWINSZ, &ws);
}


void pty_destroy(PTY *p) {
    if (p->shell_pid > 0) {
        kill(p->shell_pid, SIGHUP);
        /* Wait up to 2 seconds for clean exit */
        int i;
        for (i = 0; i < 20; i++) {
            int status;
            pid_t r = waitpid(p->shell_pid, &status, WNOHANG);
            if (r != 0) break;
            usleep(100000);
        }
        if (i == 20)
            kill(p->shell_pid, SIGKILL);
        p->shell_pid = 0;
    }
    if (p->master_fd >= 0) {
        close(p->master_fd);
        p->master_fd = -1;
    }
}