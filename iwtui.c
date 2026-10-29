/* iwtui: terminal UI for iwd via iwctl; see README.md */

#define IWTUI_VERSION "0.1.0"

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define TB_IMPL
#define TB_OPT_ATTR_W 32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#include "termbox2.h"
#pragma GCC diagnostic pop

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define COL_BG       0x101318
#define COL_FG       0xEAEFF5
#define COL_CURSOR   "#E37B66"
#define COL_CURSOR_RGB 0xE37B66
#define COL_BLACK    0x222733
#define COL_RED      0xE37B66
#define COL_GREEN    0x8DA882
#define COL_YELLOW   0xE0A568
#define COL_CYAN     0x809D9E
#define COL_WHITE    0xC6CED8
#define COL_LBLACK   0x98A0AE
#define COL_LWHITE   0xEAEFF5

#define MAX_NETS 256
#define NAME_MAX_LEN 64
#define OUT_MAX (256 * 1024)
#define REFRESH_MS 4000
#define MAX_CONTENT_W 72
/* kill a stuck command after this many ms */
#define CMD_TIMEOUT_MS 30000
#define REFRESH_TIMEOUT_MS 8000
#define RC_TIMEOUT (-2)

struct net {
    char name[NAME_MAX_LEN];
    char security[16];
    int bars;
    int connected;
    int known;
};

static struct net nets[MAX_NETS];
static int nnets;
static int sel;
static int scroll_top;

static char device[64];
static char connected_name[NAME_MAX_LEN];
static char scanning[8] = "";
static char status[512];
static int status_is_error;
static int status_is_backend;

static const char *iwctl_path = "iwctl";

static long status_until;
static int busy_active;
static int spin_frame;
static const char *run_stdin;
static int cmd_timeout_ms = CMD_TIMEOUT_MS;
static int backend_down;

static void draw(void);

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

struct job {
    pid_t pid;
    int fd;
    char *buf;
    size_t len;
};

static void cloexec(int fd)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int job_start(struct job *j, char *const argv[], const char *input)
{
    int fds[2], inp[2] = {-1, -1};
    j->pid = -1;
    j->fd = -1;
    j->len = 0;
    j->buf = malloc(OUT_MAX + 1);
    if (!j->buf) return -1;
    if (pipe(fds) != 0) goto fail_buf;
    cloexec(fds[0]);
    cloexec(fds[1]);
    if (input) {
        if (pipe(inp) != 0) goto fail_pipe;
        cloexec(inp[0]);
        cloexec(inp[1]);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (inp[0] >= 0) { close(inp[0]); close(inp[1]); }
        goto fail_pipe;
    }
    if (pid == 0) {
        if (inp[0] >= 0) {
            dup2(inp[0], 0);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) dup2(devnull, 0);
        }
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    if (inp[0] >= 0) {
        close(inp[0]);
        const char *d = input;
        size_t dl = strlen(d);
        while (dl > 0) {
            ssize_t w = write(inp[1], d, dl);
            if (w <= 0) break;
            d += w;
            dl -= (size_t)w;
        }
        close(inp[1]);
    }
    j->pid = pid;
    j->fd = fds[0];
    return 0;

fail_pipe:
    close(fds[0]);
    close(fds[1]);
fail_buf:
    free(j->buf);
    j->buf = NULL;
    return -1;
}

/* reap pid, killing it if it ignores its closed output too long */
static int reap(pid_t pid, int *st)
{
    long deadline = now_ms() + 2000;
    for (;;) {
        pid_t r = waitpid(pid, st, WNOHANG);
        if (r == pid) return 1;
        if (r < 0 && errno != EINTR) return 0;
        if (now_ms() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, st, 0) < 0 && errno == EINTR) {}
            return 1;
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

/* run up to 4 commands at once; outs[i]/rcs[i] per command */
static void run_many(int n, char *const *argvs[], const char *input, char *outs[], int rcs[])
{
    struct job jobs[4];
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) job_start(&jobs[i], argvs[i], input);

    long deadline = now_ms() + cmd_timeout_ms;
    int timed_out = 0;
    for (;;) {
        struct pollfd pf[4];
        int idx[4], np = 0;
        for (int i = 0; i < n; i++)
            if (jobs[i].fd >= 0) {
                pf[np].fd = jobs[i].fd;
                pf[np].events = POLLIN;
                pf[np].revents = 0;
                idx[np++] = i;
            }
        if (!np) break;
        long left = deadline - now_ms();
        if (left <= 0) { timed_out = 1; break; }
        int pr = poll(pf, (nfds_t)np, (int)(left < 100 ? left : 100));
        if (pr < 0) {
            if (errno == EINTR) continue;
            timed_out = 1;
            break;
        }
        if (pr == 0) {
            if (busy_active) {
                spin_frame++;
                draw();
            }
            continue;
        }
        for (int k = 0; k < np; k++) {
            if (!pf[k].revents) continue;
            struct job *j = &jobs[idx[k]];
            ssize_t r = read(j->fd, j->buf + j->len, OUT_MAX - j->len);
            if (r < 0 && errno == EINTR) continue;
            if (r > 0) j->len += (size_t)r;
            if (r <= 0 || j->len >= OUT_MAX) {
                close(j->fd);
                j->fd = -1;
            }
        }
    }

    if (timed_out)
        for (int i = 0; i < n; i++)
            if (jobs[i].pid > 0) kill(jobs[i].pid, SIGKILL);

    for (int i = 0; i < n; i++) {
        struct job *j = &jobs[i];
        outs[i] = j->buf;
        rcs[i] = timed_out ? RC_TIMEOUT : -1;
        if (j->fd >= 0) close(j->fd);
        if (j->pid > 0) {
            int st = 0;
            if (reap(j->pid, &st))
                rcs[i] = timed_out ? RC_TIMEOUT : (WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        }
        if (j->buf) j->buf[j->len] = 0;
    }
}

static int run(char *const argv[], char **out)
{
    char *const *av[1] = {argv};
    int rc[1];
    run_many(1, av, run_stdin, out, rc);
    return rc[0];
}

static int has_prompt(const char *s, const char *want)
{
    for (; *s; s++) {
        size_t i = 0;
        while (want[i] && s[i] &&
               (s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]) == want[i]) i++;
        if (!want[i]) return 1;
    }
    return 0;
}

/* Run argv on a pty, typing secret at the prompt. */
static int run_typing(char *const argv[], const char *secret, const char *prompt, char **out)
{
    *out = NULL;
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0) { close(m); return -1; }
    const char *sn = ptsname(m);
    int s = sn ? open(sn, O_RDWR | O_NOCTTY) : -1;
    if (s < 0) { close(m); return -1; }
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        t.c_lflag &= (tcflag_t) ~(ECHO | ECHONL);
        tcsetattr(s, TCSANOW, &t);
    }
    cloexec(m);
    char *buf = malloc(OUT_MAX + 1);
    if (!buf) { close(m); close(s); return -1; }

    pid_t pid = fork();
    if (pid < 0) { close(m); close(s); free(buf); return -1; }
    if (pid == 0) {
        setsid();
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) close(s);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(s);

    size_t len = 0;
    int sent = 0, timed_out = 0;
    long deadline = now_ms() + cmd_timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) { timed_out = 1; break; }
        struct pollfd pfd = {m, POLLIN, 0};
        int pr = poll(&pfd, 1, (int)(left < 100 ? left : 100));
        if (pr == 0) {
            if (busy_active) {
                spin_frame++;
                draw();
            }
            continue;
        }
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ssize_t n = read(m, buf + len, OUT_MAX - len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = 0;
        if (!sent && has_prompt(buf, prompt)) {
            size_t sl = strlen(secret);
            if (write(m, secret, sl) < 0 || write(m, "\n", 1) < 0) break;
            sent = 1;
            len = 0;
        }
        if (len >= OUT_MAX) break;
    }
    buf[len] = 0;
    if (timed_out) kill(pid, SIGKILL);
    close(m);
    int st = 0;
    reap(pid, &st);
    *out = buf;
    if (timed_out) return RC_TIMEOUT;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void strip_ansi(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        if (in[i] == '\x1b' && in[i + 1] == '[') {
            i += 2;
            while (in[i] && !(in[i] >= '@' && in[i] <= '~')) i++;
            if (!in[i]) break;
            continue;
        }
        if (in[i] == '\r') continue;
        out[o++] = in[i];
    }
    out[o] = 0;
}

