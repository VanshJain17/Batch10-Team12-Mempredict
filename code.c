/*
 * mp_ui.c - Terminal UI for MemPredict-C
 *
 * Reads the CSV written by the C sampler (time_s, live_bytes, ...) or runs a
 * built-in demo simulation and prints a short, plain-text leak report:
 * result, memory over time, growth rate and time until the limit.
 *
 * Build:  gcc -Wall -Wextra -O2 mp_ui.c -o mp_ui -lm
 * Run:    ./mp_ui data/run_leaky.csv
 *         ./mp_ui --demo server --leak 8 --seed 42
 *         ./mp_ui --demo cache --leak 0
 * Options: --limit MB (default 256)  --live (animate while "reading")
 *          --speed ms per frame with --live (default 40)  --no-color
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

#define WIN 20      /* samples per window                       */
#define STRIDE 5    /* window moves 5 samples at a time         */
#define K 6         /* consecutive leak windows needed to alarm */
#define THR 0.012   /* floor slope threshold, fraction per sec  */
#define DECAY 0.6   /* growth must not be fading out (warm-up)  */
#define WARM 1.0    /* ignore the first second (cache warm-up)  */
#define FLOORN 6    /* rolling-minimum width                    */
#define SPARK 60    /* sparkline width                          */

static int color = 1;
#define C(code) (color ? "\033[" code "m" : "")
#define RST C("0")

typedef struct { int end; double tEnd; int flag; double rel, frel, r2, fr2, inc, cv; } Win;

static double *T, *Y, *FL;
static int N = 0;
static Win *wins; static int nw = 0, alarmIdx = -1;

static void on_exit_sig(int s) { (void)s; printf("\033[?25h%s\n", RST); _exit(0); }

/* ---------- data sources ---------- */
static uint32_t rs;
static double rnd(void) {
    uint32_t t; rs += 0x6D2B79F5u; t = rs;
    t = (t ^ (t >> 15)) * (t | 1u);
    t = (t + (t ^ (t >> 7)) * (t | 61u)) ^ t;
    return (double)(t ^ (t >> 14)) / 4294967296.0;
}

static void simulate(const char *kind, double leak, unsigned seed, int ticks) {
    rs = seed; T = malloc(ticks * sizeof *T); Y = malloc(ticks * sizeof *Y);
    double leaked = 0;
    for (int i = 0; i < ticks; i++) {
        leaked += 150e3 * leak / 1000.0 * (0.7 + 0.6 * rnd());
        double base;
        if (!strcmp(kind, "server")) base = 150e3 * (i + 1 < 20 ? i + 1 : 20) * (0.995 + 0.01 * rnd());
        else if (!strcmp(kind, "cache")) base = 3e6 * (1 - exp(-i / 15.0)) + 25e3 * (rnd() - 0.5);
        else { int p = i % 100; double b = p < 20 ? p / 20.0 : p < 40 ? (40 - p) / 20.0 : 0;
               base = 2e6 + 1.4e6 * b + 25e3 * (rnd() - 0.5); }
        T[i] = i * 0.01; Y[i] = base + leaked;
    }
    N = ticks;
}

static int load_csv(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 0; }
    char line[512]; int it = -1, iy = -1, cap = 1024;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    char *tok = strtok(line, ",\r\n"); int col = 0;
    while (tok) { if (!strcmp(tok, "time_s")) it = col; if (!strcmp(tok, "live_bytes")) iy = col; tok = strtok(NULL, ",\r\n"); col++; }
    if (it < 0 || iy < 0) { fprintf(stderr, "CSV needs time_s and live_bytes columns\n"); fclose(f); return 0; }
    T = malloc(cap * sizeof *T); Y = malloc(cap * sizeof *Y);
    while (fgets(line, sizeof line, f)) {
        double tv = 0, yv = 0; int c = 0;
        for (char *p = strtok(line, ",\r\n"); p; p = strtok(NULL, ",\r\n"), c++) {
            if (c == it) tv = atof(p);
            if (c == iy) yv = atof(p);
        }
        if (N == cap) { cap *= 2; T = realloc(T, cap * sizeof *T); Y = realloc(Y, cap * sizeof *Y); }
        T[N] = tv; Y[N] = yv; N++;
    }
    fclose(f); return 1;
}

