/*
 * pipeline.c - Multi-process log-processing pipeline using pipes (IPC).
 *
 *            pipe p1                 pipe p2
 *  reader  ----------->  filter  ----------->  summarizer --> report
 *    ^                     ^                       ^
 *    |      fork()         |       fork()          |  fork()
 *    +------------------ supervisor (parent) ------+
 *
 *  - reader     : reads the log file, validates each line, numbers it
 *                 with a sequence number and sends it down p1.
 *  - filter     : keeps "selected" events (by level / keyword) and marks
 *                 the rest as dropped, sends everything down p2.
 *  - summarizer : counts everything and prints the final report.
 *  - supervisor : creates the pipes and processes, waits for them with
 *                 waitpid(), logs their life cycle, and restarts the
 *                 pipeline from the last checkpoint if a stage fails.
 *
 * Wire format between stages: one text record per line, fields separated
 * by '|'. The message is always the LAST field, so a '|' inside a message
 * is harmless.
 *
 *   p1 (reader -> filter)      R|seq|timestamp|LEVEL|message   valid line
 *                              M|seq|reason                    malformed line
 *                              E|total_lines                   end of stream
 *   p2 (filter -> summarizer)  K|seq|timestamp|LEVEL|message   kept (selected)
 *                              D|seq|LEVEL                     dropped by filter
 *                              M|seq|reason                    passed through
 *                              E|total_lines                   end of stream
 *
 * Why an explicit E (end) record? Because EOF on a pipe only means "all
 * writers closed it" - that also happens when the writer CRASHES. A stage
 * treats its input as complete only if it saw E; plain EOF without E is
 * reported as an upstream failure (exit code EX_INCOMPLETE).
 *
 * Restart without loss or duplication (the "challenge"):
 *  - every input line gets a sequence number = its line number;
 *  - the summarizer accepts only seq == last_seq + 1 (a gap = lost data
 *    = protocol error) and ignores seq <= last_seq (duplicates);
 *  - every N records it atomically saves its counters + last_seq to a
 *    checkpoint file;
 *  - when any stage fails, the supervisor lets the others stop, then
 *    starts a fresh pipeline. The summarizer reloads the checkpoint and
 *    the reader resumes at last_seq + 1. Records counted after the last
 *    checkpoint are thrown away together with the in-memory counters and
 *    counted again, so nothing is counted twice and nothing is skipped.
 */
#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---------- exit codes used by the stages ---------- */
#define EX_OK          0
#define EX_USAGE       1   /* bad arguments / cannot start            */
#define EX_IO          2   /* read/write error, including broken pipe */
#define EX_INCOMPLETE  3   /* input ended without the E (end) record  */
#define EX_PROTOCOL    4   /* malformed record or sequence gap        */
#define EX_OUTPUT      5   /* cannot write final report (not retried) */

enum stage { ST_READER = 0, ST_FILTER, ST_SUMMARIZER, NSTAGES, ST_NONE = -1 };
static const char *STAGE_NAME[NSTAGES]  = { "reader", "filter", "summarizer" };
static const char *STAGE_COMM[NSTAGES]  = { "lp-reader", "lp-filter", "lp-summarizer" };

#define GRACE_MS 2000   /* how long other stages get to stop after a failure */

struct options {
    const char       *input;
    const char       *output;        /* NULL => stdout */
    struct filter_cfg filter;
    int               max_restarts;
    long              ckpt_every;    /* checkpoint every N records */
    long              delay_us;      /* artificial per-record delay in filter */
    int               pipe_size;     /* requested kernel pipe buffer (bytes)   */
    int               work;          /* simulated CPU work per record per stage */
    int               crash_stage;   /* fault injection for testing */
    long              crash_seq;
    int               crash_always;  /* crash on every attempt, not only the first */
    int               quiet;
    char              ckpt_path[512];
};

static int                   g_attempt;
static int                   g_quiet;
static const char           *g_who = "supervisor";
static volatile sig_atomic_t g_stop;   /* set by SIGINT / SIGTERM in supervisor */

