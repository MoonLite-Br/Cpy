/* main.c - command line driver: run files, -c code, stdin, and the REPL */
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "cpy.h"

static int g_argc;
static char **g_argvp;
static int g_exit_code;

static void usage(void) {
  puts("cpy " CPY_VERSION " - a small Python-like language\n"
       "\n"
       "usage: cpy [options] [script.cpy [args...]]\n"
       "       cpy -c \"code\"\n"
       "\n"
       "options:\n"
       "  -c CODE       run CODE and exit\n"
       "  -i            start the REPL even if stdin is not a terminal\n"
       "  -h, --help    show this help\n"
       "  -V, --version show version\n"
       "\n"
       "With no script (and a terminal on stdin) an interactive REPL starts.\n"
       "If stdin is a pipe, the program is read from it.\n"
       "Extra module search directories: set CPY_PATH=dir1:dir2");
}

static int run_guarded(const char *src, const char *file) {
  Handler h; h.prev = NULL; h.frame = NULL; h.depth = 0; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) {
    run_module_source(src, file, g_globals);
    g_handler = NULL;
    return 0;
  }
  g_handler = NULL; g_frame = NULL; g_depth = 0;
  report_uncaught(h.exc);
  return 1;
}

/* ------------------------------------------------------------- REPL */
static int bracket_depth(const char *s) {
  int d = 0; char q = 0;
  for (; *s; s++) {
    if (q) { if (*s == '\\' && s[1]) s++; else if (*s == q) q = 0; continue; }
    if (*s == '#') { while (*s && *s != '\n') s++; if (!*s) break; continue; }
    if (*s == '"' || *s == '\'') q = *s;
    else if (*s == '(' || *s == '[' || *s == '{') d++;
    else if (*s == ')' || *s == ']' || *s == '}') d--;
  }
  return d;
}
static int ends_with_colon(const char *s) {
  int n = (int)strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
  return n > 0 && s[n - 1] == ':';
}
static int is_blank(const char *s) { while (*s) { if (!isspace((unsigned char)*s)) return 0; s++; } return 1; }

static void repl_exec(const char *src) {
  Handler h; h.prev = NULL; h.frame = NULL; h.depth = 0; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) {
    int n; Node **prog = parse_program(src, "<stdin>", &n);
    Frame fr; fr.prev = NULL; fr.fn = NULL; fr.env = g_globals; fr.file = "<stdin>"; fr.name = "<module>"; fr.line = 0; fr.exc = V_none();
    g_frame = &fr;
    for (int i = 0; i < n; i++) {
      g_line = prog[i]->line;
      if (prog[i]->k == N_EXPR) {
        Value v = eval(prog[i]->a, g_globals);
        if (v.t != T_NONE) { Str *r = val_repr(v); printf("%s\n", r->s); odec(r); }
        decref(v);
      } else exec_block(&prog[i], 1, g_globals);
    }
    g_frame = NULL; g_handler = NULL;
    return;
  }
  g_handler = NULL; g_frame = NULL; g_depth = 0;
  report_uncaught(h.exc);
}

static int repl(void) {
  printf("cpy %s  (Ctrl-D or exit() to quit)\n", CPY_VERSION);
  char *line = NULL; size_t cap = 0;
  for (;;) {
    fputs(">>> ", stdout); fflush(stdout);
    if (getline(&line, &cap, stdin) < 0) { putchar('\n'); break; }
    Buf b; buf_init(&b); buf_adds(&b, line);
    if (is_blank(line)) { free(b.p); continue; }
    { char *t = line; while (isspace((unsigned char)*t)) t++;
      size_t tl = strlen(t); while (tl && isspace((unsigned char)t[tl - 1])) tl--;
      if ((tl == 4 && !strncmp(t, "exit", 4)) || (tl == 4 && !strncmp(t, "quit", 4))) { free(b.p); break; } }
    int block = ends_with_colon(line);
    int depth = bracket_depth(b.p);
    while (block || depth > 0) {
      fputs("... ", stdout); fflush(stdout);
      if (getline(&line, &cap, stdin) < 0) break;
      if (block && is_blank(line)) break;
      buf_adds(&b, line);
      depth = bracket_depth(b.p);
    }
    buf_adds(&b, "\n");
    repl_exec(b.p);
    free(b.p);
  }
  free(line);
  return 0;
}

/* ------------------------------------------------------------- main */
static void set_script_dir(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) { strcpy(g_script_dir, "."); return; }
  size_t n = (size_t)(slash - path);
  if (n == 0) n = 1;
  if (n >= 1024) n = 1023;
  memcpy(g_script_dir, path, n); g_script_dir[n] = 0;
}

static void *real_main(void *unused) {
  (void)unused;
  int argc = g_argc; char **argv = g_argvp;
  const char *code = NULL, *script = NULL; int i = 1, force_repl = 0;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return NULL; }
    if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) { puts("cpy " CPY_VERSION); return NULL; }
    if (!strcmp(argv[i], "-i")) { force_repl = 1; continue; }
    if (!strcmp(argv[i], "-c")) {
      if (i + 1 >= argc) { fputs("cpy: -c needs an argument\n", stderr); g_exit_code = 2; return NULL; }
      code = argv[++i]; i++; break;
    }
    if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "cpy: unknown option %s\n(try cpy --help)\n", argv[i]); g_exit_code = 2; return NULL; }
    script = argv[i]; break;
  }
  if (code) { char *av[1] = {"-c"}; set_sys_argv(1, av); }
  else if (script) set_sys_argv(argc - i, argv + i);
  else { char *av[1] = {""}; set_sys_argv(1, av); }
  interp_init();
  interp_setup_path(".");
  if (code) { g_exit_code = run_guarded(code, "<string>"); }
  else if (script) {
    long len; char *src = read_file(script, &len);
    if (!src) { fprintf(stderr, "cpy: cannot open '%s': %s\n", script, strerror(errno)); g_exit_code = 2; return NULL; }
    set_script_dir(script);
    interp_setup_path(g_script_dir);
    g_exit_code = run_guarded(src, xstrdup(script));
  } else if (force_repl || isatty(0)) g_exit_code = repl();
  else {
    Buf b; buf_init(&b); buf_add(&b, "", 0); char tmp[4096]; size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_add(&b, tmp, (int)k);
    g_exit_code = run_guarded(b.p, "<stdin>");
  }
  fflush(stdout);
  return NULL;
}

int main(int argc, char **argv) {
  g_argc = argc; g_argvp = argv;
  size_t sizes[] = {(size_t)512 << 20, (size_t)128 << 20, (size_t)32 << 20, 0};
  for (int i = 0; sizes[i]; i++) {
    pthread_attr_t at; pthread_t th;
    pthread_attr_init(&at);
    if (pthread_attr_setstacksize(&at, sizes[i]) != 0) continue;
    if (pthread_create(&th, &at, real_main, NULL) == 0) { pthread_join(th, NULL); return g_exit_code; }
  }
  real_main(NULL);
  return g_exit_code;
}
