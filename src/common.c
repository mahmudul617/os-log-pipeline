/*
 * common.c - Parsing, filtering, statistics and reporting shared by the
 * pipeline and single-process versions.
 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

static const char *LEVEL_NAMES[NLEVELS] = {
    "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"
};

static const char *MAL_NAMES[NMALFORMED] = {
    "none", "empty_line", "too_long", "bad_timestamp", "bad_level", "missing_message"
};

const char *level_name(enum level l)
{
    return (l >= 0 && l < NLEVELS) ? LEVEL_NAMES[l] : "?";
}

int level_from_str(const char *s, size_t n)
{
    for (int i = 0; i < NLEVELS; i++)
        if (strlen(LEVEL_NAMES[i]) == n && strncmp(s, LEVEL_NAMES[i], n) == 0)
            return i;
    return -1;
}

const char *malformed_name(enum malformed m)
{
    return (m >= 0 && m < NMALFORMED) ? MAL_NAMES[m] : "?";
}

int malformed_from_str(const char *s)
{
    for (int i = 1; i < NMALFORMED; i++)
        if (strcmp(s, MAL_NAMES[i]) == 0)
            return i;
    return -1;
}

size_t chomp(char *line, size_t len)
{
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    return len;
}

/* Read exactly `n` digits starting at s; returns -1 if any is not a digit. */
static int read_num(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i]))
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Validate "YYYY-MM-DD HH:MM:SS" (positions, digits and value ranges). */
static int valid_timestamp(const char *s)
{
    if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':')
        return 0;
    int y = read_num(s, 4), mo = read_num(s + 5, 2), d = read_num(s + 8, 2);
    int h = read_num(s + 11, 2), mi = read_num(s + 14, 2), se = read_num(s + 17, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59)
        return 0;
    return 1;
}

enum malformed parse_line(const char *line, size_t len, struct record *out)
{
    if (len == 0)
        return MAL_EMPTY;
    if (len > MAX_LINE_LEN)
        return MAL_TOO_LONG;
    if (len < TS_LEN || !valid_timestamp(line))
        return MAL_BAD_TIMESTAMP;

    const char *p = line + TS_LEN;
    if (*p != ' ')
        return MAL_BAD_LEVEL;            /* nothing after the timestamp */
    while (*p == ' ')
        p++;

    const char *lv = p;
    while (*p && *p != ' ')
        p++;
    int level = level_from_str(lv, (size_t)(p - lv));
    if (level < 0)
        return MAL_BAD_LEVEL;

    while (*p == ' ')
        p++;
    if (*p == '\0')
        return MAL_NO_MESSAGE;

    memcpy(out->ts, line, TS_LEN);
    out->ts[TS_LEN] = '\0';
    out->level = (enum level)level;
    out->msg = p;
    return MAL_NONE;
}

int parse_level_list(const char *list, unsigned *mask)
{
    *mask = 0;
    if (strcmp(list, "ALL") == 0) {
        *mask = (1u << NLEVELS) - 1;
        return 0;
    }
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        int l = level_from_str(p, n);
        if (l < 0)
            return -1;
        *mask |= 1u << l;
        p += n;
        if (*p == ',')
            p++;
    }
    return *mask ? 0 : -1;
}

int filter_match(const struct filter_cfg *cfg, enum level l, const char *msg)
{
    if (!(cfg->level_mask & (1u << l)))
        return 0;
    if (cfg->keyword && !strstr(msg, cfg->keyword))
        return 0;
    return 1;
}

void stats_init(struct stats *s)
{
    memset(s, 0, sizeof *s);
}

void stats_add_malformed(struct stats *s, enum malformed m)
{
    s->total++;
    s->malformed[m]++;
}

void stats_add_valid(struct stats *s, enum level l, int selected, const char *ts)
{
    s->total++;
    s->valid++;
    s->by_level[l]++;
    if (selected) {
        s->selected++;
        /* Timestamps in this format sort correctly as strings. */
        if (s->first_sel_ts[0] == '\0' || strcmp(ts, s->first_sel_ts) < 0)
            strcpy(s->first_sel_ts, ts);
        if (strcmp(ts, s->last_sel_ts) > 0)
            strcpy(s->last_sel_ts, ts);
    } else {
        s->filtered_out++;
    }
}

/*
 * Checkpoint format (text, one value per line, easy to inspect with cat):
 *   last_seq total valid m1..m5 l0..l4 selected filtered_out first_ts last_ts
 * Written to "<path>.tmp", then rename()d over <path>. rename() is atomic,
 * so a crashing PROCESS never leaves a half-written checkpoint. We do not
 * fsync(): that would also survive a power cut, but costs milliseconds per
 * call on a virtual disk, and our failure model is process crashes only.
 */