/* ------------------------------------------------------------------ */
/* Logging: one complete line per write() so lines from different      */
/* processes never get mixed together on the terminal.                 */
/* ------------------------------------------------------------------ */
static void vlog(int force, const char *fmt, va_list ap)
{
    if (g_quiet && !force)
        return;
    char ts[32], buf[1024];
    now_str(ts, sizeof ts);
    int n = snprintf(buf, sizeof buf, "[%s] [%-10s pid=%-6d ppid=%-6d] ",
                     ts, g_who, (int)getpid(), (int)getppid());
    n += vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    ssize_t w = write(STDERR_FILENO, buf, (size_t)n);
    (void)w;
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(0, fmt, ap);
    va_end(ap);
}

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(1, fmt, ap);
    va_end(ap);
}

/* Fault injection: lets the tests kill a stage in the middle of the data. */
static void maybe_crash(const struct options *o, enum stage st, long seq)
{
    if (o->crash_stage == (int)st && seq == o->crash_seq &&
        (g_attempt == 0 || o->crash_always)) {
        err("INJECTED CRASH at seq %ld (sending SIGKILL to myself)", seq);
        kill(getpid(), SIGKILL);
    }
}

/* Report a failed write. EPIPE means the next stage is gone. */
static int write_failed(void)
{
    if (errno == EPIPE)
        err("broken pipe: downstream stage has exited, stopping");
    else
        err("write error: %s", strerror(errno));
    return EX_IO;
}

/* Write a line plus '\n' (cheaper than fprintf("%s\n")). <0 on error. */
static int fwrite_line(FILE *out, const char *line, size_t maxlen)
{
    size_t len = strnlen(line, maxlen);
    if (fwrite(line, 1, len, out) != len || putc('\n', out) == EOF)
        return -1;
    return 0;
}

/*
 * Every record a stage writes ends with '\n'. If the last line read from a
 * pipe has no '\n', the writer was killed in the middle of writing it (its
 * stdio buffer was flushed only partly). That fragment is NOT bad data, it
 * is a sign of an upstream crash: we discard it and treat the stream as
 * incomplete, so the restart logic replays it from the checkpoint.
 */
static int partial_record(const char *line, ssize_t n)
{
    if (n > 0 && line[n - 1] == '\n')
        return 0;
    err("discarding partial record \"%.40s\" (writer died mid-write)", line);
    return 1;
}

/* Split "T|seq|rest" into its parts. Returns 0 on success. */
static int parse_header(char *line, char *type, long *seq, char **rest)
{
    if (line[0] == '\0' || line[1] != '|')
        return -1;
    *type = line[0];
    char *end;
    errno = 0;
    *seq = strtol(line + 2, &end, 10);
    if (errno || end == line + 2 || *seq < 0)
        return -1;
    if (*type == 'E')
        return *end == '\0' ? 0 : -1;
    if (*end != '|')
        return -1;
    *rest = end + 1;
    return 0;
}

/* ================================================================== */
/* Stage 1: reader                                                     */
/* ================================================================== */
static int run_reader(const struct options *o, long start_seq, int outfd)
{
    FILE *in = fopen(o->input, "r");
    if (!in) {
        err("cannot open %s: %s", o->input, strerror(errno));
        return EX_IO;
    }
    FILE *out = fdopen(outfd, "w");
    if (!out) {
        err("fdopen: %s", strerror(errno));
        fclose(in);
        return EX_IO;
    }
    setvbuf(out, NULL, _IOFBF, 64 * 1024);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long seq = 0, sent = 0;
    int rc = EX_OK;

    while ((n = getline(&line, &cap, in)) != -1) {
        seq++;
        if (seq < start_seq)        /* already accounted for before a restart */
            continue;
        maybe_crash(o, ST_READER, seq);

        size_t len = chomp(line, (size_t)n);
        struct record r;
        enum malformed m = parse_line(line, len, &r);
        busy_work(seq, o->work);
        int w = (m == MAL_NONE)
              ? fprintf(out, "R|%ld|%s|%s|%s\n", seq, r.ts, level_name(r.level), r.msg)
              : fprintf(out, "M|%ld|%s\n", seq, malformed_name(m));
        if (w < 0) {
            rc = write_failed();
            goto done;
        }
        sent++;
    }
    if (ferror(in)) {
        err("error reading %s", o->input);
        rc = EX_IO;
        goto done;
    }
    if (fprintf(out, "E|%ld\n", seq) < 0 || fflush(out) != 0) {
        rc = write_failed();
        goto done;
    }
    info("done: %ld lines in file, sent %ld records (starting at seq %ld)",
         seq, sent, start_seq);
done:
    free(line);
    fclose(in);
    if (fclose(out) != 0 && rc == EX_OK)
        rc = write_failed();
    return rc;
}

