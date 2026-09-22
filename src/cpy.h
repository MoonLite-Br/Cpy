/* cpy - a small Python-like language. Shared declarations. */
#ifndef CPY_H
#define CPY_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>

#define CPY_VERSION "1.0.0"
#define MAX_PARAMS 32
#define MAX_DEPTH 1000
#define MAX_KW 16

/* ------------------------------------------------------------ values */
typedef enum {
  T_NONE, T_BOOL, T_INT, T_FLOAT, T_UNDEF,          /* immediates */
  T_STR, T_LIST, T_TUPLE, T_DICT, T_SET, T_RANGE, T_FUNC, T_BUILTIN,
  T_CLASS, T_INST, T_BOUND, T_MODULE, T_FILE, T_SUPER, T_DESCR, T_ITER, T_GEN, T_BIGINT, T_ENV /* heap */
} VType;

typedef struct Obj { int rc; uint8_t type; } Obj;
typedef struct Value { uint8_t t; union { int64_t i; double d; Obj *o; }; } Value;

#define IS_OBJ(v) ((v).t >= T_STR)
#define IS_NUM(v) ((v).t == T_BOOL || (v).t == T_INT || (v).t == T_FLOAT)

static inline Value V_none(void) { Value v; v.t = T_NONE; v.i = 0; return v; }
static inline Value V_undef(void) { Value v; v.t = T_UNDEF; v.i = 0; return v; }
static inline Value V_bool(int b) { Value v; v.t = T_BOOL; v.i = b ? 1 : 0; return v; }
static inline Value V_int(int64_t i) { Value v; v.t = T_INT; v.i = i; return v; }
static inline Value V_float(double d) { Value v; v.t = T_FLOAT; v.d = d; return v; }
static inline Value V_obj(void *o, VType t) { Value v; v.t = t; v.o = (Obj *)o; return v; }

void free_obj(Obj *o);
#define ENV_INLINE 8
extern void *env_freelist;
static inline void incref(Value v) { if (IS_OBJ(v)) v.o->rc++; }
static inline void decref(Value v) { if (IS_OBJ(v) && --v.o->rc == 0) free_obj(v.o); }
static inline Value inc(Value v) { incref(v); return v; }
static inline void oinc(void *o) { if (o) ((Obj *)o)->rc++; }
static inline void odec(void *o) { Obj *p = (Obj *)o; if (p && --p->rc == 0) free_obj(p); }

typedef struct Str { Obj h; int len; int ascii; int cplen; uint32_t hash; char s[]; } Str;
typedef struct List { Obj h; Value *items; int len, cap; } List; /* also tuple */
typedef struct DEntry { Value key, val; uint32_t hash; } DEntry;
enum { DK_PLAIN = 0, DK_SET, DK_FROZEN, DK_DEFAULT, DK_COUNTER, DK_ORDERED };
typedef struct Dict { Obj h; DEntry *e; int n, cap, live; int *idx; int icap; uint8_t kind; Value factory; } Dict;
typedef struct Range { Obj h; int64_t start, stop, step; } Range;

typedef struct Node Node;
typedef struct FuncDef FuncDef;
typedef struct Env Env;
struct Env { Obj h; Dict *vars; Env *parent; Env *globals; FuncDef *fd; int is_class; int nslots; Value *slots; };

struct Class;
typedef struct Func { Obj h; FuncDef *def; Env *closure; Value *defaults; struct Class *owner; Str *name; struct Dict *attrs; } Func;
typedef struct Kw { int n; Str **names; Value *vals; } Kw;
typedef Value (*BuiltinFn)(Value *args, int n, Kw *kw);
typedef struct Builtin { Obj h; const char *name; BuiltinFn fn; } Builtin;
typedef struct Class { Obj h; Str *name; struct Class *base; Dict *attrs; struct Class **bases; int nbases; struct Class **mro; int nmro; int has_props; } Class;
enum { DS_STATIC = 1, DS_CLASS, DS_PROP };
typedef struct Descr { Obj h; int kind; Value fget, fset, fdel; } Descr;
typedef struct Inst { Obj h; Class *cls; Dict *attrs; } Inst;
typedef struct Bound { Obj h; Value self; Value func; } Bound;
typedef struct Module { Obj h; Str *name; Dict *attrs; } Module;
typedef struct File { Obj h; FILE *fp; int closed; } File;
typedef struct Super { Obj h; Value self; Class *owner; } Super;

typedef struct Buf { char *p; int len, cap; } Buf;
typedef struct Iter { Value seq; int64_t i; } Iter;