int stats_save(const char *path, const struct stats *s)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "%ld\n%ld\n%ld\n", s->last_seq, s->total, s->valid);
    for (int i = 1; i < NMALFORMED; i++)
        fprintf(f, "%ld\n", s->malformed[i]);
    for (int i = 0; i < NLEVELS; i++)
        fprintf(f, "%ld\n", s->by_level[i]);
    fprintf(f, "%ld\n%ld\n", s->selected, s->filtered_out);
    fprintf(f, "%s\n%s\n", s->first_sel_ts[0] ? s->first_sel_ts : "-",
            s->last_sel_ts[0] ? s->last_sel_ts : "-");
    if (fclose(f) != 0)
        return -1;
    return rename(tmp, path);
}

/* Read one line into buf and strip the newline; returns 0 on success. */
static int read_field(FILE *f, char *buf, size_t n)
{
    if (!fgets(buf, (int)n, f))
        return -1;
    chomp(buf, strlen(buf));
    return 0;
}

static int read_long(FILE *f, long *v)
{
    char buf[64], *end;
    if (read_field(f, buf, sizeof buf) != 0)
        return -1;
    errno = 0;
    *v = strtol(buf, &end, 10);
    return (errno || end == buf || *end) ? -1 : 0;
}

static int read_ts(FILE *f, char *dst)
{
    char buf[64];
    if (read_field(f, buf, sizeof buf) != 0)
        return -1;
    if (strcmp(buf, "-") == 0) {
        dst[0] = '\0';
        return 0;
    }
    if (strlen(buf) != TS_LEN)
        return -1;
    strcpy(dst, buf);
    return 0;
}

int stats_load(const char *path, struct stats *s)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    stats_init(s);
    int ok = read_long(f, &s->last_seq) == 0 && read_long(f, &s->total) == 0 &&
             read_long(f, &s->valid) == 0;
    for (int i = 1; ok && i < NMALFORMED; i++)
        ok = read_long(f, &s->malformed[i]) == 0;
    for (int i = 0; ok && i < NLEVELS; i++)
        ok = read_long(f, &s->by_level[i]) == 0;
    ok = ok && read_long(f, &s->selected) == 0 && read_long(f, &s->filtered_out) == 0 &&
         read_ts(f, s->first_sel_ts) == 0 && read_ts(f, s->last_sel_ts) == 0;
    fclose(f);
    if (!ok) {
        stats_init(s);
        return -1;
    }
    return 0;
}

void print_report(FILE *out, const struct stats *s, const struct filter_cfg *cfg)
{
    long mal_total = 0;
    for (int i = 1; i < NMALFORMED; i++)
        mal_total += s->malformed[i];

    fprintf(out, "================ LOG SUMMARY ================\n");
    fprintf(out, "Input lines         : %ld\n", s->total);
    fprintf(out, "Valid records       : %ld\n", s->valid);
    fprintf(out, "Malformed records   : %ld\n", mal_total);
    for (int i = 1; i < NMALFORMED; i++)
        fprintf(out, "  %-17s : %ld\n", malformed_name((enum malformed)i), s->malformed[i]);

    fprintf(out, "Records by level:\n");
    for (int i = 0; i < NLEVELS; i++)
        fprintf(out, "  %-17s : %ld\n", level_name((enum level)i), s->by_level[i]);

    fprintf(out, "Filter levels       : ");
    int first = 1;
    for (int i = 0; i < NLEVELS; i++)
        if (cfg->level_mask & (1u << i)) {
            fprintf(out, "%s%s", first ? "" : ",", level_name((enum level)i));
            first = 0;
        }
    fprintf(out, "\nFilter keyword      : %s\n", cfg->keyword ? cfg->keyword : "(none)");
    fprintf(out, "Selected events     : %ld\n", s->selected);
    fprintf(out, "Filtered out        : %ld\n", s->filtered_out);
    fprintf(out, "Selected time range : %s .. %s\n",
            s->first_sel_ts[0] ? s->first_sel_ts : "-",
            s->last_sel_ts[0] ? s->last_sel_ts : "-");

    int ok = (s->valid + mal_total == s->total) &&
             (s->selected + s->filtered_out == s->valid);
    fprintf(out, "Consistency check   : %s\n", ok ? "OK (every input line accounted for)"
                                                 : "FAILED");
    fprintf(out, "=============================================\n");
}

volatile unsigned long busy_sink;

void busy_work(long seed, int iters)
{
    unsigned long h = 1469598103934665603UL ^ (unsigned long)seed;  /* FNV-1a style */
    for (int i = 0; i < iters; i++)
        for (int j = 0; j < 64; j++)
            h = (h ^ (unsigned long)j) * 1099511628211UL;
    busy_sink += h;
}

void now_str(char *buf, size_t n)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    size_t k = strftime(buf, n, "%H:%M:%S", &tm);
    snprintf(buf + k, n - k, ".%03ld", (long)(tv.tv_usec / 1000));
}