/* ================================================================== */
/* Stage 2: filter                                                     */
/* ================================================================== */
static int run_filter(const struct options *o, int infd, int outfd)
{
    FILE *in = fdopen(infd, "r");
    FILE *out = fdopen(outfd, "w");
    if (!in || !out) {
        err("fdopen: %s", strerror(errno));
        return EX_IO;
    }
    setvbuf(out, NULL, _IOFBF, 64 * 1024);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long kept = 0, dropped = 0, passed = 0;
    int got_end = 0, rc = EX_OK;

    while ((n = getline(&line, &cap, in)) != -1) {
        if (partial_record(line, n))
            break;                       /* reader died mid-write */
        chomp(line, (size_t)n);
        char type, *rest = NULL;
        long seq;
        if (parse_header(line, &type, &seq, &rest) != 0) {
            err("protocol error: bad record \"%.60s\"", line);
            rc = EX_PROTOCOL;
            goto done;
        }
        if (type == 'E') {
            if (fprintf(out, "E|%ld\n", seq) < 0) {
                rc = write_failed();
                goto done;
            }
            got_end = 1;
            break;
        }
        maybe_crash(o, ST_FILTER, seq);
        if (o->delay_us > 0)
            usleep((useconds_t)o->delay_us);
        busy_work(seq, o->work);

        int w;
        if (type == 'M') {
            w = fwrite_line(out, line, (size_t)n);  /* pass malformed through */
            passed++;
        } else if (type == 'R') {
            /* rest = "timestamp|LEVEL|message" */
            char *lv = rest + TS_LEN;
            char *bar = (strlen(rest) > TS_LEN && *lv == '|') ? strchr(lv + 1, '|') : NULL;
            int level = bar ? level_from_str(lv + 1, (size_t)(bar - lv - 1)) : -1;
            if (level < 0) {
                err("protocol error at seq %ld: bad R record", seq);
                rc = EX_PROTOCOL;
                goto done;
            }
            if (filter_match(&o->filter, (enum level)level, bar + 1)) {
                line[0] = 'K';                        /* forward unchanged, marked kept */
                w = fwrite_line(out, line, (size_t)n);
                kept++;
            } else {
                w = fprintf(out, "D|%ld|%s\n", seq, level_name((enum level)level));
                dropped++;
            }
        } else {
            err("protocol error at seq %ld: unknown record type '%c'", seq, type);
            rc = EX_PROTOCOL;
            goto done;
        }
        if (w < 0) {
            rc = write_failed();
            goto done;
        }
    }
    if (!got_end) {
        err("input pipe closed WITHOUT end marker -> reader failed; not finishing");
        rc = EX_INCOMPLETE;
        goto done;
    }
    if (fflush(out) != 0) {
        rc = write_failed();
        goto done;
    }
    info("done: kept %ld, dropped %ld, malformed passed on %ld", kept, dropped, passed);
done:
    free(line);
    fclose(in);
    if (fclose(out) != 0 && rc == EX_OK)
        rc = write_failed();
    return rc;
}

/* ================================================================== */
/* Stage 3: summarizer                                                 */
/* ================================================================== */
static int write_report(const struct options *o, const struct stats *s)
{
    if (!o->output) {
        print_report(stdout, s, &o->filter);
        return fflush(stdout) == 0 ? 0 : -1;
    }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", o->output);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    print_report(f, s, &o->filter);
    if (fclose(f) != 0)
        return -1;
    return rename(tmp, o->output);   /* atomic: never a half-written report */
}

