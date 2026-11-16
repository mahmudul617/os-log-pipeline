/*
 * common.h - Shared definitions for the log-processing pipeline.
 *
 * Both the multi-process pipeline (pipeline.c) and the single-process
 * baseline (single.c) use these helpers, so the two programs parse,
 * filter and report in exactly the same way. That lets the tests check
 * that both versions produce byte-identical reports.
 *
 * Input log line format (one event per line):
 *     YYYY-MM-DD HH:MM:SS LEVEL message text...
 * Example:
 *     2026-11-15 10:23:45 ERROR Database connection failed
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stddef.h>

#define MAX_LINE_LEN 1024   /* longer input lines are treated as malformed */
#define TS_LEN       19     /* strlen("YYYY-MM-DD HH:MM:SS")                   */

/* ---------- severity levels ---------- */
enum level {
    LVL_DEBUG = 0,
    LVL_INFO,
    LVL_WARNING,
    LVL_ERROR,
    LVL_CRITICAL,
    NLEVELS
};

/* ---------- reasons a line can be malformed ---------- */
enum malformed {
    MAL_NONE = 0,         /* line is valid */
    MAL_EMPTY,            /* blank line */
    MAL_TOO_LONG,         /* longer than MAX_LINE_LEN */
    MAL_BAD_TIMESTAMP,    /* missing / invalid timestamp */
    MAL_BAD_LEVEL,        /* missing / unknown severity level */
    MAL_NO_MESSAGE,       /* nothing after the level */
    NMALFORMED
};

/* A parsed, valid log record. Pointers point INTO the caller's buffer. */
struct record {
    char        ts[TS_LEN + 1];
    enum level  level;
    const char *msg;
};

/* Which events the filter stage keeps ("selected events"). */
struct filter_cfg {
    unsigned    level_mask;   /* bit i set => keep level i      */
    const char *keyword;      /* NULL => no keyword requirement */
};

/* Running totals. Also the content of a checkpoint file. */
struct stats {
    long last_seq;                 /* highest sequence number accounted for */
    long total;                    /* lines seen                            */
    long valid;
    long malformed[NMALFORMED];
    long by_level[NLEVELS];
    long selected;                 /* kept by the filter                    */
    long filtered_out;             /* valid, but not selected               */
    char first_sel_ts[TS_LEN + 1]; /* time range of selected events         */
    char last_sel_ts[TS_LEN + 1];
};

/* ---------- functions (common.c) ---------- */
const char *level_name(enum level l);
int         level_from_str(const char *s, size_t n);      /* -1 if unknown */
const char *malformed_name(enum malformed m);
int         malformed_from_str(const char *s);            /* -1 if unknown */

/* Strip trailing "\n" / "\r\n" in place; returns new length. */
size_t chomp(char *line, size_t len);

/* Parse one (already chomped) line. Returns MAL_NONE and fills *out
 * on success, otherwise the reason the line is malformed. */
enum malformed parse_line(const char *line, size_t len, struct record *out);

int  parse_level_list(const char *list, unsigned *mask); /* "ERROR,CRITICAL" */
int  filter_match(const struct filter_cfg *cfg, enum level l, const char *msg);

void stats_init(struct stats *s);
void stats_add_malformed(struct stats *s, enum malformed m);
void stats_add_valid(struct stats *s, enum level l, int selected, const char *ts);

/* Checkpoint files: written atomically (temp file + rename). */
int  stats_save(const char *path, const struct stats *s);
int  stats_load(const char *path, struct stats *s);   /* 0 ok, -1 missing/bad */

void print_report(FILE *out, const struct stats *s, const struct filter_cfg *cfg);

/* Simulated per-stage processing cost (for experiments): iters * 64 hash
 * steps, the same amount for every record. 0 = no extra work. The result
 * goes to a volatile sink so the compiler cannot remove the loop. */
void busy_work(long seed, int iters);

/* "HH:MM:SS.mmm" wall-clock time for lifecycle logs. */
void now_str(char *buf, size_t n);

#endif /* COMMON_H */