/* ---------- model ---------- */
static void linfit(const double *x, const double *v, int n, double *slope, double *r2, double *mean) {
    double sx = 0, sy = 0; for (int i = 0; i < n; i++) { sx += x[i]; sy += v[i]; }
    double mx = sx / n, my = sy / n, sxx = 0, sxy = 0, syy = 0;
    for (int i = 0; i < n; i++) { double dx = x[i] - mx, dy = v[i] - my; sxx += dx * dx; sxy += dx * dy; syy += dy * dy; }
    if (slope) *slope = sxx ? sxy / sxx : 0;
    if (r2) *r2 = (sxx && syy) ? sxy * sxy / (sxx * syy) : 0;
    if (mean) *mean = my;
}

static void analyze(void) {
    FL = malloc(N * sizeof *FL);
    for (int i = 0; i < N; i++) { double m = Y[i]; for (int j = i - FLOORN + 1; j < i; j++) if (j >= 0 && Y[j] < m) m = Y[j]; FL[i] = m; }
    wins = malloc((N / STRIDE + 2) * sizeof *wins);
    for (int e = WIN - 1; e < N; e += STRIDE) {
        int s = e - WIN + 1, h = WIN / 2; double ls, lr2, mean, fs, fr2, f1, f2, dummy;
        linfit(T + s, Y + s, WIN, &ls, &lr2, &mean);
        linfit(T + s, FL + s, WIN, &fs, &fr2, &dummy);
        linfit(T + s, FL + s, h, &f1, NULL, NULL);
        linfit(T + s + h, FL + s + h, h, &f2, NULL, NULL);
        int inc = 0; double var = 0;
        for (int i = s + 1; i <= e; i++) if (Y[i] > Y[i - 1]) inc++;
        for (int i = s; i <= e; i++) var += (Y[i] - mean) * (Y[i] - mean);
        Win *w = &wins[nw++];
        w->end = e; w->tEnd = T[e]; w->rel = mean ? ls / mean : 0; w->frel = mean ? fs / mean : 0;
        w->r2 = lr2; w->fr2 = fr2; w->inc = (double)inc / (WIN - 1); w->cv = mean ? sqrt(var / WIN) / mean : 0;
        w->flag = T[s] >= WARM && w->frel > THR && fr2 > 0.5 && f2 >= DECAY * f1;
    }
    int run = 0;
    for (int i = 0; i < nw; i++) { run = wins[i].flag ? run + 1 : 0; if (run >= K) { alarmIdx = i; break; } }
}

/* ---------- simple report ---------- */
static void fmtB(char *o, double b) {
    double a = fabs(b);
    if (a >= 1e6) sprintf(o, "%.2f MB", b / 1e6); else if (a >= 1e3) sprintf(o, "%.1f KB", b / 1e3); else sprintf(o, "%.0f B", b);
}
static void fmtT(char *o, double s) {
    if (!isfinite(s)) strcpy(o, "-"); else if (s < 60) sprintf(o, "%.0f seconds", s);
    else if (s < 3600) sprintf(o, "%.1f minutes", s / 60); else if (s < 86400) sprintf(o, "%.1f hours", s / 3600);
    else sprintf(o, "%.1f days", s / 86400);
}