static int run_summarizer(const struct options *o, int infd)
{
    FILE *in = fdopen(infd, "r");
    if (!in) {
        err("fdopen: %s", strerror(errno));
        return EX_IO;
    }
    struct stats st;
    if (stats_load(o->ckpt_path, &st) == 0)
        info("resumed from checkpoint: %ld lines already counted", st.last_seq);
    else
        stats_init(&st);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long since_ckpt = 0, dups = 0;
    int got_end = 0, rc = EX_OK;

    while ((n = getline(&line, &cap, in)) != -1) {
        if (partial_record(line, n))
            break;                       /* filter died mid-write */
        chomp(line, (size_t)n);
        char type, *rest = NULL;
        long seq;
        if (parse_header(line, &type, &seq, &rest) != 0) {
            err("protocol error: bad record \"%.60s\"", line);
            rc = EX_PROTOCOL;
            goto done;
        }
        if (type == 'E') {
            if (seq != st.last_seq) {
                err("END says %ld lines but %ld were accounted for", seq, st.last_seq);
                rc = EX_PROTOCOL;
                goto done;
            }
            got_end = 1;
            break;
        }
        maybe_crash(o, ST_SUMMARIZER, seq);

        if (seq <= st.last_seq) {       /* duplicate (already counted) */
            dups++;
            continue;
        }
        if (seq != st.last_seq + 1) {   /* gap => records were lost */
            err("sequence gap: expected %ld, got %ld (records lost)", st.last_seq + 1, seq);
            rc = EX_PROTOCOL;
            goto done;
        }
        busy_work(seq, o->work);

        if (type == 'M') {
            int m = malformed_from_str(rest);
            if (m < 0) {
                err("protocol error at seq %ld: unknown reason", seq);
                rc = EX_PROTOCOL;
                goto done;
            }
            stats_add_malformed(&st, (enum malformed)m);
        } else if (type == 'K') {
            char *lv = rest + TS_LEN;
            char *bar = (strlen(rest) > TS_LEN && *lv == '|') ? strchr(lv + 1, '|') : NULL;
            int level = bar ? level_from_str(lv + 1, (size_t)(bar - lv - 1)) : -1;
            if (level < 0) {
                err("protocol error at seq %ld: bad K record", seq);
                rc = EX_PROTOCOL;
                goto done;
            }
            *lv = '\0';                 /* terminate the timestamp */
            stats_add_valid(&st, (enum level)level, 1, rest);
        } else if (type == 'D') {
            int level = level_from_str(rest, strlen(rest));
            if (level < 0) {
                err("protocol error at seq %ld: bad D record", seq);
                rc = EX_PROTOCOL;
                goto done;
            }
            stats_add_valid(&st, (enum level)level, 0, NULL);
        } else {
            err("protocol error at seq %ld: unknown record type '%c'", seq, type);
            rc = EX_PROTOCOL;
            goto done;
        }
        st.last_seq = seq;

        if (++since_ckpt >= o->ckpt_every) {
            if (stats_save(o->ckpt_path, &st) != 0)
                err("warning: could not write checkpoint: %s", strerror(errno));
            since_ckpt = 0;
        }
    }

    if (!got_end) {
        /* Upstream died. Everything up to last_seq is correct and
         * contiguous, so save it: the restart then replays less. */
        stats_save(o->ckpt_path, &st);
        err("input pipe closed WITHOUT end marker -> upstream failed; "
            "saved checkpoint at seq %ld", st.last_seq);
        rc = EX_INCOMPLETE;
        goto done;
    }
    if (write_report(o, &st) != 0) {
        err("cannot write report: %s", strerror(errno));
        rc = EX_OUTPUT;
        goto done;
    }
    unlink(o->ckpt_path);
    info("done: report written (%ld lines, %ld duplicates ignored)", st.total, dups);
done:
    free(line);
    fclose(in);
    return rc;
}