/* -------------------------------------------------------------- AST */
enum {
  N_CONST = 1, N_FSTR, N_FMT, N_NAME, N_LIST, N_TUPLE, N_DICT, N_BINOP, N_UNARY,
  N_AND, N_OR, N_NOT, N_CMP, N_CALL, N_ATTR, N_INDEX, N_SLICE, N_IFEXP, N_LAMBDA,
  N_LISTCOMP, N_DICTCOMP, N_COMPFOR, N_COMPIF, N_STAR,
  N_EXPR, N_ASSIGN, N_AUG, N_IF, N_WHILE, N_FOR, N_DEF, N_CLASS, N_RETURN,
  N_BREAK, N_CONTINUE, N_PASS, N_TRY, N_EXCEPT, N_BLOCK, N_RAISE, N_IMPORT,
  N_FROM, N_GLOBAL, N_NONLOCAL, N_DEL, N_ASSERT, N_WITH,
  N_SET, N_SETCOMP, N_DSTAR, N_WALRUS, N_ANNASSIGN, N_YIELD, N_YIELDFROM, N_GENEXP
};
enum {
  OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_FDIV, OP_MOD, OP_POW,
  OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_NEG, OP_POS, OP_INV,
  C_EQ, C_NE, C_LT, C_LE, C_GT, C_GE, C_IN, C_NOTIN, C_IS, C_ISNOT
};

struct Node {
  int k, line, op;
  Node *a, *b, *c, *d;
  Node **v; int nv;
  Node **v2; int nv2;
  Str **names, **names2;
  int *ops;
  Str *s, *s2;
  Value val;
  FuncDef *fd;
  int rk, ri, rh;     /* name resolution (see resolve.c) */
  int rk2, ri2, rh2;  /* fallback resolution for names in class bodies */
  int gidx;           /* cached dict entry index for global/module lookups */
};
enum { RK_DYN = 0, RK_LOCAL, RK_FREE, RK_GLOBAL, RK_CLASS };

struct FuncDef {
  Str *name;
  Str **params; Node **defaults; int nparams; int star;
  int nnormal, nkwonly, kwstar, is_gen;   /* parameter layout: normal..., [*args], kwonly..., [**kw] */
  Node **body; int nbody; Node *expr;
  Str **globals; int nglobals; Str **nonlocals; int nnonlocals;
  const char *file; int line;
  Str **locals; int nlocals;   /* filled by resolve.c: parameters first, then other locals */
};

/* ---------------------------------------------------------- lexer */
typedef enum { TK_EOF, TK_NEWLINE, TK_INDENT, TK_DEDENT, TK_NAME, TK_INT, TK_FLOAT,
               TK_STR, TK_FSTR, TK_OP } TokKind;
typedef struct Token { TokKind k; int line; char *text; int len; int raw; int64_t i; double d; } Token;
Token *lex(const char *src, const char *file, int *ntok);
char *decode_escapes(const char *s, int len, int *outlen);

/* --------------------------------------------------------- parser */
Node **parse_program(const char *src, const char *file, int *n);

/* ------------------------------------------------------ runtime state */
typedef struct Frame { struct Frame *prev; Func *fn; Env *env; const char *file; const char *name; int line; Value exc; } Frame;
typedef struct Handler { jmp_buf jb; Frame *frame; int depth; Value exc; struct Handler *prev; } Handler;

extern Env *g_globals, *g_benv;
extern Dict *g_builtins, *g_modules;
extern Frame *g_frame;
extern Handler *g_handler;
extern int g_depth, g_line;
extern char g_script_dir[1024];
extern int g_max_depth;
extern Str *S_init, *S_name, *S_str, *S_repr;

