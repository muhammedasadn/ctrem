#define _GNU_SOURCE  /* For openpty() and other GNU extensions Decalre in linux */

#include "pty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>          /* openpty()                          */
#include <utmp.h>          /* struct winsize                      */
#include <sys/ioctl.h>    /* ioctl(), struct winsize            */
#include <sys/wait.h>     /* waitpid()                          */
#include <signal.h>       /* SIGHUP                             */

/*
 * pty_init — the heart of the terminal emulator.
 *
 * This function does three things:
 *   1. Opens a PTY pair (master + slave file descriptors)
 *   2. Forks a new process (creates a copy of our program)
 *   3. In the child: exec bash (replace child with bash)
 *   4. In the parent: store the master_fd, continue running
 *
 * "Fork and exec" is the Unix way to start new programs.
 * fork() splits the process into two identical copies.
 * exec() replaces the current process with a new program.
 */
int pty_init(PTY *p, int cols, int rows) {

    p->cols = cols;
    p->rows = rows;

    /*
     * struct winsize tells the kernel the terminal dimensions.
     * bash uses this to wrap long lines and format output.
     * Programs like vim, htop use it to draw their UI correctly.
     */
    struct winsize ws = {
        .ws_row    = rows,
        .ws_col    = cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    int master_fd, slave_fd;

    /*
     * openpty() — asks the kernel for a fresh PTY pair.
     * Fills master_fd and slave_fd with the two ends.
     * The last two NULLs are for terminal settings we
     * don't need to customize right now.
     */
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0) {
        perror("openpty failed");
        return -1;
    }

    p->master_fd = master_fd;

    /*
     * fork() creates a copy of the current process.
     * After fork(), TWO processes are running this same code.
     * fork() returns:
     *   0         → you are the CHILD process
     *   positive  → you are the PARENT, value is child's PID
     *   -1        → fork failed (rare, system is out of resources)
     */
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        return -1;
    }

    if (pid == 0) {
        /*
         * ===== CHILD PROCESS =====
         * This code only runs in the child.
         * Goal: become a new session, connect to the slave PTY,
         * then replace ourselves with bash.
         */

        /* Close the master end — child doesn't need it */
        close(master_fd);

        /*
         * setsid() creates a new "session".
         * A session is a group of processes with one controlling terminal.
         * This detaches the child from our terminal so it can have
         * its own (the PTY slave).
         */
        if (setsid() < 0) {
            perror("setsid failed");
            exit(1);
        }

        /*
         * TIOCSCTTY makes the slave PTY the controlling terminal
         * of our new session. This is what lets bash receive
         * signals like Ctrl+C (SIGINT) correctly.
         */
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            perror("ioctl TIOCSCTTY failed");
            exit(1);
        }

        /*
         * dup2(old_fd, new_fd) — duplicate a file descriptor.
         * Here we connect the three standard streams to the slave PTY:
         *   stdin  (fd 0) = slave PTY  → bash reads keyboard input from here
         *   stdout (fd 1) = slave PTY  → bash writes output here
         *   stderr (fd 2) = slave PTY  → bash writes errors here
         */
        dup2(slave_fd, STDIN_FILENO);   /* fd 0 */
        dup2(slave_fd, STDOUT_FILENO);  /* fd 1 */
        dup2(slave_fd, STDERR_FILENO);  /* fd 2 */

        /* Close the slave fd itself — we now have it as 0, 1, 2 */
        close(slave_fd);

        /*
         * Set the TERM environment variable.
         * Programs check $TERM to know what escape sequences they can use.
         * "xterm-256color" tells them: full color support, standard VT100.
         */
        setenv("TERM", "xterm-256color", 1);

        /*
         * execl() replaces this process with bash.
         * After execl(), this code no longer exists — bash takes over.
         * The arguments are: program path, arg0 (program name), NULL terminator.
         * "-bash" with a dash prefix tells bash to run as a login shell.
         */
        execl("/bin/bash", "-bash", NULL);

        /* execl only returns if it FAILED */
        perror("execl failed");
        exit(1);
    }

    /*
     * ===== PARENT PROCESS =====
     * If we reach here, we are the parent (cterm).
     * pid holds the child's (bash's) PID.
     */

    /* Close slave end — parent only needs the master */
    close(slave_fd);

    p->shell_pid = pid;

    /*
     * Make master_fd non-blocking.
     * Without this, read() on master_fd would BLOCK (freeze) if
     * there's no data yet. Non-blocking means read() returns
     * immediately with -1 if nothing is available, so our
     * render loop keeps running while waiting for bash output.
     *
     * F_GETFL = get current file flags
     * F_SETFL = set file flags
     * O_NONBLOCK = the non-blocking flag
     */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    printf("PTY created: shell PID=%d, master_fd=%d\n",
           p->shell_pid, p->master_fd);
    return 0;
}

/*
 * pty_read — read whatever bash has written to the PTY.
 * Returns number of bytes read, 0 if nothing available, -1 on error.
 */
int pty_read(PTY *p, char *buf, int bufsize) {
    int n = read(p->master_fd, buf, bufsize - 1);
    if (n > 0) {
        buf[n] = '\0';  /* Null-terminate so we can use it as a string */
    }
    return n;
}

/*
 * pty_write — send a keypress (or string) to bash.
 * Returns number of bytes written.
 */
int pty_write(PTY *p, const char *buf, int len) {
    return write(p->master_fd, buf, len);
}

/*
 * pty_resize — tell bash the window changed size.
 * Called when the user resizes the SDL2 window.
 * Without this, vim/htop/nano would draw in wrong dimensions.
 */
void pty_resize(PTY *p, int cols, int rows) {
    p->cols = cols;
    p->rows = rows;

    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols
    };

    /* TIOCSWINSZ = "Terminal I/O Control Set Window Size" */
    ioctl(p->master_fd, TIOCSWINSZ, &ws);
}

/*
 * pty_destroy — cleanly shut down the PTY and bash process.
 */
void pty_destroy(PTY *p) {
    /* Send SIGHUP to bash — "terminal disconnected, please exit" */
    if (p->shell_pid > 0) {
        kill(p->shell_pid, SIGHUP);

        /* Wait for bash to actually exit — avoids zombie processes */
        waitpid(p->shell_pid, NULL, 0);
    }

    if (p->master_fd >= 0) {
        close(p->master_fd);
    }

    printf("PTY destroyed.\n");
}