/* ================================================================== */
/* Supervisor (parent process)                                         */
/* ================================================================== */
static void on_signal(int sig)
{
    g_stop = sig;
}

static pid_t spawn(enum stage st, const struct options *o, long start_seq,
                   int p1[2], int p2[2])
{
    fflush(stdout);   /* otherwise buffered output would be duplicated in the child */
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        err("fork failed for %s: %s", STAGE_NAME[st], strerror(errno));
        return -1;
    }
    if (pid > 0) {
        info("created %-10s -> pid %d", STAGE_NAME[st], (int)pid);
        return pid;
    }

    /* ----- child ----- */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);       /* get EPIPE from write() instead of dying */
    prctl(PR_SET_NAME, STAGE_COMM[st], 0, 0, 0);   /* name shown by ps / pstree */
    g_who = STAGE_NAME[st];

    /* Close every pipe end this stage does not use. This is essential:
     * if the reader kept p1[0] open, a dead filter would never cause
     * EPIPE; if the filter kept p1[1] open, it would never see EOF. */
    int rc;
    switch (st) {
    case ST_READER:
        close(p1[0]); close(p2[0]); close(p2[1]);
        rc = run_reader(o, start_seq, p1[1]);
        break;
    case ST_FILTER:
        close(p1[1]); close(p2[0]);
        rc = run_filter(o, p1[0], p2[1]);
        break;
    default:
        close(p1[0]); close(p1[1]); close(p2[1]);
        rc = run_summarizer(o, p2[0]);
        break;
    }
    exit(rc);
}

static void describe_exit(int idx, pid_t pid, int status, int *ok)
{
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        static const char *why[] = { "success", "usage error", "I/O error / broken pipe",
                                     "incomplete input", "protocol error", "output error" };
        *ok = (code == 0);
        if (code == 0)
            info("%-10s (pid %d) exited normally, status 0", STAGE_NAME[idx], (int)pid);
        else
            err("%-10s (pid %d) exited with status %d (%s)", STAGE_NAME[idx], (int)pid,
                code, code < 6 ? why[code] : "unknown");
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        *ok = 0;
        err("%-10s (pid %d) was TERMINATED BY SIGNAL %d (%s)%s", STAGE_NAME[idx],
            (int)pid, sig, strsignal(sig), WCOREDUMP(status) ? ", core dumped" : "");
    } else {
        *ok = 0;
    }
}

static long elapsed_ms(const struct timespec *a)
{
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}

/* Wait for all stages. Returns 0 if the summarizer finished successfully,
 * 1 if the run failed but can be retried, 2 if it failed fatally. */
