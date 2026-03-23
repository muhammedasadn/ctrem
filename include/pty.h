#ifndef PTY_H
#define PTY_H

/*
 * pty.h — Pseudo-terminal interface for cterm.
 *
 * Declares the PTY struct and all functions for creating,
 * reading, writing, resizing and destroying a PTY session.
 */

/*
 * _GNU_SOURCE must be defined before ANY system header is
 * included — including indirect includes from other headers.
 * Defining it here in the guard block guarantees it is set
 * before pty.h pulls in anything else.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <unistd.h>    /* pid_t, read(), write(), close()  */
#include <sys/types.h> /* pid_t                            */

/*
 * PTY — everything needed to talk to one shell process.
 *
 * master_fd  — the file descriptor we read/write.
 *              A file descriptor is an integer handle the
 *              kernel uses for any open resource (file,
 *              socket, pipe, PTY). We read shell output
 *              and write keypresses through this fd.
 *
 * shell_pid  — the Process ID of the bash we spawned.
 *              Used to send SIGHUP when closing the tab
 *              and to wait for the process to exit cleanly.
 *
 * cols/rows  — current terminal grid dimensions.
 *              Kept in sync with the pane pixel size so
 *              that programs like vim draw at the right size.
 */
typedef struct {
    int   master_fd;   /* Our end of the PTY pair          */
    pid_t shell_pid;   /* PID of the child bash process    */
    int   cols;        /* Terminal width  in characters    */
    int   rows;        /* Terminal height in characters    */
} PTY;

/*
 * pty_init — open a PTY pair, fork, and exec bash.
 * Returns 0 on success, -1 on failure.
 */
int  pty_init(PTY *p, int cols, int rows);

/*
 * pty_read — read bytes bash has written to the PTY.
 * Non-blocking: returns 0 immediately if nothing available.
 * Returns number of bytes read, 0 if nothing, -1 on error.
 */
int  pty_read(PTY *p, char *buf, int bufsize);

/*
 * pty_write — send bytes to bash (keyboard input).
 * Returns number of bytes written, -1 on error.
 */
int  pty_write(PTY *p, const char *buf, int len);

/*
 * pty_resize — notify bash the terminal changed size.
 * Always call this after the pane rect changes.
 */
void pty_resize(PTY *p, int cols, int rows);

/*
 * pty_destroy — send SIGHUP to bash and close the master fd.
 * Always call before freeing the PTY struct.
 */
void pty_destroy(PTY *p);

#endif /* PTY_H */