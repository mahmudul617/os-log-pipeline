/*
 * single.c - Single-process baseline: does the same work as the pipeline
 * (read -> validate -> filter -> summarize) in one loop, with no fork()
 * and no pipes. Used to check correctness (reports must be identical)
 * and to measure the overhead of interprocess communication.
 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-l LEVELS] [-k WORD] [-o FILE] LOGFILE\n"
        "  -l LEVELS  levels to select, comma separated or ALL (default ERROR,CRITICAL)\n"
        "  -k WORD    only select events whose message contains WORD\n"
        "  -o FILE    write report to FILE instead of stdout\n"
        "  -w N       simulated CPU work per record, done 3 times (once per\n"
        "             'stage') to match the pipeline's -w N (experiment)\n", prog);
}

int main(int argc, char **argv)
{
    struct filter_cfg cfg = { 0, NULL };
    const char *output = NULL;
    int work = 0;
    parse_level_list("ERROR,CRITICAL", &cfg.level_mask);

    int c;
    while ((c = getopt(argc, argv, "l:k:o:w:h")) != -1) {
        switch (c) {
        case 'l':
            if (parse_level_list(optarg, &cfg.level_mask) != 0) {
                fprintf(stderr, "invalid level list: %s\n", optarg);
                return 1;
            }
            break;
        case 'k': cfg.keyword = optarg; break;
        case 'o': output = optarg; break;
        case 'w': work = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (optind != argc - 1) {
        usage(argv[0]);
        return 1;
    }

    FILE *in = fopen(argv[optind], "r");
    if (!in) {
        fprintf(stderr, "error: cannot open %s: %s\n", argv[optind], strerror(errno));
        return 1;
    }

    struct stats st;
    stats_init(&st);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, in)) != -1) {
        size_t len = chomp(line, (size_t)n);
        struct record r;
        enum malformed m = parse_line(line, len, &r);
        long seq = st.last_seq + 1;
        busy_work(seq, work);        /* "reader" work     */
        busy_work(seq, work);        /* "filter" work     */
        busy_work(seq, work);        /* "summarizer" work */
        if (m != MAL_NONE)
            stats_add_malformed(&st, m);
        else
            stats_add_valid(&st, r.level, filter_match(&cfg, r.level, r.msg), r.ts);
        st.last_seq++;
    }
    int read_error = ferror(in);
    free(line);
    fclose(in);
    if (read_error) {
        fprintf(stderr, "error reading %s\n", argv[optind]);
        return 2;
    }

    FILE *out = output ? fopen(output, "w") : stdout;
    if (!out) {
        fprintf(stderr, "error: cannot write %s: %s\n", output, strerror(errno));
        return 1;
    }
    print_report(out, &st, &cfg);
    if ((output ? fclose(out) : fflush(out)) != 0) {
        fprintf(stderr, "error writing report\n");
        return 2;
    }
    return 0;
}