static int supervise(pid_t pids[NSTAGES])
{
    int alive = 0, failed = 0, fatal = 0, summarizer_ok = 0;
    struct timespec fail_time;
    for (int i = 0; i < NSTAGES; i++)
        if (pids[i] > 0)
            alive++;

    while (alive > 0) {
        if (g_stop && !failed) {
            err("received signal %d, stopping all stages", (int)g_stop);
            for (int i = 0; i < NSTAGES; i++)
                if (pids[i] > 0)
                    kill(pids[i], SIGTERM);   /* only our own children */
            failed = fatal = 1;
            clock_gettime(CLOCK_MONOTONIC, &fail_time);
        }
        int status;
        pid_t w = waitpid(-1, &status, failed ? WNOHANG : 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;                       /* ECHILD: nothing left */
        }
        if (w == 0) {                    /* failure happened, others still running */
            if (elapsed_ms(&fail_time) > GRACE_MS) {
                for (int i = 0; i < NSTAGES; i++)
                    if (pids[i] > 0) {
                        /* pids[i] is our own un-reaped child, so this PID cannot
                         * have been reused by an unrelated process. */
                        err("%s (pid %d) did not stop in time, sending SIGKILL",
                            STAGE_NAME[i], (int)pids[i]);
                        kill(pids[i], SIGKILL);
                    }
            }
            struct timespec t = { 0, 10 * 1000 * 1000 };
            nanosleep(&t, NULL);
            continue;
        }
        int idx = -1;
        for (int i = 0; i < NSTAGES; i++)
            if (pids[i] == w)
                idx = i;
        if (idx < 0)
            continue;
        pids[idx] = 0;
        alive--;

        int ok;
        describe_exit(idx, w, status, &ok);
        if (idx == ST_SUMMARIZER && ok)
            summarizer_ok = 1;
        if (!ok) {
            if (idx == ST_SUMMARIZER && WIFEXITED(status) &&
                (WEXITSTATUS(status) == EX_OUTPUT || WEXITSTATUS(status) == EX_USAGE))
                fatal = 1;
            if (!failed) {
                failed = 1;
                clock_gettime(CLOCK_MONOTONIC, &fail_time);
                /* Other stages notice by themselves (EOF without END, or
                 * EPIPE). We give them GRACE_MS to stop cleanly. */
            }
        }
    }
    if (summarizer_ok)
        return 0;     /* report complete and verified, even if another stage complained */
    return fatal ? 2 : 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] LOGFILE\n"
        "  -l LEVELS   levels to select, comma separated or ALL (default ERROR,CRITICAL)\n"
        "  -k WORD     only select events whose message contains WORD\n"
        "  -o FILE     write report to FILE instead of stdout\n"
        "  -r N        max restarts after a stage failure (default 3)\n"
        "  -c N        checkpoint every N records (default 10000)\n"
        "  -w N        simulated CPU work per record in EACH stage (experiment, default 0)\n"
        "  -b BYTES    kernel pipe buffer size (default 1048576, 0 = system default 65536)\n"
        "  -d USEC     slow down the filter by USEC microseconds per record (demo)\n"
        "  -x ST:SEQ[:always]  inject a crash in stage ST (reader|filter|summarizer)\n"
        "              at sequence number SEQ (first attempt only, unless :always)\n"
        "  -q          quiet: only print errors and the report\n"
        "  -h          this help\n", prog);
}

static int parse_crash(const char *arg, struct options *o)
{
    char name[32];
    long seq;
    char extra[16] = "";
    if (sscanf(arg, "%31[^:]:%ld:%15s", name, &seq, extra) < 2 || seq < 1)
        return -1;
    for (int i = 0; i < NSTAGES; i++)
        if (strcmp(name, STAGE_NAME[i]) == 0) {
            o->crash_stage = i;
            o->crash_seq = seq;
            o->crash_always = strcmp(extra, "always") == 0;
            return 0;
        }
    return -1;
}