static int count_bars(const char *raw)
{
    int bars = 0, dim = 0;
    for (size_t i = 0; raw[i]; i++) {
        if (raw[i] == '\x1b' && raw[i + 1] == '[') {
            i += 2;
            int code = 0, have = 0;
            for (;; i++) {
                char c = raw[i];
                if (c >= '0' && c <= '9') {
                    code = code * 10 + (c - '0');
                    have = 1;
                    continue;
                }
                if (c == ';' || c == 'm') {
                    if (!have) code = 0;
                    if (code == 0 || code == 22 || code == 39) dim = 0;
                    else if (code == 2 || code == 90 || code == 30) dim = 1;
                    code = 0;
                    have = 0;
                    if (c == 'm') break;
                    continue;
                }
                if (!c) return bars;
                break;
            }
            continue;
        }
        if (raw[i] == '*' && !dim) bars++;
    }
    return bars > 4 ? 4 : bars;
}

typedef void (*row_cb)(const char *plain, const char *raw, void *ud);

static int is_dashes(const char *s)
{
    int n = 0;
    while (*s == ' ') s++;
    while (*s == '-') {
        s++;
        n++;
    }
    while (*s == ' ') s++;
    return n >= 8 && *s == 0;
}

static void each_row(char *raw_text, row_cb cb, void *ud)
{
    int dashes = 0;
    char *save = NULL;
    for (char *line = strtok_r(raw_text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char plain[1024];
        strip_ansi(line, plain, sizeof plain);
        if (is_dashes(plain)) {
            dashes++;
            continue;
        }
        if (dashes < 2) continue;
        const char *p = plain;
        while (*p == ' ') p++;
        if (!*p) continue;
        cb(plain, line, ud);
    }
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void first_column(const char *line, char *out, size_t cap, const char **rest)
{
    const char *p = line;
    size_t o = 0;
    while (*p && !(p[0] == ' ' && p[1] == ' ') && o + 1 < cap) out[o++] = *p++;
    out[o] = 0;
    trim(out);
    while (*p == ' ') p++;
    if (rest) *rest = p;
}

static void parse_network(const char *plain, const char *raw, void *ud)
{
    (void)ud;
    if (nnets >= MAX_NETS) return;
    struct net *n = &nets[nnets];
    memset(n, 0, sizeof *n);

    const char *p = plain;
    while (*p == ' ') p++;
    if (*p == '>') {
        n->connected = 1;
        p++;
        while (*p == ' ') p++;
    }

    size_t end = strlen(p);
    while (end && p[end - 1] == ' ') end--;
    while (end && p[end - 1] != ' ') end--;
    while (end && p[end - 1] == ' ') end--;
    size_t sec_end = end;
    while (end && p[end - 1] != ' ') end--;
    if (!sec_end || end == sec_end) return;
    char sec[16] = "";
    size_t sl_ = sec_end - end;
    if (sl_ >= sizeof sec) sl_ = sizeof sec - 1;
    memcpy(sec, p + end, sl_);
    sec[sl_] = 0;
    snprintf(n->security, sizeof n->security, "%s", sec);
    while (end && p[end - 1] == ' ') end--;
    size_t nl = end;
    if (nl >= sizeof n->name) nl = sizeof n->name - 1;
    memcpy(n->name, p, nl);
    n->name[nl] = 0;
    if (!n->name[0]) return;

    const char *last = raw + strlen(raw);
    while (last > raw && (last[-1] == ' ')) last--;
    const char *start = last;
    while (start > raw && start[-1] != ' ') start--;
    char sig[256];
    size_t sl = (size_t)(last - start);
    if (sl >= sizeof sig) sl = sizeof sig - 1;
    memcpy(sig, start, sl);
    sig[sl] = 0;
    n->bars = count_bars(sig);
    nnets++;
}

static void parse_known(const char *plain, const char *raw, void *ud)
{
    (void)raw;
    (void)ud;
    const char *p = plain;
    while (*p == ' ') p++;

    size_t best = 0;
    for (int i = 0; i < nnets; i++) {
        size_t l = strlen(nets[i].name);
        if (l >= best && !strncmp(p, nets[i].name, l) && (p[l] == 0 || (p[l] == ' ' && p[l + 1] == ' '))) {
            best = l;
        }
    }
    if (!best) return;
    for (int i = 0; i < nnets; i++)
        if (strlen(nets[i].name) == best && !strncmp(p, nets[i].name, best)) nets[i].known = 1;
}

static void parse_station_list(const char *plain, const char *raw, void *ud)
{
    (void)raw;
    char *dev = ud;
    if (dev[0]) return;
    const char *p = plain;
    while (*p == ' ') p++;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i + 1 < 64) {
        dev[i] = p[i];
        i++;
    }
    dev[i] = 0;
}

static void scopy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void parse_show(char *raw_text)
{
    char *save = NULL;
    connected_name[0] = 0;
    scanning[0] = 0;
    for (char *line = strtok_r(raw_text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char plain[512];
        strip_ansi(line, plain, sizeof plain);
        const char *p = plain;
        while (*p == ' ') p++;
        char key[64];
        const char *val;
        first_column(p, key, sizeof key, &val);
        if (!strcmp(key, "Connected network")) scopy(connected_name, sizeof connected_name, val);
        else if (!strcmp(key, "Scanning")) scopy(scanning, sizeof scanning, val);
    }
    trim(connected_name);
    trim(scanning);
}

enum inet_state { INET_UNKNOWN, INET_OK, INET_OFFLINE, INET_PORTAL, INET_NODNS };

static enum inet_state inet = INET_UNKNOWN;
static int check_enabled = 1;
static const char *check_host = "connectivitycheck.gstatic.com";
static pid_t check_pid = -1;
static int check_fd = -1;
static long check_started;
static long check_finished;
static char check_for[NAME_MAX_LEN];
static char check_last;
static int check_force;

#define CHECK_TIMEOUT_MS 10000
#define CHECK_OK_EVERY_MS 30000
#define CHECK_BAD_EVERY_MS 8000

static int connect_timeout(int fd, const struct sockaddr *sa, socklen_t len, int ms)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -1;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc;
    do {
        rc = connect(fd, sa, len);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0 && errno != EINPROGRESS) {
        fcntl(fd, F_SETFL, fl);
        return -1;
    }
    if (rc != 0) {
        struct pollfd p = {fd, POLLOUT, 0};
        int pr;
        do {
            pr = poll(&p, 1, ms);
        } while (pr < 0 && errno == EINTR);
        if (pr <= 0) {
            fcntl(fd, F_SETFL, fl);
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            fcntl(fd, F_SETFL, fl);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl);
    return 0;
}

static int tcp_reachable(const char *ip, int port)
{
    char service[8];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(ip, service, &hints, &ai) != 0) return 0;
    int ok = 0;
    for (struct addrinfo *a = ai; a && !ok; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        ok = connect_timeout(fd, a->ai_addr, a->ai_addrlen, 2500) == 0;
        close(fd);
    }
    freeaddrinfo(ai);
    return ok;
}

static void split_host_port(const char *in, char *host, size_t hcap, char *port, size_t pcap)
{
    const char *colon = NULL;
    if (in[0] == '[') {
        const char *rb = strchr(in, ']');
        if (rb) {
            size_t n = (size_t)(rb - in - 1);
            if (n >= hcap) n = hcap - 1;
            memcpy(host, in + 1, n);
            host[n] = 0;
            if (rb[1] == ':' && rb[2]) snprintf(port, pcap, "%s", rb + 2);
            else snprintf(port, pcap, "80");
            return;
        }
    }
    colon = strrchr(in, ':');
    if (colon && strchr(in, ':') == colon) {
        size_t n = (size_t)(colon - in);
        if (n >= hcap) n = hcap - 1;
        memcpy(host, in, n);
        host[n] = 0;
        snprintf(port, pcap, "%s", colon + 1);
    } else {
        snprintf(host, hcap, "%s", in);
        snprintf(port, pcap, "80");
    }
}

/* child writes r/n for raw reachability, then K/P/D/O */
static void connectivity_probe(int wfd)
{
    char b;
    int raw = tcp_reachable("1.1.1.1", 443) || tcp_reachable("9.9.9.9", 443);
    b = raw ? 'r' : 'n';
    if (write(wfd, &b, 1) < 0) return;

    char host[128], port[8];
    split_host_port(check_host, host, sizeof host, port, sizeof port);

    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    char verdict = raw ? 'D' : 'O';
    if (getaddrinfo(host, port, &hints, &ai) == 0) {
        verdict = 'O';
        for (struct addrinfo *a = ai; a; a = a->ai_next) {
            int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            if (connect_timeout(fd, a->ai_addr, a->ai_addrlen, 3000) == 0) {
                struct timeval tv = {4, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
                char req[320], resp[128], hosthdr[150];

                if (strchr(host, ':')) snprintf(hosthdr, sizeof hosthdr, "[%s]", host);
                else snprintf(hosthdr, sizeof hosthdr, "%s", host);
                int rl = snprintf(req, sizeof req,
                                  "GET /generate_204 HTTP/1.1\r\nHost: %s\r\n"
                                  "User-Agent: iwtui\r\nConnection: close\r\n\r\n", hosthdr);
                if (write(fd, req, (size_t)rl) == rl) {
                    size_t got = 0;
                    while (got < sizeof resp - 1) {
                        ssize_t n = read(fd, resp + got, sizeof resp - 1 - got);
                        if (n <= 0) break;
                        got += (size_t)n;
                        if (memchr(resp, '\n', got)) break;
                    }
                    resp[got] = 0;
                    if (!strncmp(resp, "HTTP/", 5)) {
                        const char *sp = strchr(resp, ' ');

                        /* 204 is the real internet; anything else is a portal. */
                        verdict = (sp && atoi(sp + 1) == 204) ? 'K' : 'P';
                    }
                }
                close(fd);
                if (verdict != 'O') break;
            } else {
                close(fd);
            }
        }
        freeaddrinfo(ai);
    }
    if (write(wfd, &verdict, 1) < 0) return;
}

static void check_start(void)
{
    if (!check_enabled || check_pid > 0 || !connected_name[0]) return;
    int fds[2];
    if (pipe(fds) != 0) return;
    cloexec(fds[0]);
    cloexec(fds[1]);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        close(fds[0]);
        connectivity_probe(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    check_pid = pid;
    check_fd = fds[0];
    check_started = now_ms();
    check_last = 0;
    snprintf(check_for, sizeof check_for, "%s", connected_name);
}

static void check_finish(enum inet_state result)
{
    if (check_pid > 0) {
        kill(check_pid, SIGKILL);
        while (waitpid(check_pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    if (check_fd >= 0) close(check_fd);
    check_pid = -1;
    check_fd = -1;
    check_finished = now_ms();
    if (!strcmp(check_for, connected_name)) inet = result;
}

static int check_poll(void)
{
    enum inet_state before = inet;
    if (!check_enabled) return 0;

    if (!connected_name[0]) {
        if (check_pid > 0) check_finish(INET_UNKNOWN);
        inet = INET_UNKNOWN;
        check_for[0] = 0;
        check_finished = 0;
        return before != inet;
    }
    if (check_pid > 0) {
        char c;
        ssize_t n;
        while ((n = read(check_fd, &c, 1)) == 1) check_last = c;
        int done = 1;
        enum inet_state r = INET_OFFLINE;
        switch (check_last) {
        case 'K': r = INET_OK; break;
        case 'P': r = INET_PORTAL; break;
        case 'D': r = INET_NODNS; break;
        case 'O': r = INET_OFFLINE; break;
        default: done = 0; break;
        }
        if (!done && n == 0) done = 1;
        if (!done && now_ms() - check_started > CHECK_TIMEOUT_MS) {

            r = check_last == 'r' ? INET_NODNS : INET_OFFLINE;
            done = 1;
        }
        if (done) check_finish(r);
        return before != inet;
    }

    int new_net = strcmp(check_for, connected_name) != 0;
    if (new_net) inet = INET_UNKNOWN;
    long every = inet == INET_OK ? CHECK_OK_EVERY_MS : CHECK_BAD_EVERY_MS;
    if (new_net || check_force || !check_finished || now_ms() - check_finished >= every) {
        check_force = 0;
        check_start();
    }
    return before != inet;
}

static void set_status(int is_error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(int is_error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_is_error = is_error;
    status_is_backend = 0;
    busy_active = 0;
    status_until = now_ms() + (is_error ? 6000 : 3500);
}

static void set_backend_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_backend_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_is_error = 1;
    status_is_backend = 1;
    busy_active = 0;
    status_until = 0;
}

static void first_line(const char *raw, char *out, size_t cap)
{
    char plain[2048];
    strip_ansi(raw, plain, sizeof plain);
    const char *p = plain;
    out[0] = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        int has_word = 0;
        for (size_t i = 0; i < ll; i++)
            if ((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= 'a' && p[i] <= 'z')) { has_word = 1; break; }
        if (has_word) {
            while (ll && *p == ' ') { p++; ll--; }
            if (ll >= cap) ll = cap - 1;
            memcpy(out, p, ll);
            out[ll] = 0;
            trim(out);
            return;
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static int report(const char *what, int rc, const char *out, const char *ok_msg)
{
    if (rc == 0) {
        if (ok_msg) set_status(0, "%s", ok_msg);
        return 1;
    }
    char line[200] = "";
    if (out) first_line(out, line, sizeof line);
    set_status(1, "%s: %s", what,
               line[0] ? line : rc == RC_TIMEOUT ? "iwctl did not respond" :
               (rc < 0 || rc == 127) ? "could not run iwctl" : "failed");
    return 0;
}

static int find_device(void)
{
    if (device[0]) return 0;
    char *out;
    char *argv[] = {(char *)iwctl_path, "station", "list", NULL};
    if (run(argv, &out) != 0 || !out) {
        free(out);
        return -1;
    }
    each_row(out, parse_station_list, device);
    free(out);
    return device[0] ? 0 : -1;
}

static long last_refresh;

static void refresh(void)
{
    char prev[NAME_MAX_LEN];
    snprintf(prev, sizeof prev, "%s", (nnets && sel < nnets) ? nets[sel].name : "");
    last_refresh = now_ms();
    int saved_timeout = cmd_timeout_ms;
    cmd_timeout_ms = REFRESH_TIMEOUT_MS;

    if (find_device() != 0) {
        backend_down = 1;
        set_backend_error("iwd is not running, or there is no wireless station (retrying)");
        nnets = 0;
        sel = 0;
        scroll_top = 0;
        connected_name[0] = 0;
        scanning[0] = 0;
        cmd_timeout_ms = saved_timeout;
        return;
    }

    char *show[] = {(char *)iwctl_path, "station", device, "show", NULL};
    char *get[] = {(char *)iwctl_path, "station", device, "get-networks", NULL};
    char *known[] = {(char *)iwctl_path, "known-networks", "list", NULL};
    char *const *argvs[3] = {show, get, known};
    char *outs[3];
    int rcs[3];
    run_many(3, argvs, NULL, outs, rcs);
    cmd_timeout_ms = saved_timeout;
    int ok = rcs[0] == 0 && rcs[1] == 0 && rcs[2] == 0;
    char err[200] = "";
    /* parse only successful commands; failures print error text */
    if (rcs[0] == 0 && outs[0]) parse_show(outs[0]);
    nnets = 0;
    if (rcs[1] == 0 && outs[1]) each_row(outs[1], parse_network, NULL);
    if (rcs[2] == 0 && outs[2]) each_row(outs[2], parse_known, NULL);
    for (int i = 0; i < 3; i++) {
        if (rcs[i] != 0 && !err[0] && outs[i]) first_line(outs[i], err, sizeof err);
        free(outs[i]);
    }

    if (prev[0])
        for (int i = 0; i < nnets; i++)
            if (!strcmp(nets[i].name, prev)) sel = i;
    if (sel >= nnets) sel = nnets ? nnets - 1 : 0;

    if (!ok) {
        int was_down = backend_down;
        backend_down = 1;

        if (!was_down || !status[0] || status_is_backend)
            set_backend_error("%s", err[0] ? err : "iwd is not responding (retrying)");
    } else if (backend_down) {
        backend_down = 0;
        if (status_is_backend) {
            status[0] = 0;
            status_is_backend = 0;
        }
    }
}

static void do_scan(void)
{
    char *out;
    char *argv[] = {(char *)iwctl_path, "station", device, "scan", NULL};
    int rc = run(argv, &out);
    report("scan", rc, out, "scanning...");
    free(out);
}

static void do_disconnect(void)
{
    char *out;
    char *argv[] = {(char *)iwctl_path, "station", device, "disconnect", NULL};
    int rc = run(argv, &out);
    report("disconnect", rc, out, "disconnected");
    free(out);
}

static void do_forget(const char *name)
{
    char *out;
    char *argv[] = {(char *)iwctl_path, "known-networks", (char *)name, "forget", NULL};
    int rc = run(argv, &out);
    char ok[NAME_MAX_LEN + 16];
    snprintf(ok, sizeof ok, "forgot %s", name);
    report("forget", rc, out, ok);
    free(out);
}

static void do_connect(const char *name, const char *pass, int hidden)
{
    char *out = NULL;
    int rc;
    const char *verb = hidden ? "connect-hidden" : "connect";
    int have_pass = pass && *pass;

    if (have_pass && !getenv("IWTUI_PASS_ARGV")) {

        char *argv[] = {(char *)iwctl_path, "station", device, (char *)verb, (char *)name, NULL};
        rc = run_typing(argv, pass, "passphrase:", &out);
    } else {
        char *argv[8];
        int a = 0;
        char pbuf[160];
        argv[a++] = (char *)iwctl_path;
        if (have_pass) {
            snprintf(pbuf, sizeof pbuf, "--passphrase=%s", pass);
            argv[a++] = pbuf;
        }
        argv[a++] = "station";
        argv[a++] = device;
        argv[a++] = (char *)verb;
        argv[a++] = (char *)name;
        argv[a] = NULL;
        rc = run(argv, &out);
        wipe(pbuf, sizeof pbuf);
    }

    char line[200] = "";
    if (out) first_line(out, line, sizeof line);
    if (rc != 0)
        set_status(1, "%s", line[0] ? line : rc == RC_TIMEOUT ? "iwctl did not respond" :
                   (rc < 0 || rc == 127) ? "could not run iwctl" : "connect failed");
    else if (line[0] && !have_pass)
        set_status(0, "%s", line);
    else
        set_status(0, "connected to %s", name);
    free(out);
    check_force = 1;
}

enum mode { MODE_LIST, MODE_PASS, MODE_HIDDEN_NAME, MODE_CONFIRM_FORGET, MODE_QR };
static enum mode mode = MODE_LIST;
static char input[128];
static int input_len;
static char pending_name[NAME_MAX_LEN];
static char pending_sec[16];
static int pending_hidden;
static int pending_qr;
static int pending_sudo;

/* QR: byte mode, ECC level M, versions 1-10. */
#define QR_MAXV 10
#define QR_MAXN (17 + 4 * QR_MAXV)

static const int qr_ecc_len[QR_MAXV + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const int qr_blocks[QR_MAXV + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
static const int qr_total_cw[QR_MAXV + 1] = {0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
static const int qr_align_n[QR_MAXV + 1] = {0, 0, 2, 2, 2, 2, 2, 3, 3, 3, 3};
static const int qr_align_pos[QR_MAXV + 1][3] = {
    {0}, {0}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50}};

static unsigned char qr_m[QR_MAXN][QR_MAXN];
static unsigned char qr_fn[QR_MAXN][QR_MAXN];
static int qr_n;
static char qr_name[NAME_MAX_LEN];

static void qr_clear(void)
{
    memset(qr_m, 0, sizeof qr_m);
    memset(qr_fn, 0, sizeof qr_fn);
    qr_n = 0;
}

static void qr_fnset(int x, int y, int dark)
{
    if (x < 0 || y < 0 || x >= qr_n || y >= qr_n) return;
    qr_m[y][x] = (unsigned char)(dark != 0);
    qr_fn[y][x] = 1;
}

static int qr_gf_mul(int x, int y)
{
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return z;
}

static void qr_rs(const unsigned char *data, int len, int deg, unsigned char *res)
{
    unsigned char gen[32];
    memset(gen, 0, sizeof gen);
    gen[deg - 1] = 1;
    int root = 1;
    for (int i = 0; i < deg; i++) {
        for (int j = 0; j < deg; j++) {
            gen[j] = (unsigned char)qr_gf_mul(gen[j], root);
            if (j + 1 < deg) gen[j] ^= gen[j + 1];
        }
        root = qr_gf_mul(root, 0x02);
    }
    memset(res, 0, (size_t)deg);
    for (int i = 0; i < len; i++) {
        int f = data[i] ^ res[0];
        memmove(res, res + 1, (size_t)deg - 1);
        res[deg - 1] = 0;
        for (int j = 0; j < deg; j++) res[j] ^= (unsigned char)qr_gf_mul(gen[j], f);
    }
}

static void qr_finder(int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            qr_fnset(cx + dx, cy + dy, d != 2 && d != 4);
        }
}

static void qr_align(int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            qr_fnset(cx + dx, cy + dy, (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) != 1);
}

static void qr_format(int mask)
{
    int data = mask;
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    int n = qr_n;
    for (int i = 0; i <= 5; i++) qr_fnset(8, i, (bits >> i) & 1);
    qr_fnset(8, 7, (bits >> 6) & 1);
    qr_fnset(8, 8, (bits >> 7) & 1);
    qr_fnset(7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) qr_fnset(14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++) qr_fnset(n - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) qr_fnset(8, n - 15 + i, (bits >> i) & 1);
    qr_fnset(8, n - 8, 1);
}

static void qr_version_bits(int ver)
{
    if (ver < 7) return;
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    int bits = (ver << 12) | rem;
    for (int i = 0; i < 18; i++) {
        int a = qr_n - 11 + i % 3, b = i / 3;
        qr_fnset(a, b, (bits >> i) & 1);
        qr_fnset(b, a, (bits >> i) & 1);
    }
}

static void qr_apply_mask(int mask)
{
    for (int y = 0; y < qr_n; y++)
        for (int x = 0; x < qr_n; x++) {
            int inv;
            switch (mask) {
            case 0: inv = (x + y) % 2 == 0; break;
            case 1: inv = y % 2 == 0; break;
            case 2: inv = x % 3 == 0; break;
            case 3: inv = (x + y) % 3 == 0; break;
            case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
            case 5: inv = x * y % 2 + x * y % 3 == 0; break;
            case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
            default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            if (inv && !qr_fn[y][x]) qr_m[y][x] ^= 1;
        }
}

static int qr_penalty(void)
{
    int n = qr_n, score = 0, dark = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int a = 0; a < n; a++) {
            int run = 1;
            unsigned bits = 0;
            for (int b = 0; b < n; b++) {
                int c = pass ? qr_m[b][a] : qr_m[a][b];
                if (b > 0) {
                    int prev = pass ? qr_m[b - 1][a] : qr_m[a][b - 1];
                    if (c == prev) {
                        run++;
                        if (run == 5) score += 3;
                        else if (run > 5) score += 1;
                    } else {
                        run = 1;
                    }
                }
                bits = ((bits << 1) | (unsigned)c) & 0x7FF;
                if (b >= 10 && (bits == 0x05D || bits == 0x5D0)) score += 40;
                if (!pass) dark += c;
            }
        }
    }
    for (int y = 0; y + 1 < n; y++)
        for (int x = 0; x + 1 < n; x++)
            if (qr_m[y][x] == qr_m[y][x + 1] && qr_m[y][x] == qr_m[y + 1][x] && qr_m[y][x] == qr_m[y + 1][x + 1])
                score += 3;
    int total = n * n;
    int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    return score + (k > 0 ? k * 10 : 0);
}

static int qr_encode(const char *text, int len)
{
    int ver = 0;
    for (int v = 1; v <= QR_MAXV; v++) {
        int cap = qr_total_cw[v] - qr_blocks[v] * qr_ecc_len[v];
        int cc = v <= 9 ? 8 : 16;
        if (4 + cc + 8 * len <= cap * 8) { ver = v; break; }
    }
    if (!ver) return -1;

    qr_clear();
    qr_n = 17 + 4 * ver;
    int n = qr_n;

    for (int i = 0; i < n; i++) {
        qr_fnset(6, i, i % 2 == 0);
        qr_fnset(i, 6, i % 2 == 0);
    }
    qr_finder(3, 3);
    qr_finder(n - 4, 3);
    qr_finder(3, n - 4);
    for (int i = 0; i < qr_align_n[ver]; i++)
        for (int j = 0; j < qr_align_n[ver]; j++) {
            int last = qr_align_n[ver] - 1;
            if ((i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0)) continue;
            qr_align(qr_align_pos[ver][i], qr_align_pos[ver][j]);
        }
    qr_format(0);
    qr_version_bits(ver);

    int ncw = qr_total_cw[ver], ecc = qr_ecc_len[ver], nb = qr_blocks[ver];
    int ndata = ncw - nb * ecc;
    unsigned char data[QR_MAXN * 8];
    memset(data, 0, sizeof data);
    int nbits = 0;
#define QR_PUT(val, cnt) \
    for (int i_ = (cnt) - 1; i_ >= 0; i_--, nbits++) data[nbits >> 3] |= (unsigned char)((((val) >> i_) & 1) << (7 - (nbits & 7)))
    QR_PUT(4, 4);
    QR_PUT(len, ver <= 9 ? 8 : 16);
    for (int i = 0; i < len; i++) QR_PUT((unsigned char)text[i], 8);
#undef QR_PUT
    int term = ndata * 8 - nbits;
    nbits += term < 4 ? term : 4;
    nbits = (nbits + 7) & ~7;
    for (int pad = 0xEC; nbits < ndata * 8; nbits += 8, pad ^= 0xEC ^ 0x11) data[nbits >> 3] = (unsigned char)pad;

    int short_blocks = nb - ncw % nb, short_len = ncw / nb;
    unsigned char out[QR_MAXN * 8];
    unsigned char eccbuf[8][32];
    const unsigned char *bdat[8];
    int blen[8], k = 0;
    for (int i = 0; i < nb; i++) {
        blen[i] = short_len - ecc + (i < short_blocks ? 0 : 1);
        bdat[i] = data + k;
        k += blen[i];
        qr_rs(bdat[i], blen[i], ecc, eccbuf[i]);
    }
    int o = 0;
    for (int i = 0; i < short_len - ecc + 1; i++)
        for (int j = 0; j < nb; j++)
            if (i < blen[j]) out[o++] = bdat[j][i];
    for (int i = 0; i < ecc; i++)
        for (int j = 0; j < nb; j++) out[o++] = eccbuf[j][i];

    int bit = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                int up = ((right + 1) & 2) == 0;
                int y = up ? n - 1 - vert : vert;
                if (!qr_fn[y][x] && bit < ncw * 8) {
                    qr_m[y][x] = (unsigned char)((out[bit >> 3] >> (7 - (bit & 7))) & 1);
                    bit++;
                }
            }
    }

    int best = 0, best_score = 1 << 30;
    for (int m = 0; m < 8; m++) {
        qr_apply_mask(m);
        qr_format(m);
        int sc = qr_penalty();
        if (sc < best_score) { best_score = sc; best = m; }
        qr_apply_mask(m);
    }
    qr_apply_mask(best);
    qr_format(best);
    return 0;
}

/* Escape the WIFI: format's reserved characters. */
static size_t wifi_esc(char *out, size_t o, size_t cap, const char *s)
{
    for (; *s && o + 3 < cap; s++) {
        if (strchr("\\;,\":", *s)) out[o++] = '\\';
        out[o++] = *s;
    }
    return o;
}

/* <state dir>/<ssid>.psk, hex if the SSID is not plain */
static void psk_path(const char *ssid, char *file, size_t cap)
{
    const char *dir = getenv("STATE_DIRECTORY");
    size_t o = (size_t)snprintf(file, cap, "%s/", dir && *dir ? dir : "/var/lib/iwd");
    int plain = 1;
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) &&
            *p != ' ' && *p != '-' && *p != '_') plain = 0;
    if (!plain && o + 1 < cap) file[o++] = '=';
    for (const unsigned char *p = (const unsigned char *)ssid; *p && o + 4 < cap; p++) {
        if (plain) file[o++] = (char)*p;
        else o += (size_t)snprintf(file + o, cap - o, "%02x", *p);
    }
    snprintf(file + o, cap - o, ".psk");
}

static int parse_psk_text(const char *text, char *out, size_t cap)
{
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        if (ll > 11 && !strncmp(p, "Passphrase=", 11)) {
            size_t vl = ll - 11;
            if (p[ll - 1] == '\r') vl--;
            if (vl == 0 || vl >= cap) return -1;
            memcpy(out, p + 11, vl);
            out[vl] = 0;
            return 0;
        }
        p = eol ? eol + 1 : NULL;
    }
    return -1;
}

/* 0 found, -1 no saved passphrase, -2 not allowed to read it. */
static int read_saved_pass(const char *ssid, char *out, size_t cap)
{
    char file[512], text[4096];
    psk_path(ssid, file, sizeof file);
    FILE *f = fopen(file, "r");
    if (!f) return (errno == EACCES || errno == EPERM) ? -2 : -1;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    int rc = parse_psk_text(text, out, cap);
    wipe(text, sizeof text);
    return rc;
}

enum { SUDO_OK = 0, SUDO_NEED_PW = 1, SUDO_NO_PASS = 2, SUDO_FAILED = 3, SUDO_DENIED = 4 };

static const char *priv_tool;

static int have_cmd(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char dir[512];
    for (const char *p = path; p;) {
        const char *colon = strchr(p, ':');
        size_t l = colon ? (size_t)(colon - p) : strlen(p);
        if (l && l < sizeof dir - 64) {
            memcpy(dir, p, l);
            snprintf(dir + l, sizeof dir - l, "/%s", name);
            if (access(dir, X_OK) == 0) return 1;
        }
        p = colon ? colon + 1 : NULL;
    }
    return 0;
}

static int in_group(const char *group)
{
    struct group *g = getgrnam(group);
    if (!g) return 0;
    gid_t gids[256];
    int n = getgroups(256, gids);
    for (int i = 0; i < n; i++)
        if (gids[i] == g->gr_gid) return 1;
    return getegid() == g->gr_gid;
}

static int doas_allows_me(void)
{
    static const char *files[] = {"/etc/doas.conf", "/usr/local/etc/doas.conf"};
    struct passwd *pw = getpwuid(getuid());
    int unreadable = 0, allowed = 0;
    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        if (!fp) {
            if (errno == EACCES) unreadable = 1;
            continue;
        }
        char line[512];
        while (fgets(line, sizeof line, fp)) {
            char *save = NULL, *tok = strtok_r(line, " \t\r\n", &save);
            if (!tok || tok[0] == '#' || strcmp(tok, "permit")) continue;
            while ((tok = strtok_r(NULL, " \t\r\n", &save))) {
                if (tok[0] == ':' ? in_group(tok + 1) : (pw && !strcmp(tok, pw->pw_name))) allowed = 1;
                if (!strcmp(tok, "as") || !strcmp(tok, "cmd")) break;
            }
        }
        fclose(fp);
    }
    return allowed ? 1 : unreadable ? -1 : 0;
}

static const char *priv_name(void)
{
    if (priv_tool) return priv_tool;
    const char *want = getenv("IWTUI_PRIV");
    int s = have_cmd("sudo"), d = have_cmd("doas");
    if (want && !strcmp(want, "sudo")) d = 0;
    if (want && !strcmp(want, "doas")) s = 0;
    if (s && d) {
        int da = doas_allows_me();
        int sa = in_group("sudo") || in_group("wheel") || in_group("admin");
        if (da == 1) s = 0;
        else if (sa) d = 0;
        else if (da == -1) s = 0;
        else d = 0;
    }
    priv_tool = d ? "doas" : s ? "sudo" : "";
    return priv_tool;
}

static int priv_read_pass(const char *ssid, const char *pw, char *out, size_t cap)
{
    char path[512], pwline[160], *text = NULL;
    const char *tool = priv_name();
    if (!*tool) return SUDO_DENIED;
    psk_path(ssid, path, sizeof path);
    int rc;
    if (!strcmp(tool, "sudo")) {
        char *with_pw[] = {"sudo", "-S", "-p", "", "--", "cat", "--", path, NULL};
        char *no_pw[] = {"sudo", "-n", "--", "cat", "--", path, NULL};
        pwline[0] = 0;
        if (pw) {
            snprintf(pwline, sizeof pwline, "%s\n", pw);
            run_stdin = pwline;
        }
        rc = run(pw ? with_pw : no_pw, &text);
        run_stdin = NULL;
        wipe(pwline, sizeof pwline);
    } else {

        char *with_pw[] = {"doas", "cat", "--", path, NULL};
        char *no_pw[] = {"doas", "-n", "cat", "--", path, NULL};
        rc = pw ? run_typing(with_pw, pw, "password:", &text) : run(no_pw, &text);
    }

    int res = SUDO_FAILED;
    if (text) {
        if (parse_psk_text(text, out, cap) == 0) res = SUDO_OK;
        else if (strstr(text, "not in the sudoers") || strstr(text, "may not run sudo") ||
                 strstr(text, "not permitted"))
            res = SUDO_DENIED;
        else if (rc == 0 || strstr(text, "No such file")) res = SUDO_NO_PASS;
        else if (strstr(text, "incorrect password") || strstr(text, "password is required") ||
                 strstr(text, "no password was provided") || strstr(text, "Authentication failed") ||
                 strstr(text, "Authorization required"))
            res = SUDO_NEED_PW;
        wipe(text, strlen(text));
        free(text);
    } else if (rc == 127) {
        res = SUDO_DENIED;
    }
    return res;
}

/* No passphrase needed: open, OWE or none. */
static int is_open_security(const char *sec)
{
    return !sec[0] || !strcasecmp(sec, "open") || !strcasecmp(sec, "owe") ||
           !strcasecmp(sec, "none");
}

static void show_qr(const char *ssid, const char *security, const char *pass)
{
    char payload[400];
    const char *type = is_open_security(security) ? "nopass" : !strcmp(security, "wep") ? "WEP" : "WPA";
    size_t o = (size_t)snprintf(payload, sizeof payload, "WIFI:T:%s;S:", type);
    o = wifi_esc(payload, o, sizeof payload, ssid);
    if (strcmp(type, "nopass") && pass && *pass) {
        o += (size_t)snprintf(payload + o, sizeof payload - o, ";P:");
        o = wifi_esc(payload, o, sizeof payload, pass);
    }
    if (o + 3 < sizeof payload) o += (size_t)snprintf(payload + o, sizeof payload - o, ";;");
    int rc = qr_encode(payload, (int)o);
    wipe(payload, sizeof payload);
    if (rc != 0) {
        set_status(1, "name and passphrase are too long for a QR code");
        return;
    }
    snprintf(qr_name, sizeof qr_name, "%s", ssid);
    status[0] = 0;
    status_until = 0;
    status_is_backend = 0;
    busy_active = 0;
    mode = MODE_QR;
}

static int put(int x, int y, uintattr_t fg, uintattr_t bg, const char *s, int maxw)
{
    int used = 0;
    while (*s && used < maxw) {
        uint32_t ch;
        int len = tb_utf8_char_to_unicode(&ch, s);
        if (len <= 0) break;
        int w = tb_wcwidth(ch);
        if (w < 0) w = 1;
        if (used + w > maxw) break;
        if (w > 0) tb_set_cell(x + used, y, ch, fg, bg);
        used += w;
        s += len;
    }
    return used;
}

static void fill(int x, int y, int w, uintattr_t fg, uintattr_t bg)
{
    for (int i = 0; i < w; i++) tb_set_cell(x + i, y, ' ', fg, bg);
}

static int reveal;
static int list_rows = 10;

static int text_width(const char *s)
{
    int used = 0;
    while (*s) {
        uint32_t ch;
        int len = tb_utf8_char_to_unicode(&ch, s);
        if (len <= 0) break;
        int w = tb_wcwidth(ch);
        used += w < 0 ? 1 : w;
        s += len;
    }
    return used;
}

static const char *tail_fit(const char *s, int w)
{
    int total = text_width(s);
    while (*s && total > w) {
        uint32_t ch;
        int len = tb_utf8_char_to_unicode(&ch, s);
        if (len <= 0) break;
        int cw = tb_wcwidth(ch);
        total -= cw < 0 ? 1 : cw;
        s += len;
    }
    return s;
}

static void put_centered(int y, uintattr_t fg, uintattr_t bg, const char *s, int maxw)
{
    int w = text_width(s);
    if (w > maxw) w = maxw;
    put((tb_width() - w) / 2, y, fg, bg, s, w);
}

static void draw_modal(const char *title, const char *label, const char *hint,
                       const char *text, int mask, uintattr_t label_fg)
{
    int W = tb_width(), H = tb_height();
    int bw = W < 54 ? W - 2 : 52;
    if (bw < 16) bw = W;
    int pad = bw >= 24 ? 3 : 1;
    int inner = bw - 2 * pad;

    int bh = text ? 9 : 6;
    int bx = (W - bw) / 2, by = (H - bh) / 2;
    if (by < 0) by = 0;

    for (int y = 0; y < bh; y++) fill(bx, by + y, bw, COL_FG, COL_BLACK);
    put(bx + pad, by + 1, COL_LWHITE | TB_BOLD, COL_BLACK, title, inner);
    int hw = text_width(hint);
    if (hw > inner) hw = inner;
    put(bx + (bw - hw) / 2, by + bh - 2, COL_LBLACK, COL_BLACK, hint, hw);

    if (text) {
        int ly = by + 2;
        if (label) put(bx + pad, ly, label_fg, COL_BLACK, label, inner);

        for (int y = 1; y <= 3; y++) fill(bx + pad, ly + y, inner, COL_FG, COL_BG);
        int fy = ly + 2, fx = bx + pad + 2, fw = inner - 4;
        int x = fx;
        if (mask && !reveal) {
            int n = 0;
            for (int i = 0; i < input_len; i++)
                if ((input[i] & 0xC0) != 0x80) n++;
            if (n > fw) n = fw;
            for (int i = 0; i < n; i++) put(x++, fy, COL_FG, COL_BG, "•", 1);
        } else {
            x += put(x, fy, COL_FG, COL_BG, tail_fit(text, fw), fw);
        }
        tb_set_cursor(x, fy);
    } else {
        tb_hide_cursor();
    }
}

static int qr_dark(int x, int y)
{
    return x >= 0 && y >= 0 && x < qr_n && y < qr_n && qr_m[y][x];
}

static void draw_qr(int top, int avail)
{
    int W = tb_width();
    int q = 3, total = qr_n + 2 * q, prow = (total + 1) / 2;
    if (prow > avail || total > W) {
        q = 2;
        total = qr_n + 2 * q;
        prow = (total + 1) / 2;
    }
    if (prow > avail || total > W) {
        char msg[96];
        snprintf(msg, sizeof msg, "terminal too small for the QR code (need %dx%d)", total, prow + 2);
        put_centered(top + avail / 2, COL_RED, COL_BG, msg, W - 2);
        return;
    }
    int x0 = (W - total) / 2, y0 = top + (avail - prow) / 2;
    for (int r = 0; r < prow; r++)
        for (int c = 0; c < total; c++) {
            uintattr_t up = qr_dark(c - q, 2 * r - q) ? COL_BG : COL_LWHITE;
            uintattr_t dn = qr_dark(c - q, 2 * r + 1 - q) ? COL_BG : COL_LWHITE;
            tb_set_cell(x0 + c, y0 + r, 0x2580, up, dn);
        }
}

static const char *inet_issue_label(void)
{
    switch (inet) {
    case INET_OFFLINE: return "no internet";
    case INET_PORTAL: return "sign-in page";
    case INET_NODNS: return "no DNS";
    default: return NULL;
    }
}

static void draw(void)
{
    int W = tb_width(), H = tb_height();
    tb_clear();
    tb_hide_cursor();

    int mx = W >= 40 ? 3 : 1;
    int cw = W - 2 * mx;
    if (cw > MAX_CONTENT_W) {
        mx += (cw - MAX_CONTENT_W) / 2;
        cw = MAX_CONTENT_W;
    }
    if (cw < 1) cw = 1;
    int xr = mx + cw;

    if (mode != MODE_QR) {
        int x = mx;
        x += put(x, 1, COL_LWHITE | TB_BOLD, COL_BG, "Wi-Fi", cw);
        if (device[0]) put(x + 2, 1, COL_LBLACK, COL_BG, device, cw - (x - mx) - 2);

        int is_scanning = !strcmp(scanning, "yes");
        char right[40] = "";
        uintattr_t rfg = COL_LBLACK;
        const char *issue = inet_issue_label();
        if (is_scanning) {
            snprintf(right, sizeof right, "scanning…");
            rfg = COL_YELLOW;
        } else if (issue && connected_name[0]) {
            snprintf(right, sizeof right, "%s", issue);
            rfg = COL_YELLOW;
        } else if (nnets) {
            snprintf(right, sizeof right, "%d network%s", nnets, nnets == 1 ? "" : "s");
        }
        int rw = text_width(right);
        if (rw && rw + 16 < cw) put(xr - rw, 1, rfg, COL_BG, right, rw);
    }

    int top = 3, rows = H - top - 2;
    if (rows < 1) rows = 1;
    list_rows = rows;

    if (mode == MODE_QR) {
        draw_qr(0, H - 2);
    } else if (mode == MODE_LIST) {
        if (sel < scroll_top) scroll_top = sel;
        if (sel >= scroll_top + rows) scroll_top = sel - rows + 1;
        if (scroll_top < 0) scroll_top = 0;

        int show_sec = cw >= 30, show_tag = cw >= 50;
        int bars_x = xr - 4, sec_x = 0, tag_x = 0, edge = bars_x;
        if (show_sec) { edge -= 7; sec_x = edge; }
        if (show_tag) { edge -= 14; tag_x = edge; }
        int namew = edge - 1 - (mx + 1);
        if (namew < 4) namew = 4;

        if (nnets == 0) {
            const char *empty = backend_down ? "iwd is not running. Waiting for it to come back\u2026"
                                             : "No networks found. Press s to scan.";
            put(mx, top, backend_down ? COL_YELLOW : COL_LBLACK, COL_BG, empty, cw);
        }

        for (int r = 0; r < rows && scroll_top + r < nnets; r++) {
            int i = scroll_top + r;
            struct net *n = &nets[i];
            int y = top + r;
            int on = (i == sel);
            uintattr_t bg = on ? COL_BLACK : COL_BG;
            if (on) fill(mx - 1, y, cw + 2, COL_FG, bg);
            const char *issue = n->connected ? inet_issue_label() : NULL;
            uintattr_t nfg = n->connected ? ((issue ? COL_YELLOW : COL_GREEN) | TB_BOLD)
                           : on ? (COL_LWHITE | TB_BOLD) : COL_FG;
            put(mx + 1, y, nfg, bg, n->name, namew);

            if (show_tag && n->connected && issue) put(tag_x, y, COL_YELLOW, bg, issue, 12);
            else if (show_tag && n->connected) put(tag_x, y, COL_GREEN, bg, "connected", 9);
            else if (show_tag && n->known) put(tag_x, y, COL_CYAN, bg, "saved", 5);
            if (show_sec) {
                int open = !strcmp(n->security, "open");
                put(sec_x, y, open ? COL_YELLOW : COL_LBLACK, bg, n->security, 5);
            }
            for (int b = 0; b < 4; b++) {
                static const char *glyph[] = {"▂", "▄", "▆", "█"};
                int lit = b < n->bars;
                uintattr_t fg = lit ? (n->bars >= 3 ? COL_GREEN : n->bars == 2 ? COL_YELLOW : COL_RED) : COL_LBLACK;
                put(bars_x + b, y, fg, bg, lit ? glyph[b] : "▁", 1);
            }
        }
    }

    int by = H - 1;
    if (status[0]) {
        static const char *spin[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        uintattr_t fg = status_is_error ? COL_RED : COL_WHITE;
        int w = text_width(status) + (busy_active ? 2 : 0);
        if (w > cw) w = cw;
        int sx = (W - w) / 2;
        if (busy_active) {
            put(sx, by, COL_YELLOW, COL_BG, spin[spin_frame % 10], 1);
            put(sx + 2, by, fg, COL_BG, status, w - 2);
        } else {
            put(sx, by, fg, COL_BG, status, w);
        }
    } else if (mode == MODE_QR) {
        char msg[160];
        snprintf(msg, sizeof msg, "scan to join %s   ·   any key to close", qr_name);
        put_centered(by, COL_LBLACK, COL_BG, msg, cw);
    } else if (mode == MODE_LIST) {
        struct { const char *key, *label; } hints[10];
        int nh = 0;
        if (nnets && !nets[sel].connected) hints[nh].key = "↵", hints[nh++].label = "connect";
        hints[nh].key = "s", hints[nh++].label = "scan";
        if (connected_name[0]) hints[nh].key = "d", hints[nh++].label = "disconnect";
        if (nnets && nets[sel].known) hints[nh].key = "f", hints[nh++].label = "forget";
        if (nnets) hints[nh].key = "p", hints[nh++].label = "qr";
        hints[nh].key = "h", hints[nh++].label = "hidden";
        hints[nh].key = "q", hints[nh++].label = "quit";
        hints[nh].key = "r", hints[nh++].label = "refresh";

        int fit = 0, total = 0;
        for (int i = 0; i < nh; i++) {
            int need = text_width(hints[i].key) + 1 + text_width(hints[i].label) + (i ? 3 : 0);
            if (total + need > cw) break;
            total += need;
            fit++;
        }
        int fx = (W - total) / 2;
        for (int i = 0; i < fit; i++) {
            if (i) fx += 3;
            fx += put(fx, by, COL_FG, COL_BG, hints[i].key, cw);
            fx += 1;
            fx += put(fx, by, COL_LBLACK, COL_BG, hints[i].label, cw);
        }
    }

    if (mode == MODE_PASS) {
        char title[128];
        snprintf(title, sizeof title, pending_qr ? "Share %s" : "%s", pending_name);
        const char *label = pending_hidden ? "Wi-Fi password (empty if open)" : "Wi-Fi password";
        char hint[96];
        uintattr_t label_fg = COL_CYAN;
        char plabel[40];
        if (pending_sudo) {
            snprintf(plabel, sizeof plabel, "Your %s password", priv_name());
            label = plabel;
            label_fg = COL_YELLOW;
        }

        if (pending_qr && (pending_sudo || *priv_name()))
            snprintf(hint, sizeof hint, "%s   Ctrl-W  %s password   esc cancel", reveal ? "tab hide" : "tab show",
                     pending_sudo ? "Wi-Fi" : priv_name());
        else
            snprintf(hint, sizeof hint, "\u21b5 ok   %s   esc cancel", reveal ? "tab hide" : "tab show");
        draw_modal(title, label, hint, input, 1, label_fg);
    } else if (mode == MODE_HIDDEN_NAME) {
        draw_modal("Hidden network", "Network name", "\u21b5 continue   esc cancel", input, 0, COL_CYAN);
    } else if (mode == MODE_CONFIRM_FORGET) {
        char title[96];
        snprintf(title, sizeof title, "Forget %s?", pending_name);
        draw_modal(title, NULL, "y forget   any other key cancels", NULL, 0, COL_FG);
    }
    tb_present();
}

static void move_sel(int d)
{
    sel += d;
    if (sel >= nnets) sel = nnets - 1;
    if (sel < 0) sel = 0;
}

static void start_share(void)
{
    if (!nnets) return;
    struct net *n = &nets[sel];
    if (!strcmp(n->security, "8021x")) {
        set_status(1, "802.1x networks can't be shared as a QR code");
        return;
    }
    snprintf(pending_name, sizeof pending_name, "%s", n->name);
    snprintf(pending_sec, sizeof pending_sec, "%s", n->security);
    if (is_open_security(n->security)) {
        show_qr(n->name, n->security, NULL);
        return;
    }
    char pass[128];
    int rc = read_saved_pass(n->name, pass, sizeof pass);
    int sr = SUDO_FAILED;
    if (rc == -2) {
        sr = priv_read_pass(n->name, NULL, pass, sizeof pass);
        if (sr == SUDO_OK) rc = 0;
    }
    if (rc == 0) {
        show_qr(n->name, n->security, pass);
        wipe(pass, sizeof pass);
        return;
    }
    input[0] = 0;
    input_len = 0;
    reveal = 0;
    pending_qr = 1;
    pending_sudo = (rc == -2 && sr == SUDO_NEED_PW);
    if (rc == -2 && sr == SUDO_DENIED) set_status(1, "no sudo or doas access, so type the Wi-Fi password instead");
    mode = MODE_PASS;
}

static void busy(const char *msg)
{
    set_status(0, "%s", msg);
    busy_active = 1;
    status_until = 0;
    spin_frame = 0;
    draw();
}

static void start_connect(void)
{
    if (!nnets) return;
    struct net *n = &nets[sel];
    if (n->connected) {
        set_status(0, "already connected to %s", n->name);
        return;
    }
    if (!strcmp(n->security, "8021x") && !n->known) {
        set_status(1, "%s uses 802.1x: it needs a provisioning file in /var/lib/iwd", n->name);
        return;
    }
    if (is_open_security(n->security) || n->known) {
        busy("connecting...");
        do_connect(n->name, NULL, 0);
        refresh();
        return;
    }
    snprintf(pending_name, sizeof pending_name, "%s", n->name);
    input[0] = 0;
    input_len = 0;
    reveal = 0;
    mode = MODE_PASS;
}

static int input_key(const struct tb_event *ev)
{

    if (ev->key == TB_KEY_ENTER) return 1;
    if (ev->key == TB_KEY_ESC) return -1;
    if (ev->key == TB_KEY_TAB) {
        if (mode == MODE_PASS) reveal = !reveal;
        return 0;
    }
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
        while (input_len > 0 && (input[input_len - 1] & 0xC0) == 0x80) input_len--;
        if (input_len > 0) input_len--;
        input[input_len] = 0;
        return 0;
    }
    if (ev->key == TB_KEY_CTRL_U) {
        input_len = 0;
        input[0] = 0;
        return 0;
    }
    if (ev->ch >= 32) {
        char buf[8];
        int n = tb_utf8_unicode_to_char(buf, ev->ch);

        /* SSID 32 bytes, passphrase 63, password up to the field */
        int cap = mode == MODE_HIDDEN_NAME ? 32
                : (mode == MODE_PASS && pending_sudo) ? (int)sizeof input - 1
                : 63;
        if (n > 0 && input_len + n <= cap) {
            memcpy(input + input_len, buf, (size_t)n);
            input_len += n;
            input[input_len] = 0;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (argc == 2 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
            printf("iwtui %s\n", IWTUI_VERSION);
            return 0;
        }
        fprintf(stderr, "usage: iwtui [-v | --version]\n");
        return 2;
    }

    const char *env = getenv("IWTUI_IWCTL");
    if (env && *env) iwctl_path = env;
    env = getenv("IWTUI_CHECK_HOST");
    if (env && *env) check_host = env;
    env = getenv("IWTUI_NO_CHECK");
    if (env && *env && strcmp(env, "0")) check_enabled = 0;

    signal(SIGPIPE, SIG_IGN);
    if (tb_init() != 0) {
        fprintf(stderr, "iwtui: cannot initialise the terminal\n");
        return 1;
    }
    tb_set_input_mode(TB_INPUT_ESC);
    tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    tb_set_clear_attrs(COL_FG, COL_BG);
    tb_sendf("\x1b]12;%s\x07", COL_CURSOR);

    refresh();
    check_poll();
    draw();

    for (;;) {
        struct tb_event ev;
        int rc = tb_peek_event(&ev, check_pid > 0 ? 150 : 500);
        if (rc == TB_OK && ev.type == TB_EVENT_KEY) {
            if (mode == MODE_LIST) {
                if (ev.ch == 'q' || ev.key == TB_KEY_CTRL_Q) break;
                else if (ev.ch == 'j' || ev.key == TB_KEY_ARROW_DOWN) { if (sel + 1 < nnets) sel++; }
                else if (ev.ch == 'k' || ev.key == TB_KEY_ARROW_UP) { if (sel > 0) sel--; }
                else if (ev.key == TB_KEY_PGDN) move_sel(list_rows);
                else if (ev.key == TB_KEY_PGUP) move_sel(-list_rows);
                else if (ev.key == TB_KEY_CTRL_D) move_sel(list_rows / 2);
                else if (ev.key == TB_KEY_CTRL_U) move_sel(-(list_rows / 2));
                else if (ev.ch == 'g' || ev.key == TB_KEY_HOME) sel = 0;
                else if (ev.ch == 'G' || ev.key == TB_KEY_END) sel = nnets ? nnets - 1 : 0;
                else if (ev.key == TB_KEY_ENTER) start_connect();
                else if (ev.ch == 'r') {
                    busy("refreshing...");
                    check_force = 1;
                    refresh();

                    if (busy_active) { busy_active = 0; status[0] = 0; }
                }
                else if (ev.ch == 's') { do_scan(); }
                else if (ev.ch == 'p') start_share();
                else if (ev.ch == 'd') { busy("disconnecting..."); do_disconnect(); refresh(); }
                else if (ev.ch == 'f' && nnets && nets[sel].known) {
                    snprintf(pending_name, sizeof pending_name, "%s", nets[sel].name);
                    mode = MODE_CONFIRM_FORGET;
                } else if (ev.ch == 'h') {
                    wipe(input, sizeof input);
                    input_len = 0;
                    reveal = 0;
                    mode = MODE_HIDDEN_NAME;
                }
            } else if (mode == MODE_PASS) {
                if (pending_qr && ev.key == TB_KEY_CTRL_W && (pending_sudo || *priv_name())) {

                    pending_sudo = !pending_sudo;
                    wipe(input, sizeof input);
                    input_len = 0;
                    reveal = 0;
                    status[0] = 0;
                    draw();
                    continue;
                }
                int k = input_key(&ev);
                if (k < 0) {
                    wipe(input, sizeof input);
                    input_len = 0;
                    reveal = 0;
                    mode = MODE_LIST;
                    pending_hidden = 0;
                    pending_qr = 0;
                    pending_sudo = 0;
                }
                else if (k > 0 && pending_sudo) {
                    if (input_len > 0) {
                        char pass[128];
                        busy("checking password...");
                        int sr = priv_read_pass(pending_name, input, pass, sizeof pass);
                        wipe(input, sizeof input);
                        input_len = 0;
                        if (sr == SUDO_OK) {
                            busy_active = 0;
                            status[0] = 0;
                            pending_sudo = 0;
                            pending_qr = 0;
                            mode = MODE_LIST;
                            show_qr(pending_name, pending_sec, pass);
                        } else if (sr == SUDO_NEED_PW) {
                            set_status(1, "wrong password, try again");
                        } else if (sr == SUDO_DENIED) {
                            pending_sudo = 0;
                            set_status(1, "%s isn't allowed for you, type the Wi-Fi password instead", priv_name());
                        } else {
                            pending_sudo = 0;
                            set_status(1, "no saved Wi-Fi password found, type it instead");
                        }
                        wipe(pass, sizeof pass);
                    }
                } else if (k > 0 && pending_qr) {
                    if (input_len > 0) {
                        pending_qr = 0;
                        mode = MODE_LIST;
                        show_qr(pending_name, pending_sec, input);
                        wipe(input, sizeof input);
                        input_len = 0;
                    }
                } else if (k > 0 && input_len > 0 && input_len < 8) {
                    set_status(1, "passphrase must be 8-63 characters");
                } else if (k > 0 && (input_len > 0 || pending_hidden)) {
                    mode = MODE_LIST;
                    busy("connecting...");
                    do_connect(pending_name, input, pending_hidden);
                    pending_hidden = 0;
                    wipe(input, sizeof input);
                    input_len = 0;
                    refresh();
                }
            } else if (mode == MODE_HIDDEN_NAME) {
                int k = input_key(&ev);
                if (k < 0) {
                    wipe(input, sizeof input);
                    input_len = 0;
                    reveal = 0;
                    mode = MODE_LIST;
                }
                else if (k > 0 && input_len > 0) {
                    snprintf(pending_name, sizeof pending_name, "%.32s", input);
                    input[0] = 0;
                    input_len = 0;
                    pending_hidden = 1;
                    reveal = 0;
                    mode = MODE_PASS;
                }
            } else if (mode == MODE_QR) {
                mode = MODE_LIST;
                qr_clear();
            } else if (mode == MODE_CONFIRM_FORGET) {
                mode = MODE_LIST;
                if (ev.ch == 'y') {
                    do_forget(pending_name);
                    refresh();
                }
            }
            draw();
        } else if (rc == TB_OK && ev.type == TB_EVENT_RESIZE) {
            draw();
        }

        if (status[0] && !busy_active && status_until && now_ms() >= status_until) {
            status[0] = 0;
            draw();
        }

        if (mode == MODE_LIST && now_ms() - last_refresh >= (backend_down ? 15000 : REFRESH_MS)) {
            refresh();
            draw();
        } else if (now_ms() - last_refresh >= REFRESH_MS) {
            last_refresh = now_ms();
        }
        if (check_poll()) draw();
    }

    if (check_pid > 0) check_finish(INET_UNKNOWN);
    tb_sendf("\x1b]112\x07");
    tb_present();
    tb_shutdown();
    return 0;
}
