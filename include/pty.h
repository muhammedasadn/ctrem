#ifndef PTY_H
#define PTY_H

#include <unistd.h>   /* pid_t, read(), write()   */

/*
 * PTY holds everything about our shell connection.
 *
 * master_fd — the file descriptor we read/write.
 *             A file descriptor is just an integer that the
 *             kernel uses to identify an open resource
 *             (file, socket, PTY, pipe — all are fds).
 *
 * shell_pid — the Process ID of the bash we spawned.
 *             We need this to kill bash when the user closes
 *             the tab, and to detect when bash exits.
 */
typedef struct {
    int   master_fd;   /* Our end of the PTY     */
    pid_t shell_pid;   /* PID of the bash child  */
    int   cols;        /* Terminal width in chars */
    int   rows;        /* Terminal height in chars*/
} PTY;

int  pty_init(PTY *p, int cols, int rows);
int  pty_read(PTY *p, char *buf, int bufsize);
int  pty_write(PTY *p, const char *buf, int len);
void pty_resize(PTY *p, int cols, int rows);
void pty_destroy(PTY *p);

#endif /* PTY_H */