static void render(int k, double limit_mb, const char *note, int live) {
    char a[32], b[32]; int nshown = 0, run = 0;
    while (nshown < nw && wins[nshown].end < k) nshown++;
    for (int i = nshown - 1; i >= 0 && wins[i].flag; i--) run++;
    int alarmed = alarmIdx >= 0 && wins[alarmIdx].end < k;
    double ymax = 0; for (int i = 0; i < N; i++) if (Y[i] > ymax) ymax = Y[i];
    double now = Y[k - 1];

    if (live) printf("\033[H\033[J");
    printf("=============================================\n");
    printf("  MemPredict-C : Memory Leak Check\n");
    printf("=============================================\n");
    if (!nshown) {
        printf("  Result : COLLECTING DATA\n  Why    : still reading the first few samples\n");
    } else if (alarmed) {
        printf("  Result : %sLEAK LIKELY%s\n", C("1;31"), RST);
        printf("  Why    : memory kept growing in %d checks in a row\n", K);
        printf("  Alarm  : raised at %.2f s (%.0f%% of the run)\n", wins[alarmIdx].tEnd, wins[alarmIdx].tEnd / T[N - 1] * 100);
    } else if (run) {
        printf("  Result : %sWATCHING%s\n  Why    : memory grew in %d of %d checks, not sure yet\n", C("1;33"), RST, run, K);
    } else {
        printf("  Result : %sNORMAL%s\n  Why    : memory is steady, no sign of a leak\n", C("1;32"), RST);
    }
    printf("---------------------------------------------\n");
    printf("  Memory over time\n");
    for (int r = 0; r < 6; r++) {
        int idx = (int)((double)(k - 1) * r / 5.0);
        int n = ymax ? (int)(Y[idx] / ymax * 30 + 0.5) : 0;
        fmtB(a, Y[idx]);
        printf("  %4.1f s |", T[idx]);
        for (int i = 0; i < 30; i++) putchar(i < n ? '#' : ' ');
        printf("| %s\n", a);
    }
    printf("---------------------------------------------\n");
    fmtB(a, now);
    printf("  Memory now : %s\n", a);
    if (k >= WIN) {
        int m = k < 100 ? k : 100; double sl; linfit(T + k - m, FL + k - m, m, &sl, NULL, NULL);
        if (alarmed && sl > 1) {
            fmtB(a, sl); fmtT(b, (limit_mb * 1e6 - now) / sl);
            printf("  Growing by : %s every second\n", a);
            printf("  Will reach %.0f MB in about %s\n", limit_mb, b);
        } else printf("  Growing by : no steady growth found\n");
    }
    printf("=============================================\n");
    if (note) printf("  %s\n", note);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *csv = NULL, *kind = "server"; double leak = 8, limit = 256; unsigned seed = 42; int anim = 0, speed = 40;
    if (!isatty(1)) color = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo") && i + 1 < argc) kind = argv[++i];
        else if (!strcmp(argv[i], "--leak") && i + 1 < argc) leak = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atof(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--live")) anim = 1;
        else if (!strcmp(argv[i], "--no-color")) color = 0;
        else if (argv[i][0] != '-') csv = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }
    char truth[160] = "";
    if (csv) { if (!load_csv(csv)) return 1; snprintf(truth, sizeof truth, "Loaded %d samples from %s", N, csv); }
    else {
        if (strcmp(kind, "server") && strcmp(kind, "cache") && strcmp(kind, "bursty")) { fprintf(stderr, "workload must be server, cache or bursty\n"); return 1; }
        simulate(kind, leak, seed, 500);
        snprintf(truth, sizeof truth, "Ground truth: %s workload, %s", kind, leak > 0 ? "a leak was injected" : "no leak");
    }
    if (N < WIN + STRIDE) { fprintf(stderr, "need at least %d samples\n", WIN + STRIDE); return 1; }
    analyze();
    signal(SIGINT, on_exit_sig);
    if (anim) { printf("\033[?25l\033[2J"); for (int k = WIN; k < N; k += 8) { render(k, limit, NULL, 1); usleep(speed * 1000); } }
    render(N, limit, truth, anim);
    if (anim) printf("\033[?25h");
    return 0;
}
