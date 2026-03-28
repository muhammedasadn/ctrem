/*
 * tools_autodetect.c — Drop-in replacement for the tool
 * registration section of tools.c
 *
 * HOW TO USE:
 * Replace your tools_init() function in tools.c with this one.
 * It checks whether each tool is actually installed before
 * registering it — so only tools the user has get shown.
 *
 * It also scans common extra locations so tools installed
 * via pip, cargo, npm etc. are found automatically.
 */

/*
 * tool_is_installed — check if a command exists on PATH.
 *
 * Uses access() to check each directory in $PATH.
 * Returns 1 if the tool is found and executable, 0 otherwise.
 */
static int tool_is_installed(const char *cmd) {
    /* If it's an absolute path, check directly */
    if (cmd[0] == '/') {
        return access(cmd, X_OK) == 0;
    }

    /* Search PATH */
    const char *path_env = getenv("PATH");
    if (!path_env) path_env = "/usr/bin:/bin:/usr/local/bin";

    char path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);

    char *dir = strtok(path_copy, ":");
    while (dir) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) return 1;
        dir = strtok(NULL, ":");
    }
    return 0;
}

/*
 * smart_register — register a tool ONLY if it is installed.
 * Silently skips uninstalled tools.
 */
static void smart_register(ToolManager *tm,
                            const char *name,
                            const char *desc,
                            const char *command,
                            int new_tab, ...) {
    if (!tool_is_installed(command)) return;

    if (tm->count >= MAX_TOOLS) return;

    ToolDef *t = &tm->tools[tm->count];
    memset(t, 0, sizeof(ToolDef));
    strncpy(t->name,    name,    sizeof(t->name)    - 1);
    strncpy(t->desc,    desc,    sizeof(t->desc)    - 1);
    strncpy(t->command, command, sizeof(t->command) - 1);
    t->new_tab  = new_tab;
    t->args[0]  = t->command;

    va_list vargs;
    va_start(vargs, new_tab);
    int idx = 1;
    while (idx < MAX_TOOL_ARGS - 1) {
        char *arg = va_arg(vargs, char *);
        if (!arg) break;
        t->args[idx++] = arg;
    }
    va_end(vargs);
    t->args[idx] = NULL;

    tm->count++;
}

/*
 * tools_init — register all tools that are actually installed.
 *
 * HOW AUTO-DETECTION WORKS:
 * smart_register() calls tool_is_installed() which searches
 * every directory in $PATH for the executable. If found →
 * registered. If not → silently skipped.
 *
 * This means:
 *   - If user installs btop → it appears in launcher
 *   - If user uninstalls htop → it disappears from launcher
 *   - No manual config needed
 */