int main(int argc, char **argv)
{
    struct options o = {
        .max_restarts = 3, .ckpt_every = 10000, .pipe_size = 1 << 20, .crash_stage = ST_NONE,
    };
    parse_level_list("ERROR,CRITICAL", &o.filter.level_mask);

    int c;
    char *end;
    while ((c = getopt(argc, argv, "l:k:o:r:c:d:b:w:x:qh")) != -1) {
        switch (c) {
        case 'l':
            if (parse_level_list(optarg, &o.filter.level_mask) != 0) {
                fprintf(stderr, "invalid level list: %s\n", optarg);
                return EX_USAGE;
            }
            break;
        case 'k': o.filter.keyword = optarg; break;
        case 'o': o.output = optarg; break;
        case 'r':
            o.max_restarts = (int)strtol(optarg, &end, 10);
            if (*end || o.max_restarts < 0) { usage(argv[0]); return EX_USAGE; }
            break;
        case 'c':
            o.ckpt_every = strtol(optarg, &end, 10);
            if (*end || o.ckpt_every < 1) { usage(argv[0]); return EX_USAGE; }
            break;
        case 'd':
            o.delay_us = strtol(optarg, &end, 10);
            if (*end || o.delay_us < 0) { usage(argv[0]); return EX_USAGE; }
            break;
        case 'w':
            o.work = (int)strtol(optarg, &end, 10);
            if (*end || o.work < 0) { usage(argv[0]); return EX_USAGE; }
            break;
        case 'b':
            o.pipe_size = (int)strtol(optarg, &end, 10);
            if (*end || o.pipe_size < 0) { usage(argv[0]); return EX_USAGE; }
            break;
        case 'x':
            if (parse_crash(optarg, &o) != 0) {
                fprintf(stderr, "invalid -x value: %s\n", optarg);
                return EX_USAGE;
            }
            break;
        case 'q': o.quiet = g_quiet = 1; break;
        case 'h': usage(argv[0]); return EX_OK;
        default:  usage(argv[0]); return EX_USAGE;
        }
    }
    if (optind != argc - 1) {
        usage(argv[0]);
        return EX_USAGE;
    }
    o.input = argv[optind];

    /* Fail early with a clear message instead of starting 3 processes. */
    FILE *probe = fopen(o.input, "r");
    if (!probe) {
        fprintf(stderr, "error: cannot open %s: %s\n", o.input, strerror(errno));
        return EX_USAGE;
    }
    fclose(probe);

    const char *tmpdir = getenv("TMPDIR");
    snprintf(o.ckpt_path, sizeof o.ckpt_path, "%s/lp-%d.ckpt",
             tmpdir ? tmpdir : "/tmp", (int)getpid());
    unlink(o.ckpt_path);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;          /* no SA_RESTART: waitpid returns EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    info("supervisor started: input=%s, max restarts=%d, checkpoint every %ld records",
         o.input, o.max_restarts, o.ckpt_every);

    for (int attempt = 0;; attempt++) {
        g_attempt = attempt;
        struct stats ck;
        long start_seq = (stats_load(o.ckpt_path, &ck) == 0) ? ck.last_seq + 1 : 1;
        info("attempt %d: starting pipeline at seq %ld", attempt, start_seq);

        int p1[2], p2[2];
        if (pipe(p1) < 0) {
            err("pipe: %s", strerror(errno));
            return EX_IO;
        }
        if (pipe(p2) < 0) {
            err("pipe: %s", strerror(errno));
            close(p1[0]); close(p1[1]);
            return EX_IO;
        }

        /* Bigger pipe buffers => fewer blocking reads/writes and fewer
         * context switches between stages (Linux-specific, optional). */
        if (o.pipe_size > 0) {
            if (fcntl(p1[1], F_SETPIPE_SZ, o.pipe_size) < 0 ||
                fcntl(p2[1], F_SETPIPE_SZ, o.pipe_size) < 0)
                info("note: could not set pipe size to %d (%s), using default",
                     o.pipe_size, strerror(errno));
        }

        pid_t pids[NSTAGES] = { 0 };
        int spawn_failed = 0;
        for (int i = 0; i < NSTAGES && !spawn_failed; i++) {
            pids[i] = spawn((enum stage)i, &o, start_seq, p1, p2);
            if (pids[i] < 0) {
                pids[i] = 0;
                spawn_failed = 1;
            }
        }
        /* The parent uses none of the pipe ends. If it kept them open,
         * no stage would ever see EOF or EPIPE and the pipeline would hang. */
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);

        if (spawn_failed)
            for (int i = 0; i < NSTAGES; i++)
                if (pids[i] > 0)
                    kill(pids[i], SIGTERM);

        int result = supervise(pids);
        if (result == 0 && !spawn_failed) {
            info("pipeline finished successfully in %ld ms (%d restart%s)",
                 elapsed_ms(&t0), attempt, attempt == 1 ? "" : "s");
            unlink(o.ckpt_path);
            return EX_OK;
        }
        if (g_stop) {
            unlink(o.ckpt_path);
            return 128 + (int)g_stop;
        }
        if (result == 2 || spawn_failed) {
            err("fatal failure, not restarting");
            unlink(o.ckpt_path);
            return EX_IO;
        }
        if (attempt >= o.max_restarts) {
            err("giving up after %d restart%s", attempt, attempt == 1 ? "" : "s");
            unlink(o.ckpt_path);
            return EX_IO;
        }
        err("a stage failed -> restarting pipeline from last checkpoint");
    }
}
