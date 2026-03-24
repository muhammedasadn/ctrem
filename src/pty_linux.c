/*
 * pty.c — Pseudo-terminal implementation for cterm.
 *
 * IMPORTANT: _GNU_SOURCE and _DEFAULT_SOURCE must be the very
 * first lines before any #include. The system headers use
 * feature-test macros to decide what to expose. If these
 * defines appear after an #include, that header has already
 * been processed and openpty() declaration was skipped.
 */

/* Feature-test macros — MUST come before every #include */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/*
 * On Ubuntu/Debian, openpty() is declared in <pty.h>.
 * Including <utmp.h> first ensures all required types
 * (struct utmp etc.) are defined before <pty.h> uses them.
 * This is the correct include order for these headers.
 */
#include <utmp.h>
#include <pty.h>

/* Now include our own header and the rest of the standard lib */
#include "pty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>   /* ioctl(), struct winsize, TIOCSWINSZ  */
#include <sys/wait.h>    /* waitpid()                             */
#include <signal.h>      /* SIGHUP, kill()                        */


/* ── pty_init ───────────────────────────────────────────────── */

/*
 * Opens a PTY pair, forks a child, and exec's bash.
 *
 * How fork+exec works:
 *   fork()  — splits the process into two identical copies.
 *             Returns 0 in the child, child-PID in the parent.
 *   exec()  — replaces the child's program image with bash.
 *             After exec, the child IS bash.
 *
 * Parent keeps the master_fd and communicates with bash.
 * Child connects the slave_fd to stdin/stdout/stderr and
 * becomes bash via execl().
 */
int pty_init(PTY *p, int cols, int rows) {
    p->cols = cols;
    p->rows = rows;

    /*
     * struct winsize — terminal dimensions in characters.
     * bash and TUI programs (vim, htop, nano) read this to
     * know how wide to wrap lines and draw their interfaces.
     */
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;

    int master_fd, slave_fd;

    /*
     * openpty() — ask the kernel for a fresh PTY pair.
     * Fills master_fd (our end) and slave_fd (bash's end).
     */
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0) {
        perror("pty_init: openpty failed");
        return -1;
    }

    p->master_fd = master_fd;

    /*
     * fork() — create a copy of this process.
     *
     * After fork(), two processes run simultaneously:
     *   child  (pid == 0): will become bash
     *   parent (pid > 0) : stays as cterm
     */
    pid_t pid = fork();

    if (pid < 0) {
        perror("pty_init: fork failed");
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (pid == 0) {
        /* ════════════════════════════════════════════════════
         * CHILD PROCESS — become bash
         * ════════════════════════════════════════════════════ */

        /* Child doesn't use the master end */
        close(master_fd);

        /*
         * setsid() — create a new session.
         * Detaches from cterm's controlling terminal so the
         * child can adopt the PTY slave as its own.
         */
        if (setsid() < 0) {
            perror("child: setsid failed");
            _exit(1);
        }

        /*
         * TIOCSCTTY — make slave_fd the controlling terminal
         * of this new session. Required so that bash correctly
         * receives signals like Ctrl+C (SIGINT).
         */
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            perror("child: TIOCSCTTY failed");
            _exit(1);
        }

        /*
         * dup2(old, new) — make 'new' a copy of 'old'.
         * Connects standard streams to the slave PTY so that
         * bash reads from and writes to our terminal.
         */
        dup2(slave_fd, STDIN_FILENO);   /* fd 0 = stdin  */
        dup2(slave_fd, STDOUT_FILENO);  /* fd 1 = stdout */
        dup2(slave_fd, STDERR_FILENO);  /* fd 2 = stderr */

        /* slave_fd is now redundant — we have it as 0,1,2 */
        close(slave_fd);

        /*
         * Tell programs what terminal type we emulate.
         * xterm-256color = VT100 + ANSI colors + 256-color palette.
         */
        setenv("TERM", "xterm-256color", 1);

        /*
         * execl() — replace this process with bash.
         * The leading "-" in "-bash" tells bash to act as a
         * login shell and source ~/.bashrc, ~/.profile etc.
         * If execl returns, it failed.
         */
        execl("/bin/bash", "-bash", NULL);

        perror("child: execl /bin/bash failed");
        _exit(1);
    }

    /* ════════════════════════════════════════════════════════════
     * PARENT PROCESS — continue as cterm
     * ════════════════════════════════════════════════════════════ */

    /* Parent only needs the master end */
    close(slave_fd);

    p->shell_pid = pid;

    /*
     * Set O_NONBLOCK on the master fd.
     *
     * Without this, read(master_fd) would block (freeze the
     * render loop) whenever bash hasn't produced output yet.
     * With O_NONBLOCK, read() returns -1 with errno=EAGAIN
     * immediately when nothing is available, keeping the loop
     * running smoothly at full framerate.
     */
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }

    printf("PTY ready: shell PID=%d  master_fd=%d\n",
           p->shell_pid, p->master_fd);
    return 0;
}


/* ── pty_read ────────────────────────────────────────────────── */

int pty_read(PTY *p, char *buf, int bufsize) {
    int n = (int)read(p->master_fd, buf, (size_t)(bufsize - 1));
    if (n > 0) {
        buf[n] = '\0';
    }
    return n;
}


/* ── pty_write ───────────────────────────────────────────────── */

int pty_write(PTY *p, const char *buf, int len) {
    return (int)write(p->master_fd, buf, (size_t)len);
}


/* ── pty_resize ──────────────────────────────────────────────── */

void pty_resize(PTY *p, int cols, int rows) {
    p->cols = cols;
    p->rows = rows;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;

    /* TIOCSWINSZ = "Terminal IO Control Set WINdow SiZe" */
    ioctl(p->master_fd, TIOCSWINSZ, &ws);
}


/* ── pty_destroy ─────────────────────────────────────────────── */

void pty_destroy(PTY *p) {
    /*
     * SIGHUP = "hangup" — sent when a terminal disconnects.
     * bash responds by saving history and exiting cleanly.
     * waitpid() collects the exit status so bash doesn't
     * become a zombie process.
     */
    if (p->shell_pid > 0) {
        kill(p->shell_pid, SIGHUP);
        waitpid(p->shell_pid, NULL, 0);
        p->shell_pid = 0;
    }

    if (p->master_fd >= 0) {
        close(p->master_fd);
        p->master_fd = -1;
    }

    printf("PTY destroyed.\n");
}