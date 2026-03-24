/*
 * pty_windows.c — Windows PTY implementation using ConPTY.
 *
 * ConPTY (Console Pseudoterminal) was added in Windows 10 1809.
 * It is Microsoft's equivalent of POSIX openpty().
 *
 * How it works:
 *   CreatePseudoConsole() → creates the ConPTY object
 *   CreatePipe()          → creates read/write pipe pairs
 *   STARTUPINFOEX         → attaches pipes to ConPTY
 *   CreateProcess()       → spawns cmd.exe with those pipes
 *
 * We then read from hPipeRead and write to hPipeWrite,
 * exactly like reading/writing a PTY master fd on Linux.
 */

#ifdef CTERM_WINDOWS

#include "pty.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Windows PTY extended state.
 * We store Windows-specific handles alongside the PTY struct.
 * On Windows, PTY.master_fd is unused — we use HANDLEs instead.
 */
typedef struct {
    HPCON             hPC;            /* ConPTY handle            */
    HANDLE            hPipeRead;      /* We read shell output here */
    HANDLE            hPipeWrite;     /* We write keystrokes here  */
    HANDLE            hProcess;       /* Shell process handle      */
    HANDLE            hThread;        /* Shell thread handle       */
    LPPROC_THREAD_ATTRIBUTE_LIST attrList; /* Startup attributes   */
} WinPTY;

/* One global WinPTY per PTY struct — keyed by PTY pointer */
/* For simplicity we support one PTY at a time on Windows  */
/* A production version would use a hash map               */
static WinPTY g_winpty;

int pty_init(PTY *p, int cols, int rows) {
    p->cols = cols;
    p->rows = rows;
    p->master_fd = -1;
    p->shell_pid = 0;

    memset(&g_winpty, 0, sizeof(WinPTY));

    /* ── Step 1: Create pipe pairs ── */
    /*
     * We need two pipe pairs:
     *   Pipe 1: shell writes output → we read it
     *   Pipe 2: we write input → shell reads it
     *
     * Each pipe has two ends: a read handle and a write handle.
     */
    HANDLE hPipeShellRead,  hPipeShellWrite;   /* shell's stdin  */
    HANDLE hPipeOutputRead, hPipeOutputWrite;  /* shell's stdout */

    if (!CreatePipe(&hPipeShellRead, &hPipeShellWrite, NULL, 0)) {
        fprintf(stderr, "pty_init: CreatePipe (input) failed\n");
        return -1;
    }
    if (!CreatePipe(&hPipeOutputRead, &hPipeOutputWrite, NULL, 0)) {
        fprintf(stderr, "pty_init: CreatePipe (output) failed\n");
        CloseHandle(hPipeShellRead);
        CloseHandle(hPipeShellWrite);
        return -1;
    }

    /* Store the ends we use */
    g_winpty.hPipeWrite = hPipeShellWrite;   /* we write → shell reads  */
    g_winpty.hPipeRead  = hPipeOutputRead;   /* we read ← shell writes  */

    /* ── Step 2: Create the ConPTY ── */
    COORD size = { (SHORT)cols, (SHORT)rows };
    HRESULT hr = CreatePseudoConsole(
        size,
        hPipeShellRead,    /* ConPTY reads from here (shell stdin)  */
        hPipeOutputWrite,  /* ConPTY writes to here (shell stdout)  */
        0,
        &g_winpty.hPC
    );

    /* These ends are now owned by ConPTY — close our copies */
    CloseHandle(hPipeShellRead);
    CloseHandle(hPipeOutputWrite);

    if (FAILED(hr)) {
        fprintf(stderr, "pty_init: CreatePseudoConsole failed "
                        "(hr=0x%lx). Requires Windows 10 1809+\n",
                (unsigned long)hr);
        return -1;
    }

    /* ── Step 3: Build STARTUPINFOEX with ConPTY attached ── */
    /*
     * STARTUPINFOEX extends STARTUPINFO with an attribute list.
     * The attribute list is how we attach ConPTY to the new process.
     * The sizing dance below is required by the Windows API.
     */
    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);
    g_winpty.attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)
                         HeapAlloc(GetProcessHeap(), 0, attrListSize);
    if (!g_winpty.attrList) {
        fprintf(stderr, "pty_init: HeapAlloc failed\n");
        ClosePseudoConsole(g_winpty.hPC);
        return -1;
    }

    if (!InitializeProcThreadAttributeList(
            g_winpty.attrList, 1, 0, &attrListSize)) {
        fprintf(stderr, "pty_init: InitializeProcThreadAttributeList"
                        " failed\n");
        return -1;
    }

    if (!UpdateProcThreadAttribute(
            g_winpty.attrList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            g_winpty.hPC, sizeof(HPCON),
            NULL, NULL)) {
        fprintf(stderr, "pty_init: UpdateProcThreadAttribute failed\n");
        return -1;
    }

    STARTUPINFOEX siEx = {0};
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEX);
    siEx.lpAttributeList = g_winpty.attrList;

    /* ── Step 4: Launch the shell ── */
    PROCESS_INFORMATION pi = {0};
    char cmd[] = "cmd.exe";   /* Windows shell */

    if (!CreateProcess(
            NULL, cmd, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT,
            NULL, NULL,
            &siEx.StartupInfo,
            &pi)) {
        fprintf(stderr, "pty_init: CreateProcess failed "
                        "(error=%lu)\n", GetLastError());
        return -1;
    }

    g_winpty.hProcess = pi.hProcess;
    g_winpty.hThread  = pi.hThread;
    p->shell_pid      = (int)pi.dwProcessId;

    printf("PTY ready: shell PID=%d (Windows ConPTY)\n",
           p->shell_pid);
    return 0;
}