/* value.c */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void *xcalloc(size_t a, size_t b);
char *xstrdup(const char *s);
void buf_init(Buf *b);
void buf_add(Buf *b, const char *s, int n);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_addf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
Value buf_to_str(Buf *b);
Str *str_new(const char *s, int len);
Value V_str(const char *s);
Value V_strn(const char *s, int len);
Value V_strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
Str *intern(const char *s, int len);
#define INTERN(s) intern((s), (int)strlen(s))
uint32_t str_hash(Str *s);
int str_cp_offset(Str *s, int cp);
int utf8_clen(unsigned char c);
List *list_new(int cap);
void list_append(List *l, Value v);
void list_append_own(List *l, Value v);
Value V_list_new(void);
Value V_tuple(Value *items, int n);
Dict *dict_new(void);
Value *dict_find(Dict *d, Value key);
Value *dict_find_str(Dict *d, Str *s);
Value *dict_find_cstr(Dict *d, const char *s);
void dict_set(Dict *d, Value key, Value val);
void dict_set_str(Dict *d, Str *s, Value val);
void dict_set_cstr(Dict *d, const char *s, Value val);
int dict_del(Dict *d, Value key);
int dict_index_str(Dict *d, Str *s);
void dict_set_hint(Dict *d, Str *s, Value val, int *hint);
Value make_bound(Value self, Value func);
Value make_range(int64_t a, int64_t b, int64_t c);
int64_t range_len(Range *r);
int val_truthy(Value v);
int val_eq(Value a, Value b);
uint32_t val_hash(Value v);
int val_cmpop(int op, Value a, Value b);
int val_lt(Value a, Value b);
const char *type_name(Value v);
Str *val_str(Value v);
Str *val_repr(Value v);
void fmt_float(double d, char *out);
Value format_value(Value v, const char *spec);
Value str_percent(Str *fmt, Value args);
void iter_init(Iter *it, Value seq);
int iter_next(Iter *it, Value *out);
void iter_done(Iter *it);
Value list_from_iter(Value seq);

/* interp.c */
Env *env_new_dict(Env *parent, Env *globals);
Env *env_new_func(Env *parent, Env *globals, FuncDef *fd);
void resolve_program(Node **prog, int n);
void interp_init(void);
Value eval(Node *n, Env *env);
int exec_block(Node **v, int n, Env *env);
Value call_value(Value f, Value *args, int n, Kw *kw);
int call_dunder(Value self, const char *name, Value *args, int n, Value *out);
void throw_error(const char *cls, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void throw_value(Value exc) __attribute__((noreturn));
void throw_keyerror(Value key) __attribute__((noreturn));
Value get_attr(Value obj, Str *name);
void set_attr(Value obj, Str *name, Value v);
int is_instance_of(Value obj, Class *cls);
int class_is_subclass(Class *c, Class *base);
Class *make_class(Str *name, Class **bases, int nb, Dict *attrs);
Value make_generator(Func *f, Env *e);
void gen_free(Obj *o);
void iterobj_free(Obj *o);
int iter_step(Value it, Value *out);
Value make_iterator(Value seq);
Value make_iterobj(int kind, Value a, Value b, Value c);
Value gen_yield(Value v);
Value make_comp_iter(Node *n, Env *env, Value first);
Value gen_get_method(Value obj, Str *name);
Value gen_retval(Value g);
Value run_gen_body(Func *f, Env *e);
Value g_ret_get(void);
void g_ret_set(Value v);
void eval_assign(Node *t, Value v, Env *env);
int inst_iter_next(Value it, Value *out);
enum { IT_SEQ = 1, IT_MAP, IT_FILTER, IT_ZIP, IT_ENUM, IT_CALLIT, IT_COMP, IT_COUNT, IT_CYCLE, IT_REPEAT, IT_CHAIN,
       IT_ISLICE, IT_TAKEWHILE, IT_DROPWHILE, IT_STARMAP, IT_ZIPLONG, IT_ACCUM, IT_COMPRESS, IT_PAIRWISE };
Value iter_module(void);
void iterobj_init_nums(Value it, int64_t i, int64_t n1, int64_t n2, int64_t n3, int flag);
Value builtin_type_attr(const char *tname, Str *name);
Value class_member_list(Value cls);
Value new_set_from_iter(Value seq, int kind);
Value new_set(int kind);
Value default_object_method(Value self, Str *name);
Value binop(int op, Value a, Value b);
Value index_get(Value o, Value i);
int contains(Value container, Value item);
Value import_module(Str *name);
void interp_setup_path(const char *dir);
Value get_sys_path(void);
void run_module_source(const char *src, const char *file, Env *env);
void report_uncaught(Value exc);
void clear_traceback(void);
char *read_file(const char *path, long *len);

/* builtins.c */
void builtins_init(void);
Value builtin_method(Value obj, Str *name);
Value *builtin_method_raw(Value obj, Str *name);
Value builtin_module(const char *name);
void set_sys_argv(int argc, char **argv);
void chk(const char *name, int n, int lo, int hi);
Value *kwget(Kw *kw, const char *name);
Value sorted_list(Value seq, Value keyfn, int reverse);
int64_t need_int(Value v, const char *ctx);
double need_num(Value v, const char *ctx);
Str *need_str(Value v, const char *ctx);
int try_get_attr(Value o, Str *name, Value *out);
Value make_file(FILE *fp);
void methods_init(void);
void reg_builtin(const char *name, BuiltinFn fn);
Value new_builtin(const char *name, BuiltinFn fn);

#endif
