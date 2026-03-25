#ifndef PTY_H
#define PTY_H

/*
 * pty.h — Pseudo-terminal interface.
 *
 * _GNU_SOURCE defined here in the header guard so every file
 * that includes pty.h gets it before any system header fires.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <unistd.h>
#include <sys/types.h>

typedef struct {
    int   master_fd;   /* Our end of the PTY                  */
    pid_t shell_pid;   /* PID of the bash child process       */
    int   cols;        /* Terminal width  in characters       */
    int   rows;        /* Terminal height in characters       */
} PTY;

int  pty_init(PTY *p, int cols, int rows);
int  pty_read(PTY *p, char *buf, int bufsize);
int  pty_write(PTY *p, const char *buf, int len);
void pty_resize(PTY *p, int cols, int rows);
void pty_destroy(PTY *p);

#endif /* PTY_H */