int pty_read(PTY *p, char *buf, int bufsize) {
    (void)p;
    DWORD available = 0;
    /*
     * PeekNamedPipe checks if data is available without blocking.
     * This is the Windows equivalent of O_NONBLOCK on Linux.
     */
    if (!PeekNamedPipe(g_winpty.hPipeRead, NULL, 0,
                       NULL, &available, NULL)) {
        return -1;
    }
    if (available == 0) return 0;

    DWORD to_read = (DWORD)(bufsize - 1);
    if (available < to_read) to_read = available;

    DWORD bytes_read = 0;
    if (!ReadFile(g_winpty.hPipeRead, buf,
                  to_read, &bytes_read, NULL)) {
        return -1;
    }
    buf[bytes_read] = '\0';
    return (int)bytes_read;
}

int pty_write(PTY *p, const char *buf, int len) {
    (void)p;
    DWORD written = 0;
    WriteFile(g_winpty.hPipeWrite, buf, (DWORD)len, &written, NULL);
    return (int)written;
}

void pty_resize(PTY *p, int cols, int rows) {
    p->cols = cols;
    p->rows = rows;
    COORD size = { (SHORT)cols, (SHORT)rows };
    ResizePseudoConsole(g_winpty.hPC, size);
}

void pty_destroy(PTY *p) {
    (void)p;
    if (g_winpty.hProcess) {
        TerminateProcess(g_winpty.hProcess, 0);
        WaitForSingleObject(g_winpty.hProcess, 2000);
        CloseHandle(g_winpty.hProcess);
        CloseHandle(g_winpty.hThread);
    }
    if (g_winpty.hPC) {
        ClosePseudoConsole(g_winpty.hPC);
    }
    if (g_winpty.hPipeRead)  CloseHandle(g_winpty.hPipeRead);
    if (g_winpty.hPipeWrite) CloseHandle(g_winpty.hPipeWrite);
    if (g_winpty.attrList) {
        DeleteProcThreadAttributeList(g_winpty.attrList);
        HeapFree(GetProcessHeap(), 0, g_winpty.attrList);
    }
    printf("PTY destroyed (Windows).\n");
}

#endif /* CTERM_WINDOWS */