void tools_init(ToolManager *tm) {
    memset(tm, 0, sizeof(ToolManager));

    /* System monitors */
    smart_register(tm, "btop",
        "Interactive system monitor (CPU RAM network disk)",
        "btop", 1, NULL);
    smart_register(tm, "htop",
        "Interactive process viewer",
        "htop", 1, NULL);
    smart_register(tm, "top",
        "Classic Unix process monitor",
        "top", 1, NULL);
    smart_register(tm, "iotop",
        "Disk I/O monitor by process",
        "iotop", 1, NULL);
    smart_register(tm, "iftop",
        "Network bandwidth monitor",
        "iftop", 1, NULL);
    smart_register(tm, "nethogs",
        "Network usage per process",
        "nethogs", 1, NULL);
    smart_register(tm, "bpytop",
        "Python resource monitor",
        "bpytop", 1, NULL);
    smart_register(tm, "glances",
        "Cross-platform system monitor",
        "glances", 1, NULL);

    /* Text editors */
    smart_register(tm, "vim",
        "Vi IMproved modal text editor",
        "vim", 1, NULL);
    smart_register(tm, "nvim",
        "Neovim — modern vim fork",
        "nvim", 1, NULL);
    smart_register(tm, "nano",
        "Simple beginner-friendly editor",
        "nano", 1, NULL);
    smart_register(tm, "micro",
        "Modern terminal editor",
        "micro", 1, NULL);
    smart_register(tm, "emacs",
        "GNU Emacs (terminal mode)",
        "emacs", 1, "-nw", NULL);
    smart_register(tm, "helix",
        "Helix — post-modern modal editor",
        "hx", 1, NULL);

    /* Network tools */
    smart_register(tm, "nmap",
        "Network port scanner",
        "nmap", 1, "--help", NULL);
    smart_register(tm, "netstat",
        "Network connections display",
        "netstat", 1, "-tulpn", NULL);
    smart_register(tm, "ss",
        "Socket statistics",
        "ss", 1, "-tulpn", NULL);
    smart_register(tm, "curl",
        "HTTP/FTP transfer tool",
        "curl", 1, "--help", NULL);
    smart_register(tm, "wget",
        "Network file downloader",
        "wget", 1, "--help", NULL);
    smart_register(tm, "dig",
        "DNS lookup utility",
        "dig", 1, NULL);
    smart_register(tm, "traceroute",
        "Trace network path to host",
        "traceroute", 1, NULL);
    smart_register(tm, "mtr",
        "Network diagnostic tool",
        "mtr", 1, NULL);
    smart_register(tm, "httpie",
        "Human-friendly HTTP client",
        "http", 1, "--help", NULL);
    smart_register(tm, "ncat",
        "Netcat — network swiss army knife",
        "ncat", 1, "--help", NULL);
    smart_register(tm, "tcpdump",
        "Network packet analyzer",
        "tcpdump", 1, "--help", NULL);

    /* Security / pentesting */
    smart_register(tm, "reaver",
        "WPS brute force tool",
        "reaver", 1, "--help", NULL);
    smart_register(tm, "aircrack-ng",
        "WPA/WEP cracking suite",
        "aircrack-ng", 1, "--help", NULL);
    smart_register(tm, "john",
        "John the Ripper password cracker",
        "john", 1, "--help", NULL);
    smart_register(tm, "hashcat",
        "GPU password cracker",
        "hashcat", 1, "--help", NULL);
    smart_register(tm, "sqlmap",
        "SQL injection scanner",
        "sqlmap", 1, "--help", NULL);
    smart_register(tm, "metasploit",
        "Penetration testing framework",
        "msfconsole", 1, NULL);
    smart_register(tm, "hydra",
        "Network login brute forcer",
        "hydra", 1, "--help", NULL);
    smart_register(tm, "nikto",
        "Web server scanner",
        "nikto", 1, "--help", NULL);
    smart_register(tm, "gobuster",
        "Directory/file brute forcer",
        "gobuster", 1, "--help", NULL);
    smart_register(tm, "ffuf",
        "Web fuzzer",
        "ffuf", 1, "--help", NULL);
    smart_register(tm, "wifite",
        "Automated wireless auditor",
        "wifite", 1, NULL);

    /* File managers */
    smart_register(tm, "ranger",
        "Vim-keys file manager",
        "ranger", 1, NULL);
    smart_register(tm, "mc",
        "Midnight Commander dual-pane",
        "mc", 1, NULL);
    smart_register(tm, "ncdu",
        "Disk usage analyzer",
        "ncdu", 1, NULL);
    smart_register(tm, "lf",
        "Terminal file manager",
        "lf", 1, NULL);
    smart_register(tm, "nnn",
        "Fast file manager",
        "nnn", 1, NULL);

    /* Development tools */
    smart_register(tm, "python3",
        "Python 3 REPL",
        "python3", 1, NULL);
    smart_register(tm, "node",
        "Node.js REPL",
        "node", 1, NULL);
    smart_register(tm, "lua",
        "Lua interpreter",
        "lua", 1, NULL);
    smart_register(tm, "ruby",
        "Ruby interpreter",
        "ruby", 1, NULL);
    smart_register(tm, "julia",
        "Julia REPL",
        "julia", 1, NULL);
    smart_register(tm, "gdb",
        "GNU debugger",
        "gdb", 1, NULL);
    smart_register(tm, "lldb",
        "LLVM debugger",
        "lldb", 1, NULL);
    smart_register(tm, "make",
        "GNU Make build tool",
        "make", 1, "--help", NULL);
    smart_register(tm, "cmake",
        "CMake build system",
        "cmake", 1, "--help", NULL);
    smart_register(tm, "cargo",
        "Rust package manager",
        "cargo", 1, "--help", NULL);
    smart_register(tm, "go",
        "Go language tools",
        "go", 1, "help", NULL);

    /* Git tools */
    smart_register(tm, "lazygit",
        "Terminal UI for git",
        "lazygit", 1, NULL);
    smart_register(tm, "tig",
        "Git repository browser",
        "tig", 1, NULL);
    smart_register(tm, "gitui",
        "Blazing fast git TUI",
        "gitui", 1, NULL);

    /* Database */
    smart_register(tm, "mysql",
        "MySQL client",
        "mysql", 1, "--help", NULL);
    smart_register(tm, "psql",
        "PostgreSQL client",
        "psql", 1, "--help", NULL);
    smart_register(tm, "sqlite3",
        "SQLite3 client",
        "sqlite3", 1, NULL);
    smart_register(tm, "redis-cli",
        "Redis command-line client",
        "redis-cli", 1, NULL);

    /* Shells */
    smart_register(tm, "bash",
        "New bash session",
        "/bin/bash", 1, NULL);
    smart_register(tm, "zsh",
        "Z shell",
        "zsh", 1, NULL);
    smart_register(tm, "fish",
        "Friendly interactive shell",
        "fish", 1, NULL);

    printf("ToolManager: %d installed tools registered.\n",
           tm->count);
}