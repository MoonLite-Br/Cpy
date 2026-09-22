/* interp.c - tree-walking evaluator: scopes, calls, classes, exceptions, imports */
#include <alloca.h>
#include <unistd.h>
#include "cpy.h"

Env *g_globals, *g_benv;
Dict *g_builtins, *g_modules;
Frame *g_frame;
Handler *g_handler;
int g_depth, g_line;
char g_script_dir[1024] = ".";
Str *S_init, *S_name, *S_str, *S_repr;
static Str *S_class, *S_dict, *S_mro, *S_bases, *S_setter, *S_getter, *S_deleter, *S_fget, *S_self, *S_func;
static Value g_ret;
Value g_ret_get(void) { return g_ret; }
void g_ret_set(Value v) { g_ret = v; }
static Class *C_BaseException;
static int g_raising;

/* ---------------------------------------------------------- colors */
/* Mirrors what Python 3.13+ and Node.js do: color stderr only when it's
   a real terminal, and always defer to NO_COLOR / FORCE_COLOR. */
static int g_use_color = -1;
static int use_color(void) {
  if (g_use_color < 0) {
    const char *term = getenv("TERM");
    g_use_color = isatty(fileno(stderr)) && !(term && !strcmp(term, "dumb"));
    if (getenv("NO_COLOR")) g_use_color = 0;
    if (getenv("FORCE_COLOR")) g_use_color = 1;
  }
  return g_use_color;
}
#define CLR(code) (use_color() ? code : "")
#define C_RESET   CLR("\x1b[0m")
#define C_BOLD    CLR("\x1b[1m")
#define C_DIM     CLR("\x1b[2m")
#define C_RED     CLR("\x1b[31m")
#define C_BRED    CLR("\x1b[1;31m")
#define C_GREEN   CLR("\x1b[32m")
#define C_YELLOW  CLR("\x1b[33m")
#define C_BLUE    CLR("\x1b[34m")
#define C_MAGENTA CLR("\x1b[35m")
#define C_CYAN    CLR("\x1b[36m")

/* ------------------------------------------------------- traceback */
/* Frames are captured as raw (file, line, name) data at throw time and only
   formatted -- with color and a source snippet -- when actually printed. */
typedef struct { char file[400]; char name[120]; int line; } TBFrame;
static TBFrame g_tbframes[64];
static int g_tbn;
static int g_tb_captured;

/* Best-effort: pull the exact source line out of the original file, the
   way Python's traceback module does, so the error shows real context. */
static const char *get_source_line(const char *file, int line) {
  static char buf[512];
  if (!file || line <= 0) return NULL;
  if (file[0] == '<') return NULL; /* <string>, <stdin>: no file on disk */
  FILE *f = fopen(file, "r");
  if (!f) return NULL;
  int n = 0; buf[0] = 0;
  while (n < line && fgets(buf, sizeof buf, f)) n++;
  fclose(f);
  if (n != line) return NULL;
  size_t l = strlen(buf);
  while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
  return buf;
}

enum { ST_NONE = 0, ST_BREAK, ST_CONT, ST_RET };

/* ------------------------------------------------------------ env */
static Env *env_alloc(int nslots) {
  Env *e;
  if (nslots <= ENV_INLINE) {
    if (env_freelist) { e = (Env *)env_freelist; env_freelist = e->parent; }
    else e = xmalloc(sizeof(Env) + ENV_INLINE * sizeof(Value));
  } else e = xmalloc(sizeof(Env) + nslots * sizeof(Value));
  e->h.rc = 1; e->h.type = T_ENV;
  e->nslots = nslots; e->slots = (Value *)(e + 1);
  return e;
}
Env *env_new_dict(Env *parent, Env *globals) {
  Env *e = env_alloc(0);
  e->vars = dict_new(); e->parent = parent; oinc(parent);
  e->globals = globals ? globals : e;
  e->fd = NULL; e->is_class = 0;
  return e;
}
Env *env_new_func(Env *parent, Env *globals, FuncDef *fd) {
  int ns = fd->nlocals;
  Env *e = env_alloc(ns);
  for (int i = 0; i < ns; i++) e->slots[i] = V_undef();
  e->vars = NULL; e->parent = parent; oinc(parent);
  e->globals = globals ? globals : e;
  e->fd = fd; e->is_class = 0;
  return e;
}

/* ------------------------------------------------------ exceptions */
static void capture_tb(void) {
  Frame *fs[64]; int nf = 0;
  for (Frame *f = g_frame; f && nf < 64; f = f->prev) fs[nf++] = f;
  g_tbn = 0;
  for (int i = nf - 1; i >= 0 && g_tbn < 64; i--) {
    TBFrame *tf = &g_tbframes[g_tbn++];
    tf->line = (i == 0) ? g_line : fs[i]->line;
    snprintf(tf->file, sizeof tf->file, "%s", fs[i]->file);
    snprintf(tf->name, sizeof tf->name, "%s", fs[i]->name);
  }
  g_tb_captured = 1;
}

void clear_traceback(void) { g_tb_captured = 0; g_tbn = 0; }

void report_uncaught(Value exc) {
  fflush(stdout);
  if (g_tb_captured && g_tbn) {
    fprintf(stderr, "%sTraceback (most recent call last):%s\n", C_DIM, C_RESET);
    for (int i = 0; i < g_tbn; i++) {
      TBFrame *tf = &g_tbframes[i];
      fprintf(stderr, "  %sFile%s \"%s%s%s\", line %s%d%s, in %s%s%s\n",
              C_DIM, C_RESET, C_CYAN, tf->file, C_RESET,
              C_YELLOW, tf->line, C_RESET, C_MAGENTA, tf->name, C_RESET);
      const char *src = get_source_line(tf->file, tf->line);
      if (src) {
        const char *p = src; while (*p == ' ' || *p == '\t') p++; /* trim indent */
        if (*p) fprintf(stderr, "    %s%s%s\n", C_GREEN, p, C_RESET);
      }
    }
  }
  const char *cn = exc.t == T_INST ? ((Inst *)exc.o)->cls->name->s : type_name(exc);
  char msg[1024]; msg[0] = 0;
  Handler h; h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) {
    Str *s = val_str(exc);
    snprintf(msg, sizeof msg, "%s", s->s); odec(s);
  } else snprintf(msg, sizeof msg, "<error while printing exception>");
  g_handler = h.prev;
  if (msg[0]) fprintf(stderr, "%s%s%s: %s%s%s\n", C_BRED, cn, C_RESET, C_RED, msg, C_RESET);
  else fprintf(stderr, "%s%s%s\n", C_BRED, cn, C_RESET);
  g_tb_captured = 0; g_tbn = 0;
}

void throw_value(Value exc) {
  if (!g_tb_captured) capture_tb();
  if (!g_handler) { report_uncaught(exc); exit(1); }
  Handler *h = g_handler;
  h->exc = exc;
  longjmp(h->jb, 1);
}

void throw_error(const char *cls, const char *fmt, ...) {
  char buf[1024]; va_list ap;
  va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  Value *c = g_builtins ? dict_find_cstr(g_builtins, cls) : NULL;
  if (!c || g_raising) { fflush(stdout); fprintf(stderr, "cpy: fatal error: %s: %s\n", cls, buf); exit(1); }
  Value msg = V_str(buf);
  g_raising = 1;
  Value exc = call_value(*c, &msg, 1, NULL);
  g_raising = 0;
  decref(msg);
  throw_value(exc);
}

void throw_keyerror(Value key) {
  Value *c = dict_find_cstr(g_builtins, "KeyError");
  if (!c || g_raising) { fflush(stdout); fprintf(stderr, "cpy: fatal error: KeyError\n"); exit(1); }
  g_raising = 1;
  Value exc = call_value(*c, &key, 1, NULL);
  g_raising = 0;
  throw_value(exc);
}

/* ------------------------------------------------------- classes */
static Str *S_getattr, *S_setattr, *S_new, *S_doc, *S_init_subclass, *S_class_getitem;
static Class *C_object;

static Value *class_lookup(Class *c, Str *name) {
  for (int i = 0; i < c->nmro; i++) { Value *v = dict_find_str(c->mro[i]->attrs, name); if (v) return v; }
  return NULL;
}
int is_instance_of(Value obj, Class *cls) {
  if (obj.t != T_INST) return 0;
  Class *ic = ((Inst *)obj.o)->cls;
  for (int i = 0; i < ic->nmro; i++) if (ic->mro[i] == cls) return 1;
  return 0;
}
int class_is_subclass(Class *c, Class *base) {
  for (int i = 0; i < c->nmro; i++) if (c->mro[i] == base) return 1;
  return 0;
}
static int exc_matches(Value exc, Value typ) {
  if (typ.t == T_CLASS) return is_instance_of(exc, (Class *)typ.o);
  if (typ.t == T_TUPLE) {
    List *l = (List *)typ.o;
    for (int i = 0; i < l->len; i++) if (exc_matches(exc, l->items[i])) return 1;
    return 0;
  }
  throw_error("TypeError", "catching classes that do not inherit from BaseException is not allowed");
}

/* C3 linearisation */
static void compute_mro(Class *c) {
  int nb = c->nbases;
  if (nb == 0) { c->mro = xmalloc(sizeof(Class *)); c->mro[0] = c; c->nmro = 1; return; }
  Class ***lists = xmalloc((nb + 1) * sizeof(Class **)); int *lens = xmalloc((nb + 1) * sizeof(int)); int *pos = xcalloc(nb + 1, sizeof(int));
  int total = 1;
  for (int i = 0; i < nb; i++) { lists[i] = c->bases[i]->mro; lens[i] = c->bases[i]->nmro; total += lens[i]; }
  lists[nb] = c->bases; lens[nb] = nb;
  Class **res = xmalloc((total + 1) * sizeof(Class *)); int nr = 0;
  res[nr++] = c;
  for (;;) {
    int any = 0; Class *cand = NULL;
    for (int i = 0; i <= nb && !cand; i++) {
      if (pos[i] >= lens[i]) continue;
      any = 1;
      Class *h = lists[i][pos[i]]; int ok = 1;
      for (int j = 0; j <= nb && ok; j++) for (int k = pos[j] + 1; k < lens[j]; k++) if (lists[j][k] == h) { ok = 0; break; }
      if (ok) cand = h;
    }
    if (!any) break;
    if (!cand) { free(lists); free(lens); free(pos); free(res); throw_error("TypeError", "Cannot create a consistent method resolution order (MRO) for bases"); }
    res[nr++] = cand;
    for (int i = 0; i <= nb; i++) if (pos[i] < lens[i] && lists[i][pos[i]] == cand) pos[i]++;
  }
  free(lists); free(lens); free(pos);
  c->mro = res; c->nmro = nr;
}

/* attrs: reference is taken over by the class */
Class *make_class(Str *name, Class **bases, int nb, Dict *attrs) {
  Class *c = xmalloc(sizeof(Class));
  c->h.rc = 1; c->h.type = T_CLASS; c->name = name; oinc(name); c->attrs = attrs;
  c->nbases = nb; c->bases = nb ? xmalloc(nb * sizeof(Class *)) : NULL;
  for (int i = 0; i < nb; i++) { c->bases[i] = bases[i]; oinc(bases[i]); }
  c->base = nb ? bases[0] : NULL; c->mro = NULL; c->nmro = 0; c->has_props = 0;
  compute_mro(c);
  for (int i = 0; i < attrs->n; i++) {
    if (attrs->e[i].key.t == T_UNDEF) continue;
    Value v = attrs->e[i].val;
    if (v.t == T_FUNC && !((Func *)v.o)->owner) { ((Func *)v.o)->owner = c; oinc(c); }
    else if (v.t == T_DESCR) {
      Descr *d = (Descr *)v.o;
      Value fs[3] = {d->fget, d->fset, d->fdel};
      for (int k = 0; k < 3; k++) if (fs[k].t == T_FUNC && !((Func *)fs[k].o)->owner) { ((Func *)fs[k].o)->owner = c; oinc(c); }
    }
  }
  for (int i = 0; i < c->nmro; i++) {
    Dict *at = c->mro[i]->attrs;
    for (int k = 0; k < at->n; k++) {
      if (at->e[k].key.t != T_STR) continue;
      if (at->e[k].val.t == T_DESCR && ((Descr *)at->e[k].val.o)->kind == DS_PROP) c->has_props |= 1;
      Str *ks = (Str *)at->e[k].key.o;
      if (ks == S_setattr) c->has_props |= 2;
    }
  }
  return c;
}

Value class_member_list(Value cls) {
  Value *ml = class_lookup((Class *)cls.o, INTERN("_member_list_"));
  if (ml && ml->t == T_LIST) return inc(*ml);
  return V_undef();
}

int call_dunder(Value self, const char *name, Value *args, int n, Value *out) {
  if (self.t != T_INST) return 0;
  Value *m = class_lookup(((Inst *)self.o)->cls, INTERN(name));
  if (!m) return 0;
  Value *a = alloca((n + 1) * sizeof(Value));
  a[0] = self;
  for (int i = 0; i < n; i++) a[i + 1] = args[i];
  *out = call_value(*m, a, n + 1, NULL);
  return 1;
}

/* ----------------------------------------------------- attributes */
static Value bind_class_attr(Value obj, Value v, Class *owner_cls, int via_instance) {
  if (v.t == T_FUNC) return via_instance ? make_bound(obj, v) : inc(v);
  if (v.t == T_DESCR) {
    Descr *d = (Descr *)v.o;
    switch (d->kind) {
    case DS_STATIC: return inc(d->fget);
    case DS_CLASS: return make_bound(via_instance ? V_obj(((Inst *)obj.o)->cls, T_CLASS) : obj, d->fget);
    default:
      if (via_instance) {
        if (d->fget.t == T_NONE) throw_error("AttributeError", "unreadable attribute");
        return call_value(d->fget, &obj, 1, NULL);
      }
      return inc(v);
    }
  }
  (void)owner_cls;
  return inc(v);
}

Value default_object_method(Value self, Str *name);

Value get_attr(Value obj, Str *name) {
  switch (obj.t) {
  case T_INST: {
    Inst *in = (Inst *)obj.o; Class *cls = in->cls;
    Value *v;
    if (cls->has_props & 1) {
      v = class_lookup(cls, name);
      if (v && v->t == T_DESCR && ((Descr *)v->o)->kind == DS_PROP) return bind_class_attr(obj, *v, cls, 1);
    }
    v = dict_find_str(in->attrs, name);
    if (v) return inc(*v);
    v = class_lookup(cls, name);
    if (v) return bind_class_attr(obj, *v, cls, 1);
    if (name == S_class) return inc(V_obj(cls, T_CLASS));
    if (name == S_dict) return inc(V_obj(in->attrs, T_DICT));
    v = class_lookup(cls, S_getattr);
    if (v) { Value nm = inc(V_obj(name, T_STR)); Value args[2] = {obj, nm}; Value r = call_value(*v, args, 2, NULL); decref(nm); return r; }
    Value dm = default_object_method(obj, name);
    if (dm.t != T_UNDEF) return dm;
    throw_error("AttributeError", "'%s' object has no attribute '%s'", cls->name->s, name->s);
  }
  case T_CLASS: {
    Class *c = (Class *)obj.o;
    if (name == S_name) return inc(V_obj(c->name, T_STR));
    Value *v = class_lookup(c, name);
    if (v) return bind_class_attr(obj, *v, c, 0);
    if (name == S_mro) { List *l = list_new(c->nmro); l->h.type = T_TUPLE; for (int i = 0; i < c->nmro; i++) list_append(l, V_obj(c->mro[i], T_CLASS)); return V_obj(l, T_TUPLE); }
    if (name == S_bases) { List *l = list_new(c->nbases); l->h.type = T_TUPLE; for (int i = 0; i < c->nbases; i++) list_append(l, V_obj(c->bases[i], T_CLASS)); return V_obj(l, T_TUPLE); }
    if (name == S_dict) return inc(V_obj(c->attrs, T_DICT));
    Value dm = default_object_method(V_undef(), name);
    if (dm.t != T_UNDEF) return dm;
    throw_error("AttributeError", "type object '%s' has no attribute '%s'", c->name->s, name->s);
  }
  case T_MODULE: {
    Module *m = (Module *)obj.o;
    Value *v = dict_find_str(m->attrs, name);
    if (v) return inc(*v);
    if (name == S_dict) return inc(V_obj(m->attrs, T_DICT));
    throw_error("AttributeError", "module '%s' has no attribute '%s'", m->name->s, name->s);
  }
  case T_SUPER: {
    Super *sp = (Super *)obj.o;
    Class *ic = sp->self.t == T_INST ? ((Inst *)sp->self.o)->cls : sp->self.t == T_CLASS ? (Class *)sp->self.o : NULL;
    if (ic) {
      int k = 0;
      for (; k < ic->nmro; k++) if (ic->mro[k] == sp->owner) break;
      for (k++; k < ic->nmro; k++) {
        Value *v = dict_find_str(ic->mro[k]->attrs, name);
        if (v) return bind_class_attr(sp->self, *v, ic, sp->self.t == T_INST);
      }
    }
    Value dm = default_object_method(sp->self, name);
    if (dm.t != T_UNDEF) return dm;
    throw_error("AttributeError", "'super' object has no attribute '%s'", name->s);
  }
  case T_DESCR: {
    Descr *d = (Descr *)obj.o;
    if (d->kind == DS_PROP && (name == S_setter || name == S_getter || name == S_deleter)) {
      Value *bf = dict_find_cstr(g_builtins, name == S_setter ? "__prop_setter" : name == S_getter ? "__prop_getter" : "__prop_deleter");
      return make_bound(obj, *bf);
    }
    if (name == S_fget) return inc(d->fget);
    break;
  }
  case T_FUNC: {
    Func *fn = (Func *)obj.o;
    if (fn->attrs) { Value *v = dict_find_str(fn->attrs, name); if (v) return inc(*v); }
    if (name == S_name) return inc(V_obj(fn->name, T_STR));
    if (name == S_doc) return V_none();
    if (name == S_dict) { if (!fn->attrs) fn->attrs = dict_new(); return inc(V_obj(fn->attrs, T_DICT)); }
    break;
  }
  case T_BUILTIN: {
    if (name == S_name) return V_str(((Builtin *)obj.o)->name);
    Value m = builtin_type_attr(((Builtin *)obj.o)->name, name);
    if (m.t != T_UNDEF) return m;
    break;
  }
  case T_BOUND: {
    Bound *b = (Bound *)obj.o;
    if (name == S_self) return inc(b->self);
    if (name == S_func) return inc(b->func);
    if (name == S_name) return get_attr(b->func, name);
    break;
  }
  default: break;
  }
  Value m = builtin_method(obj, name);
  if (m.t != T_UNDEF) return m;
  throw_error("AttributeError", "'%s' object has no attribute '%s'", type_name(obj), name->s);
}

void set_attr(Value obj, Str *name, Value v) {
  switch (obj.t) {
  case T_INST: {
    Inst *in = (Inst *)obj.o; Class *cls = in->cls;
    if (cls->has_props) {
      if (cls->has_props & 1) {
        Value *cv = class_lookup(cls, name);
        if (cv && cv->t == T_DESCR && ((Descr *)cv->o)->kind == DS_PROP) {
          Descr *d = (Descr *)cv->o;
          if (d->fset.t == T_NONE) throw_error("AttributeError", "property '%s' of '%s' object has no setter", name->s, cls->name->s);
          Value args[2] = {obj, v}; Value r = call_value(d->fset, args, 2, NULL); decref(r); return;
        }
      }
      if (cls->has_props & 2) {
        Value *sa = class_lookup(cls, S_setattr);
        if (sa && sa->t == T_FUNC) { Value nm = inc(V_obj(name, T_STR)); Value args[3] = {obj, nm, v}; Value r = call_value(*sa, args, 3, NULL); decref(r); decref(nm); return; }
      }
    }
    dict_set_str(in->attrs, name, v); return;
  }
  case T_CLASS: {
    Class *c = (Class *)obj.o;
    dict_set_str(c->attrs, name, v);
    if (name == S_setattr) c->has_props |= 2;
    if (v.t == T_DESCR && ((Descr *)v.o)->kind == DS_PROP) c->has_props |= 1;
    if (v.t == T_FUNC && !((Func *)v.o)->owner) { ((Func *)v.o)->owner = c; oinc(c); }
    return;
  }
  case T_MODULE: dict_set_str(((Module *)obj.o)->attrs, name, v); return;
  case T_FUNC: { Func *fn = (Func *)obj.o; if (!fn->attrs) fn->attrs = dict_new(); dict_set_str(fn->attrs, name, v); return; }
  default: throw_error("AttributeError", "'%s' object has no attribute '%s'", type_name(obj), name->s);
  }
}

/* ------------------------------------------------------ arithmetic */
static double as_d(Value v) { return v.t == T_FLOAT ? v.d : (double)v.i; }
static int is_intlike(Value v) { return v.t == T_INT || v.t == T_BOOL; }
static const char *op_name(int op) {
  switch (op) {
  case OP_ADD: return "+"; case OP_SUB: return "-"; case OP_MUL: return "*"; case OP_DIV: return "/";
  case OP_FDIV: return "//"; case OP_MOD: return "%"; case OP_POW: return "**"; case OP_BAND: return "&";
  case OP_BOR: return "|"; case OP_BXOR: return "^"; case OP_SHL: return "<<"; case OP_SHR: return ">>";
  default: return "?";
  }
}
static const char *dunders[] = {NULL, "__add__", "__sub__", "__mul__", "__truediv__", "__floordiv__", "__mod__", "__pow__", "__and__", "__or__", "__xor__", "__lshift__", "__rshift__"};
static const char *rdunders[] = {NULL, "__radd__", "__rsub__", "__rmul__", "__rtruediv__", "__rfloordiv__", "__rmod__", "__rpow__", "__rand__", "__ror__", "__rxor__", "__rlshift__", "__rrshift__"};

static int64_t floordiv_i(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static int64_t mod_i(int64_t a, int64_t b) { int64_t r = a % b; if (r != 0 && ((r < 0) != (b < 0))) r += b; return r; }

static Value repeat_seq(Value s, int64_t n) {
  if (n < 0) n = 0;
  if (s.t == T_STR) {
    Str *st = (Str *)s.o;
    if (st->len && n > (int64_t)(1 << 30) / st->len) throw_error("OverflowError", "repeated string is too long");
    Buf b; buf_init(&b); buf_add(&b, "", 0);
    for (int64_t i = 0; i < n; i++) buf_add(&b, st->s, st->len);
    return buf_to_str(&b);
  }
  List *src = (List *)s.o; List *l = list_new((int)(src->len * n)); l->h.type = s.t;
  for (int64_t i = 0; i < n; i++) for (int j = 0; j < src->len; j++) list_append(l, src->items[j]);
  return V_obj(l, s.t);
}

Value binop(int op, Value a, Value b) {
  if (IS_NUM(a) && IS_NUM(b)) {
    if (is_intlike(a) && is_intlike(b)) {
      int64_t x = a.i, y = b.i, r;
      switch (op) {
      case OP_ADD: if (__builtin_add_overflow(x, y, &r)) throw_error("OverflowError", "integer overflow"); return V_int(r);
      case OP_SUB: if (__builtin_sub_overflow(x, y, &r)) throw_error("OverflowError", "integer overflow"); return V_int(r);
      case OP_MUL: if (__builtin_mul_overflow(x, y, &r)) throw_error("OverflowError", "integer overflow"); return V_int(r);
      case OP_DIV: if (y == 0) throw_error("ZeroDivisionError", "division by zero"); return V_float((double)x / (double)y);
      case OP_FDIV:
        if (y == 0) throw_error("ZeroDivisionError", "integer division or modulo by zero");
        if (y == -1) { if (x == INT64_MIN) throw_error("OverflowError", "integer overflow"); return V_int(-x); }
        return V_int(floordiv_i(x, y));
      case OP_MOD:
        if (y == 0) throw_error("ZeroDivisionError", "integer division or modulo by zero");
        if (y == -1) return V_int(0);
        return V_int(mod_i(x, y));
      case OP_POW: {
        if (y < 0) {
          if (x == 0) throw_error("ZeroDivisionError", "0 cannot be raised to a negative power");
          return V_float(pow((double)x, (double)y));
        }
        int64_t res = 1, base = x, e = y;
        while (e > 0) {
          if (e & 1) { if (__builtin_mul_overflow(res, base, &res)) throw_error("OverflowError", "integer overflow"); }
          e >>= 1;
          if (e && __builtin_mul_overflow(base, base, &base)) throw_error("OverflowError", "integer overflow");
        }
        return V_int(res);
      }
      case OP_BAND: return V_int(x & y);
      case OP_BOR: return V_int(x | y);
      case OP_BXOR: return V_int(x ^ y);
      case OP_SHL:
        if (y < 0) throw_error("ValueError", "negative shift count");
        if (x == 0) return V_int(0);
        if (y > 62 || ((x << y) >> y) != x) throw_error("OverflowError", "integer overflow");
        return V_int(x << y);
      case OP_SHR:
        if (y < 0) throw_error("ValueError", "negative shift count");
        if (y > 63) return V_int(x < 0 ? -1 : 0);
        return V_int(x >> y);
      }
    } else {
      double x = as_d(a), y = as_d(b);
      switch (op) {
      case OP_ADD: return V_float(x + y);
      case OP_SUB: return V_float(x - y);
      case OP_MUL: return V_float(x * y);
      case OP_DIV: if (y == 0) throw_error("ZeroDivisionError", "division by zero"); return V_float(x / y);
      case OP_FDIV: if (y == 0) throw_error("ZeroDivisionError", "float floor division by zero"); return V_float(floor(x / y));
      case OP_MOD: {
        if (y == 0) throw_error("ZeroDivisionError", "float modulo by zero");
        double r = fmod(x, y); if (r != 0 && ((r < 0) != (y < 0))) r += y; return V_float(r);
      }
      case OP_POW:
        if (x == 0 && y < 0) throw_error("ZeroDivisionError", "0.0 cannot be raised to a negative power");
        return V_float(pow(x, y));
      default: break;
      }
    }
  } else {
    if (op == OP_ADD) {
      if (a.t == T_STR && b.t == T_STR) {
        Str *x = (Str *)a.o, *y = (Str *)b.o;
        Str *r = xmalloc(sizeof(Str) + x->len + y->len + 1);
        memcpy(r->s, x->s, x->len); memcpy(r->s + x->len, y->s, y->len); r->s[x->len + y->len] = 0;
        Value v = V_strn(r->s, x->len + y->len); free(r); return v;
      }
      if ((a.t == T_LIST && b.t == T_LIST) || (a.t == T_TUPLE && b.t == T_TUPLE)) {
        List *x = (List *)a.o, *y = (List *)b.o; List *l = list_new(x->len + y->len); l->h.type = a.t;
        for (int i = 0; i < x->len; i++) list_append(l, x->items[i]);
        for (int i = 0; i < y->len; i++) list_append(l, y->items[i]);
        return V_obj(l, a.t);
      }
    }
    if (op == OP_MUL) {
      if ((a.t == T_STR || a.t == T_LIST || a.t == T_TUPLE) && is_intlike(b)) return repeat_seq(a, b.i);
      if ((b.t == T_STR || b.t == T_LIST || b.t == T_TUPLE) && is_intlike(a)) return repeat_seq(b, a.i);
    }
    if (op == OP_MOD && a.t == T_STR) return str_percent((Str *)a.o, b);
    if (a.t == T_SET && b.t == T_SET && (op == OP_BOR || op == OP_BAND || op == OP_SUB || op == OP_BXOR)) {
      Dict *x = (Dict *)a.o, *y = (Dict *)b.o;
      Dict *r = dict_new(); r->kind = ((Dict *)a.o)->kind == DK_FROZEN ? DK_FROZEN : DK_SET; r->h.type = T_SET;
      Value rv = V_obj(r, T_SET);
      for (int i = 0; i < x->n; i++) {
        if (x->e[i].key.t == T_UNDEF) continue;
        int iny = dict_find(y, x->e[i].key) != NULL;
        if (op == OP_BOR || (op == OP_BAND && iny) || ((op == OP_SUB || op == OP_BXOR) && !iny)) dict_set(r, x->e[i].key, V_none());
      }
      if (op == OP_BOR || op == OP_BXOR)
        for (int i = 0; i < y->n; i++) if (y->e[i].key.t != T_UNDEF && (op == OP_BOR || !dict_find(x, y->e[i].key))) dict_set(r, y->e[i].key, V_none());
      return rv;
    }
    if (a.t == T_DICT && b.t == T_DICT && op == OP_BOR) {
      Dict *r = dict_new(); Value rv = V_obj(r, T_DICT); r->kind = ((Dict *)a.o)->kind; r->factory = inc(((Dict *)a.o)->factory);
      for (int k = 0; k < 2; k++) {
        Dict *src = (Dict *)(k ? b.o : a.o);
        for (int i = 0; i < src->n; i++) if (src->e[i].key.t != T_UNDEF) dict_set(r, src->e[i].key, src->e[i].val);
      }
      return rv;
    }
  }
  if (op >= OP_ADD && op <= OP_SHR) {
    Value out;
    if (a.t == T_INST && call_dunder(a, dunders[op], &b, 1, &out)) return out;
    if (b.t == T_INST && call_dunder(b, rdunders[op], &a, 1, &out)) return out;
  }
  throw_error("TypeError", "unsupported operand type(s) for %s: '%s' and '%s'", op_name(op), type_name(a), type_name(b));
}

static Value unaryop(int op, Value a) {
  if (op == OP_NEG) {
    if (is_intlike(a)) { if (a.i == INT64_MIN) throw_error("OverflowError", "integer overflow"); return V_int(-a.i); }
    if (a.t == T_FLOAT) return V_float(-a.d);
    Value out; if (call_dunder(a, "__neg__", NULL, 0, &out)) return out;
    throw_error("TypeError", "bad operand type for unary -: '%s'", type_name(a));
  }
  if (op == OP_POS) {
    if (a.t == T_BOOL) return V_int(a.i);
    if (a.t == T_INT || a.t == T_FLOAT) return a;
    throw_error("TypeError", "bad operand type for unary +: '%s'", type_name(a));
  }
  if (is_intlike(a)) return V_int(~a.i);
  throw_error("TypeError", "bad operand type for unary ~: '%s'", type_name(a));
}

/* ------------------------------------------------- indexing & slices */
static int64_t as_index(Value v) {
  if (is_intlike(v)) return v.i;
  throw_error("TypeError", "indices must be integers or slices, not %s", type_name(v));
}
static int64_t norm_index(int64_t i, int64_t len, const char *what) {
  if (i < 0) i += len;
  if (i < 0 || i >= len) throw_error("IndexError", "%s index out of range", what);
  return i;
}

Value index_get(Value o, Value i) {
  switch (o.t) {
  case T_LIST: case T_TUPLE: {
    List *l = (List *)o.o;
    return inc(l->items[norm_index(as_index(i), l->len, o.t == T_LIST ? "list" : "tuple")]);
  }
  case T_STR: {
    Str *s = (Str *)o.o;
    int64_t k = norm_index(as_index(i), s->cplen, "string");
    if (s->ascii) return V_strn(s->s + k, 1);
    int off = str_cp_offset(s, (int)k);
    return V_strn(s->s + off, utf8_clen((unsigned char)s->s[off]));
  }
  case T_DICT: {
    Dict *d = (Dict *)o.o;
    Value *v = dict_find(d, i);
    if (v) return inc(*v);
    if (d->kind == DK_DEFAULT && d->factory.t != T_NONE) {
      Value nv = call_value(d->factory, NULL, 0, NULL);
      dict_set(d, i, nv); return nv;
    }
    if (d->kind == DK_COUNTER) return V_int(0);
    throw_keyerror(i);
  }
  case T_RANGE: {
    Range *r = (Range *)o.o;
    int64_t k = norm_index(as_index(i), range_len(r), "range object");
    return V_int(r->start + k * r->step);
  }
  case T_CLASS: {
    Value *cg = class_lookup((Class *)o.o, S_class_getitem);
    if (cg) { Value args[2] = {o, i}; Value fnv = cg->t == T_DESCR ? ((Descr *)cg->o)->fget : *cg; Value a1[1]; (void)a1; return call_value(fnv, args, 2, NULL); }
    break;
  }
  case T_INST: { Value out; if (call_dunder(o, "__getitem__", &i, 1, &out)) return out; }
  /* fallthrough */
  default: break;
  }
  switch (o.t) {
  default: throw_error("TypeError", "'%s' object is not subscriptable", type_name(o));
  }
}

static void index_set(Value o, Value i, Value v) {
  switch (o.t) {
  case T_LIST: {
    List *l = (List *)o.o; int64_t k = norm_index(as_index(i), l->len, "list assignment");
    Value old = l->items[k]; l->items[k] = inc(v); decref(old); return;
  }
  case T_DICT: dict_set((Dict *)o.o, i, v); return;
  case T_INST: { Value args[2] = {i, v}, out; if (call_dunder(o, "__setitem__", args, 2, &out)) { decref(out); return; } }
  /* fallthrough */
  default: throw_error("TypeError", "'%s' object does not support item assignment", type_name(o));
  }
}

static void slice_bounds(int64_t len, Value lo, Value hi, Value st, int64_t *start, int64_t *step, int64_t *count) {
  int64_t s = 1, a, b;
  if (st.t != T_NONE) { s = as_index(st); if (s == 0) throw_error("ValueError", "slice step cannot be zero"); }
  if (s > 0) {
    a = lo.t == T_NONE ? 0 : as_index(lo); b = hi.t == T_NONE ? len : as_index(hi);
    if (a < 0) { a += len; if (a < 0) a = 0; } if (a > len) a = len;
    if (b < 0) { b += len; if (b < 0) b = 0; } if (b > len) b = len;
    *count = b > a ? (b - a + s - 1) / s : 0;
  } else {
    a = lo.t == T_NONE ? len - 1 : as_index(lo); b = hi.t == T_NONE ? -1 : as_index(hi);
    if (lo.t != T_NONE) { if (a < 0) { a += len; if (a < 0) a = -1; } if (a >= len) a = len - 1; }
    if (hi.t != T_NONE) { if (b < 0) { b += len; if (b < 0) b = -1; } if (b >= len) b = len - 1; }
    *count = a > b ? (a - b + (-s) - 1) / (-s) : 0;
  }
  *start = a; *step = s;
}

static Value slice_get(Value o, Value lo, Value hi, Value st) {
  int64_t start, step, count;
  if (o.t == T_LIST || o.t == T_TUPLE) {
    List *l = (List *)o.o;
    slice_bounds(l->len, lo, hi, st, &start, &step, &count);
    List *r = list_new((int)count); r->h.type = o.t;
    for (int64_t k = 0; k < count; k++) list_append(r, l->items[start + k * step]);
    return V_obj(r, o.t);
  }
  if (o.t == T_STR) {
    Str *s = (Str *)o.o;
    slice_bounds(s->cplen, lo, hi, st, &start, &step, &count);
    if (s->ascii) {
      if (step == 1) return V_strn(s->s + start, (int)count);
      Buf b; buf_init(&b); buf_add(&b, "", 0);
      for (int64_t k = 0; k < count; k++) buf_addc(&b, s->s[start + k * step]);
      return buf_to_str(&b);
    }
    int *offs = xmalloc((s->cplen + 1) * sizeof(int)); int off = 0, c = 0;
    while (off < s->len) { offs[c++] = off; off += utf8_clen((unsigned char)s->s[off]); }
    offs[c] = s->len;
    Buf b; buf_init(&b); buf_add(&b, "", 0);
    for (int64_t k = 0; k < count; k++) { int cp = (int)(start + k * step); buf_add(&b, s->s + offs[cp], offs[cp + 1] - offs[cp]); }
    free(offs); return buf_to_str(&b);
  }
  if (o.t == T_RANGE) {
    Value l = list_from_iter(o); Value r = slice_get(l, lo, hi, st); decref(l); return r;
  }
  throw_error("TypeError", "'%s' object is not subscriptable", type_name(o));
}

/* replace list[lo:hi] with the items of `v` (v may be NULL to delete) */
static void slice_replace(Value o, Value lo, Value hi, Value st, Value *v) {
  if (o.t != T_LIST) throw_error("TypeError", "'%s' object does not support slice assignment", type_name(o));
  List *l = (List *)o.o; int64_t start, step, count;
  slice_bounds(l->len, lo, hi, st, &start, &step, &count);
  if (step != 1) {
    if (v) {
      Value snap = list_from_iter(*v); List *sl = (List *)snap.o;
      if (sl->len != count) { int sn = sl->len; decref(snap); throw_error("ValueError", "attempt to assign sequence of size %d to extended slice of size %d", sn, (int)count); }
      for (int64_t k = 0; k < count; k++) { Value old = l->items[start + k * step]; l->items[start + k * step] = inc(sl->items[k]); decref(old); }
      decref(snap); return;
    }
    char *del = xcalloc(l->len, 1);
    for (int64_t k = 0; k < count; k++) del[start + k * step] = 1;
    int j = 0;
    for (int i = 0; i < l->len; i++) { if (del[i]) decref(l->items[i]); else l->items[j++] = l->items[i]; }
    l->len = j; free(del); return;
  }
  Value snap = v ? list_from_iter(*v) : V_none();
  int m = v ? ((List *)snap.o)->len : 0;
  int newlen = l->len - (int)count + m;
  Value *ni = xmalloc((newlen ? newlen : 1) * sizeof(Value));
  int k = 0;
  for (int i = 0; i < start; i++) ni[k++] = l->items[i];
  for (int i = 0; i < m; i++) ni[k++] = inc(((List *)snap.o)->items[i]);
  for (int i = (int)(start + count); i < l->len; i++) ni[k++] = l->items[i];
  for (int i = 0; i < count; i++) decref(l->items[start + i]);
  free(l->items); l->items = ni; l->len = newlen; l->cap = newlen ? newlen : 1;
  decref(snap);
}

int contains(Value c, Value item) {
  switch (c.t) {
  case T_LIST: case T_TUPLE: {
    List *l = (List *)c.o;
    for (int i = 0; i < l->len; i++) if (val_eq(l->items[i], item)) return 1;
    return 0;
  }
  case T_DICT: case T_SET: return dict_find((Dict *)c.o, item) != NULL;
  case T_STR: {
    if (item.t != T_STR) throw_error("TypeError", "'in <string>' requires string as left operand, not %s", type_name(item));
    Str *s = (Str *)c.o, *t = (Str *)item.o;
    if (t->len == 0) return 1;
    return memmem(s->s, s->len, t->s, t->len) != NULL;
  }
  case T_RANGE: {
    Range *r = (Range *)c.o;
    if (!is_intlike(item)) return 0;
    int64_t x = item.i;
    if (r->step > 0 ? (x < r->start || x >= r->stop) : (x > r->start || x <= r->stop)) return 0;
    return (x - r->start) % r->step == 0;
  }
  case T_INST: {
    Value out;
    if (call_dunder(c, "__contains__", &item, 1, &out)) { int r = val_truthy(out); decref(out); return r; }
    break;
  }
  default: break;
  }
  if (c.t == T_INST || c.t == T_ITER || c.t == T_GEN || c.t == T_FILE) {
    Iter it; iter_init(&it, c); Value x; int found = 0;
    while (iter_next(&it, &x)) { int eq = val_eq(x, item); decref(x); if (eq) { found = 1; break; } }
    iter_done(&it); return found;
  }
  throw_error("TypeError", "argument of type '%s' is not iterable", type_name(c));
}

static int cmp_op(int op, Value a, Value b) {
  switch (op) {
  case C_EQ: return val_eq(a, b);
  case C_NE: return !val_eq(a, b);
  case C_LT: case C_LE: case C_GT: case C_GE: return val_cmpop(op, a, b);
  case C_IN: return contains(b, a);
  case C_NOTIN: return !contains(b, a);
  case C_IS: case C_ISNOT: {
    int same;
    if (a.t != b.t) same = 0;
    else switch (a.t) {
      case T_NONE: same = 1; break;
      case T_BOOL: case T_INT: same = a.i == b.i; break;
      case T_FLOAT: same = memcmp(&a.d, &b.d, sizeof(double)) == 0; break;
      default: same = a.o == b.o; break;
    }
    return op == C_IS ? same : !same;
  }
  }
  return 0;
}

/* -------------------------------------------------------- functions */
static Value apply_decorators(Node *n, Env *env, Value obj) {
  for (int i = n->nv2 - 1; i >= 0; i--) {
    Value d = eval(n->v2[i], env);
    g_line = n->line;
    Value r = call_value(d, &obj, 1, NULL);
    decref(d); decref(obj); obj = r;
  }
  return obj;
}

static Value make_func(FuncDef *fd, Env *env) {
  Env *cl = env;
  while (cl->is_class && cl->parent) cl = cl->parent;
  Func *f = xmalloc(sizeof(Func));
  f->h.rc = 1; f->h.type = T_FUNC; f->def = fd; f->closure = cl; oinc(cl);
  f->owner = NULL; f->name = fd->name; oinc(fd->name); f->attrs = NULL;
  f->defaults = xmalloc((fd->nparams ? fd->nparams : 1) * sizeof(Value));
  for (int i = 0; i < fd->nparams; i++) f->defaults[i] = V_undef();
  for (int i = 0; i < fd->nparams; i++) if (fd->defaults[i]) f->defaults[i] = eval(fd->defaults[i], env);
  return V_obj(f, T_FUNC);
}

static void unbound_error(Node *n) {
  g_line = n->line;
  if (n->rk == RK_FREE) throw_error("NameError", "free variable '%s' referenced before assignment in enclosing scope", n->s->s);
  throw_error("UnboundLocalError", "cannot access local variable '%s' where it is not associated with a value", n->s->s);
}

static Value load_from_dict(Node *n, Dict *d) {
  Str *s = n->s; int h = n->gidx;
  if (h < d->n && d->e[h].key.t == T_STR && d->e[h].key.o == (Obj *)s) return inc(d->e[h].val);
  int ix = dict_index_str(d, s);
  if (ix >= 0) { n->gidx = ix; return inc(d->e[ix].val); }
  Value *v = dict_find_str(g_builtins, s);
  if (v) return inc(*v);
  g_line = n->line;
  throw_error("NameError", "name '%s' is not defined", s->s);
}

static Value load_name(Node *n, Env *env) {
  switch (n->rk) {
  case RK_LOCAL: {
    Value v = env->slots[n->ri];
    if (v.t == T_UNDEF) unbound_error(n);
    incref(v); return v;
  }
  case RK_FREE: {
    Env *e = env;
    for (int h = n->rh; h; h--) e = e->parent;
    Value v = e->slots[n->ri];
    if (v.t == T_UNDEF) unbound_error(n);
    incref(v); return v;
  }
  case RK_GLOBAL: return load_from_dict(n, env->globals->vars);
  case RK_CLASS: {
    Value *v = dict_find_str(env->vars, n->s);
    if (v) return inc(*v);
    switch (n->rk2) {
    case RK_FREE: {
      Env *e = env;
      for (int h = n->rh2; h; h--) e = e->parent;
      Value fv = e->slots[n->ri2];
      if (fv.t == T_UNDEF) unbound_error(n);
      incref(fv); return fv;
    }
    default: return load_from_dict(n, env->globals->vars);
    }
  }
  default: return load_from_dict(n, env->vars);
  }
}

static void store_name(Node *n, Value v, Env *env) {
  switch (n->rk) {
  case RK_LOCAL: { Value old = env->slots[n->ri]; env->slots[n->ri] = inc(v); decref(old); return; }
  case RK_FREE: {
    Env *e = env;
    for (int h = n->rh; h; h--) e = e->parent;
    Value old = e->slots[n->ri]; e->slots[n->ri] = inc(v); decref(old); return;
  }
  case RK_GLOBAL: dict_set_hint(env->globals->vars, n->s, v, &n->gidx); return;
  case RK_CLASS: dict_set_str(env->vars, n->s, v); return;
  default: dict_set_hint(env->vars, n->s, v, &n->gidx); return;
  }
}

/* store into the module/class dict by plain name (used by "from x import *") */
static void store_dynamic(Str *s, Value v, Env *env) {
  if (env->vars) dict_set_str(env->vars, s, v);
}

static void throw_missing(const char *fname, Str **names, int cnt, const char *what) {
  char list[512]; list[0] = 0; int len = 0;
  for (int i = 0; i < cnt; i++) {
    const char *sep = i == 0 ? "" : (i == cnt - 1 ? (cnt == 2 ? " and " : ", and ") : ", ");
    len += snprintf(list + len, sizeof list - len, "%s'%s'", sep, names[i]->s);
  }
  throw_error("TypeError", "%s() missing %d required %s argument%s: %s", fname, cnt, what, cnt == 1 ? "" : "s", list);
}

static Value call_func(Func *f, Value *args, int n, Kw *kw) {
  FuncDef *fd = f->def;
  if (g_depth >= g_max_depth + (g_raising ? 30 : 0)) throw_error("RecursionError", "maximum recursion depth exceeded");
  int star = fd->star, nn = fd->nnormal, nko = fd->nkwonly, kwstar = fd->kwstar;
  Env *e = env_new_func(f->closure, f->closure->globals, fd);
  Value *sl = e->slots;
  if (!kw && n <= nn && nko == 0 && kwstar < 0) {
    int i = 0;
    for (; i < n; i++) sl[i] = inc(args[i]);
    if (i < nn) {
      Str *missing[MAX_PARAMS]; int nm = 0;
      for (; i < nn; i++) {
        Value d = f->defaults[i];
        if (d.t == T_UNDEF) missing[nm++] = fd->params[i]; else sl[i] = inc(d);
      }
      if (nm) { odec(e); throw_missing(f->name->s, missing, nm, "positional"); }
    }
    if (star >= 0) { List *l = list_new(0); l->h.type = T_TUPLE; sl[star] = V_obj(l, T_TUPLE); }
  } else {
    if (n > nn && star < 0) {
      int nd = 0; for (int i = 0; i < nn; i++) if (f->defaults[i].t != T_UNDEF) nd++;
      odec(e);
      if (nd) throw_error("TypeError", "%s() takes from %d to %d positional arguments but %d %s given", f->name->s, nn - nd, nn, n, n == 1 ? "was" : "were");
      throw_error("TypeError", "%s() takes %d positional argument%s but %d %s given", f->name->s, nn, nn == 1 ? "" : "s", n, n == 1 ? "was" : "were");
    }
    for (int i = 0; i < n && i < nn; i++) sl[i] = inc(args[i]);
    if (star >= 0) {
      List *l = list_new(n > nn ? n - nn : 0); l->h.type = T_TUPLE;
      for (int i = nn; i < n; i++) list_append(l, args[i]);
      sl[star] = V_obj(l, T_TUPLE);
    }
    Dict *kwd = NULL;
    if (kwstar >= 0) { kwd = dict_new(); sl[kwstar] = V_obj(kwd, T_DICT); }
    int ko0 = nn + (star >= 0 ? 1 : 0);
    if (kw) for (int j = 0; j < kw->n; j++) {
      int p = -1;
      for (int i = 0; i < nn; i++) if (fd->params[i] == kw->names[j]) { p = i; break; }
      if (p < 0) for (int i = 0; i < nko; i++) if (fd->params[ko0 + i] == kw->names[j]) { p = ko0 + i; break; }
      if (p < 0) {
        if (kwd) { dict_set_str(kwd, kw->names[j], kw->vals[j]); continue; }
        const char *nm = kw->names[j]->s; odec(e);
        throw_error("TypeError", "%s() got an unexpected keyword argument '%s'", f->name->s, nm);
      }
      if (sl[p].t != T_UNDEF) { const char *nm = kw->names[j]->s; odec(e); throw_error("TypeError", "%s() got multiple values for argument '%s'", f->name->s, nm); }
      sl[p] = inc(kw->vals[j]);
    }
    Str *missing[MAX_PARAMS]; int nm = 0;
    for (int i = 0; i < nn; i++) if (sl[i].t == T_UNDEF) {
      if (f->defaults[i].t != T_UNDEF) sl[i] = inc(f->defaults[i]); else missing[nm++] = fd->params[i];
    }
    if (nm) { odec(e); throw_missing(f->name->s, missing, nm, "positional"); }
    for (int i = 0; i < nko; i++) {
      int p = ko0 + i;
      if (sl[p].t == T_UNDEF) {
        if (f->defaults[p].t != T_UNDEF) sl[p] = inc(f->defaults[p]); else missing[nm++] = fd->params[p];
      }
    }
    if (nm) { odec(e); throw_missing(f->name->s, missing, nm, "keyword-only"); }
  }

  if (fd->is_gen) return make_generator(f, e);
  Frame fr; fr.prev = g_frame; fr.fn = f; fr.env = e; fr.file = fd->file; fr.name = f->name->s; fr.line = 0; fr.exc = V_none();
  int saved_line = g_line;
  if (g_frame) g_frame->line = g_line;
  g_frame = &fr; g_depth++;
  Value ret;
  if (fd->expr) ret = eval(fd->expr, e);
  else {
    int st = exec_block(fd->body, fd->nbody, e);
    if (st == ST_RET) { ret = g_ret; g_ret = V_none(); } else ret = V_none();
  }
  g_depth--; g_frame = fr.prev; g_line = saved_line;
  odec(e);
  return ret;
}

static Value instantiate(Class *cls, Value *args, int n, Kw *kw) {
  Value *nw = class_lookup(cls, S_new);
  if (nw) {
    Value nf = nw->t == T_DESCR ? ((Descr *)nw->o)->fget : *nw;
    Value *a2 = alloca((n + 1) * sizeof(Value));
    a2[0] = V_obj(cls, T_CLASS); for (int i = 0; i < n; i++) a2[i + 1] = args[i];
    Value r = call_value(nf, a2, n + 1, kw);
    if (is_instance_of(r, cls)) {
      Value *init = class_lookup(cls, S_init);
      if (init) {
        Value *a3 = alloca((n + 1) * sizeof(Value));
        a3[0] = r; for (int i = 0; i < n; i++) a3[i + 1] = args[i];
        Value ir = call_value(*init, a3, n + 1, kw); decref(ir);
      }
    }
    return r;
  }
  Inst *in = xmalloc(sizeof(Inst));
  in->h.rc = 1; in->h.type = T_INST; in->cls = cls; oinc(cls); in->attrs = dict_new();
  Value iv = V_obj(in, T_INST);
  Value *init = class_lookup(cls, S_init);
  if (init) {
    Value *a = alloca((n + 1) * sizeof(Value));
    a[0] = iv; for (int i = 0; i < n; i++) a[i + 1] = args[i];
    Value r = call_value(*init, a, n + 1, kw); decref(r);
  } else if (n > 0 || (kw && kw->n)) throw_error("TypeError", "%s() takes no arguments", cls->name->s);
  return iv;
}

Value call_value(Value f, Value *args, int n, Kw *kw) {
  switch (f.t) {
  case T_BUILTIN: return ((Builtin *)f.o)->fn(args, n, kw);
  case T_FUNC: return call_func((Func *)f.o, args, n, kw);
  case T_BOUND: {
    Bound *b = (Bound *)f.o;
    Value *a = alloca((n + 1) * sizeof(Value));
    a[0] = b->self; for (int i = 0; i < n; i++) a[i + 1] = args[i];
    return call_value(b->func, a, n + 1, kw);
  }
  case T_CLASS: return instantiate((Class *)f.o, args, n, kw);
  case T_INST: {
    Value *m = class_lookup(((Inst *)f.o)->cls, INTERN("__call__"));
    if (m) {
      Value *a = alloca((n + 1) * sizeof(Value));
      a[0] = f; for (int i = 0; i < n; i++) a[i + 1] = args[i];
      return call_value(*m, a, n + 1, kw);
    }
  }
  /* fallthrough */
  default: throw_error("TypeError", "'%s' object is not callable", type_name(f));
  }
}

/* ---------------------------------------------------------- eval */
typedef struct { Value *v; int n, cap; Value inl[8]; } ArgVec;
static void av_init(ArgVec *a) { a->v = a->inl; a->n = 0; a->cap = 8; }
static void av_push(ArgVec *a, Value x) {
  if (a->n >= a->cap) {
    int nc = a->cap * 2;
    Value *nv = xmalloc(nc * sizeof(Value)); memcpy(nv, a->v, a->n * sizeof(Value));
    if (a->v != a->inl) free(a->v);
    a->v = nv; a->cap = nc;
  }
  a->v[a->n++] = x;
}
static void av_free(ArgVec *a) {
  for (int i = 0; i < a->n; i++) decref(a->v[i]);
  if (a->v != a->inl) free(a->v);
}


static inline Value EV(Node *n, Env *env) {
  if (n->k == N_CONST) { Value v = n->val; incref(v); return v; }
  if (n->k == N_NAME && n->rk == RK_LOCAL) {
    Value v = env->slots[n->ri];
    if (v.t != T_UNDEF) { incref(v); return v; }
  } else if (n->k == N_NAME && (n->rk == RK_GLOBAL || n->rk == RK_DYN)) {
    Dict *d = n->rk == RK_GLOBAL ? env->globals->vars : env->vars;
    int h = n->gidx;
    if (h < d->n && d->e[h].key.o == (Obj *)n->s && d->e[h].key.t == T_STR) { Value v = d->e[h].val; incref(v); return v; }
  }
  return eval(n, env);
}
static void assign_target(Node *t, Value v, Env *env);
static Value eval_call(Node *n, Env *env);

static void unpack_into(Node *t, Value v, Env *env) {
  int si = -1;
  for (int i = 0; i < t->nv; i++) if (t->v[i]->k == N_STAR) { si = i; break; }
  if (si >= 0) {
    Value lv = list_from_iter(v); List *l = (List *)lv.o;
    int fixed = t->nv - 1;
    if (l->len < fixed) { int got = l->len; decref(lv); throw_error("ValueError", "not enough values to unpack (expected at least %d, got %d)", fixed, got); }
    int rest = l->len - fixed;
    for (int i = 0; i < si; i++) assign_target(t->v[i], l->items[i], env);
    List *mid = list_new(rest);
    for (int i = 0; i < rest; i++) list_append(mid, l->items[si + i]);
    Value mv = V_obj(mid, T_LIST);
    assign_target(t->v[si]->a, mv, env); decref(mv);
    for (int i = si + 1; i < t->nv; i++) assign_target(t->v[i], l->items[rest + i - 1], env);
    decref(lv);
    return;
  }
  Value small[8]; Value *items = t->nv < 8 ? small : xmalloc((t->nv + 1) * sizeof(Value));
  int cnt = 0, cap = t->nv + 1, extra = 0;
  Iter it; iter_init(&it, v); Value x;
  while (iter_next(&it, &x)) {
    if (cnt >= cap) { decref(x); extra = 1; break; }
    items[cnt++] = x;
  }
  iter_done(&it);
  if (extra || cnt != t->nv) {
    int got = cnt;
    for (int i = 0; i < cnt; i++) decref(items[i]);
    if (items != small) free(items);
    if (got < t->nv) throw_error("ValueError", "not enough values to unpack (expected %d, got %d)", t->nv, got);
    throw_error("ValueError", "too many values to unpack (expected %d)", t->nv);
  }
  for (int i = 0; i < cnt; i++) assign_target(t->v[i], items[i], env);
  for (int i = 0; i < cnt; i++) decref(items[i]);
  if (items != small) free(items);
}

void eval_assign(Node *t, Value v, Env *env);
static void assign_target(Node *t, Value v, Env *env) {
  switch (t->k) {
  case N_NAME: store_name(t, v, env); return;
  case N_ATTR: { Value o = eval(t->a, env); set_attr(o, t->s, v); decref(o); return; }
  case N_INDEX: {
    Value o = eval(t->a, env);
    if (t->b->k == N_SLICE) {
      Node *sn = t->b;
      Value lo = sn->a ? eval(sn->a, env) : V_none(), hi = sn->b ? eval(sn->b, env) : V_none(), st = sn->c ? eval(sn->c, env) : V_none();
      slice_replace(o, lo, hi, st, &v);
      decref(lo); decref(hi); decref(st); decref(o); return;
    }
    Value i = eval(t->b, env); index_set(o, i, v); decref(o); decref(i); return;
  }
  case N_TUPLE: case N_LIST: unpack_into(t, v, env); return;
  default: throw_error("SyntaxError", "cannot assign to expression");
  }
}

static Value eval_slice(Node *n, Env *env, Value o) {
  Node *s = n->b;
  Value lo = s->a ? eval(s->a, env) : V_none();
  Value hi = s->b ? eval(s->b, env) : V_none();
  Value st = s->c ? eval(s->c, env) : V_none();
  Value r = slice_get(o, lo, hi, st);
  decref(lo); decref(hi); decref(st);
  return r;
}

static void comp_run(Node **cl, int ncl, int i, Node *elt, Node *elt2, Env *ce, Value out, int isdict, Value first) {
  if (i == ncl) {
    if (isdict == 1) { Value k = eval(elt, ce), v = eval(elt2, ce); dict_set((Dict *)out.o, k, v); decref(k); decref(v); }
    else if (isdict == 2) { Value k = eval(elt, ce); dict_set((Dict *)out.o, k, V_none()); decref(k); }
    else list_append_own((List *)out.o, eval(elt, ce));
    return;
  }
  Node *c = cl[i];
  if (c->k == N_COMPIF) {
    Value t = eval(c->a, ce); int ok = val_truthy(t); decref(t);
    if (ok) comp_run(cl, ncl, i + 1, elt, elt2, ce, out, isdict, first);
    return;
  }
  Value seq = i == 0 ? inc(first) : eval(c->b, ce);
  Iter it; iter_init(&it, seq); decref(seq);
  Value x;
  while (iter_next(&it, &x)) {
    assign_target(c->a, x, ce); decref(x);
    comp_run(cl, ncl, i + 1, elt, elt2, ce, out, isdict, first);
  }
  iter_done(&it);
}

static Value aug_apply(int op, Value cur, Value rhs) {
  if (op == OP_ADD && cur.t == T_LIST) {
    Iter it; iter_init(&it, rhs); Value x; List *l = (List *)cur.o;
    Value snapshot = (rhs.o == cur.o) ? list_from_iter(rhs) : V_none();
    if (snapshot.t != T_NONE) { iter_done(&it); iter_init(&it, snapshot); }
    while (iter_next(&it, &x)) list_append_own(l, x);
    iter_done(&it); decref(snapshot);
    return inc(cur);
  }
  if (cur.t == T_INST && op >= OP_ADD && op <= OP_SHR) {
    static const char *idun[] = {NULL, "__iadd__", "__isub__", "__imul__", "__itruediv__", "__ifloordiv__", "__imod__", "__ipow__", "__iand__", "__ior__", "__ixor__", "__ilshift__", "__irshift__"};
    Value out;
    if (call_dunder(cur, idun[op], &rhs, 1, &out)) return out;
  }
  if (cur.t == T_SET && ((Dict *)cur.o)->kind == DK_SET && (op == OP_BOR || op == OP_BAND || op == OP_SUB || op == OP_BXOR)) {
    Value r = binop(op, cur, rhs);       /* in-place semantics: mutate the left operand */
    Dict *d = (Dict *)cur.o, *nd = (Dict *)r.o;
    for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) { Value k = d->e[i].key, v = d->e[i].val; d->e[i].key.t = T_UNDEF; d->e[i].val = V_none(); decref(k); decref(v); }
    d->live = 0;
    for (int i = 0; i < nd->n; i++) if (nd->e[i].key.t != T_UNDEF) dict_set(d, nd->e[i].key, V_none());
    decref(r);
    return inc(cur);
  }
  return binop(op, cur, rhs);
}

static Value eval_call_general(Node *n, Env *env) {
  Value f = eval(n->a, env);
  ArgVec av; av_init(&av);
  Str *kn_small[8]; Value kv_small[8];
  Str **kn = kn_small; Value *kv = kv_small; int nk = 0, kcap = 8;
#define KW_PUSH(NAME, VAL) do { \
    if (nk >= kcap) { int nc = kcap * 2; Str **a2 = xmalloc(nc * sizeof(Str *)); Value *v2 = xmalloc(nc * sizeof(Value)); \
      memcpy(a2, kn, nk * sizeof(Str *)); memcpy(v2, kv, nk * sizeof(Value)); \
      if (kn != kn_small) { free(kn); free(kv); } kn = a2; kv = v2; kcap = nc; } \
    kn[nk] = (NAME); kv[nk] = (VAL); nk++; } while (0)
  for (int i = 0; i < n->nv; i++) {
    Node *an = n->v[i];
    if (an->k == N_STAR) {
      Value seq = eval(an->a, env); Iter it; iter_init(&it, seq); Value x;
      while (iter_next(&it, &x)) av_push(&av, x);
      iter_done(&it); decref(seq);
    } else if (an->k == N_DSTAR) {
      Value m = eval(an->a, env);
      if (m.t != T_DICT) throw_error("TypeError", "argument after ** must be a mapping, not %s", type_name(m));
      Dict *d = (Dict *)m.o;
      for (int q = 0; q < d->n; q++) {
        if (d->e[q].key.t == T_UNDEF) continue;
        if (d->e[q].key.t != T_STR) throw_error("TypeError", "keywords must be strings");
        Str *ks = (Str *)d->e[q].key.o;
        KW_PUSH(intern(ks->s, ks->len), inc(d->e[q].val));
      }
      decref(m);
    } else av_push(&av, eval(an, env));
  }
  for (int i = 0; i < n->nv2; i++) KW_PUSH(n->names[i], eval(n->v2[i], env));
  Kw kw; kw.n = nk; kw.names = kn; kw.vals = kv;
  g_line = n->line;
  Value r = call_value(f, av.v, av.n, nk ? &kw : NULL);
  av_free(&av);
  for (int i = 0; i < nk; i++) decref(kv[i]);
  if (kn != kn_small) { free(kn); free(kv); }
  decref(f);
  return r;
}

/* Method lookup without allocating a bound-method object. Returns the function to call
 * with `obj` as its first argument, or V_undef() if the generic path must be used. */
static Value method_fn(Value obj, Str *name) {
  if (obj.t == T_INST) {
    Inst *in = (Inst *)obj.o;
    if (in->attrs->live && dict_find_str(in->attrs, name)) return V_undef();
    Value *v = class_lookup(in->cls, name);
    if (v && v->t == T_FUNC) return *v;
    return V_undef();
  }
  Value *b = builtin_method_raw(obj, name);
  return b ? *b : V_undef();
}

static Value eval_call(Node *n, Env *env) {
  if (n->op == 0 && n->nv2 == 0 && n->nv <= 6) {
    Node *cn = n->a;
    Value argv[8]; int na = 0; Value f; int self_off = 0; Value obj = V_undef();
    if (cn->k == N_ATTR) {
      obj = eval(cn->a, env);
      g_line = cn->line;
      Value m = method_fn(obj, cn->s);
      if (m.t != T_UNDEF) { f = m; incref(f); argv[0] = obj; self_off = 1; obj = V_undef(); }
      else { f = get_attr(obj, cn->s); decref(obj); obj = V_undef(); }
    } else f = EV(cn, env);
    na = self_off;
    for (int i = 0; i < n->nv; i++) argv[na++] = EV(n->v[i], env);
    g_line = n->line;
    Value r = f.t == T_FUNC ? call_func((Func *)f.o, argv, na, NULL) : call_value(f, argv, na, NULL);
    for (int i = 0; i < na; i++) decref(argv[i]);
    decref(f);
    return r;
  }
  return eval_call_general(n, env);
}

static Value __attribute__((noinline)) eval_cold(Node *n, Env *env) {
  switch (n->k) {
  case N_CONST: return inc(n->val);
  case N_NAME: return load_name(n, env);
  case N_FSTR: {
    Buf b; buf_init(&b); buf_add(&b, "", 0);
    for (int i = 0; i < n->nv; i++) {
      Node *p = n->v[i];
      if (p->k == N_CONST) { Str *s = (Str *)p->val.o; buf_add(&b, s->s, s->len); continue; }
      Value v = eval(p->a, env);
      if (p->op == 'r') { Str *r = val_repr(v); decref(v); v = V_obj(r, T_STR); }
      else if (p->op == 's') { Str *r = val_str(v); decref(v); v = V_obj(r, T_STR); }
      Value f = format_value(v, p->s ? p->s->s : NULL);
      buf_add(&b, ((Str *)f.o)->s, ((Str *)f.o)->len);
      decref(f); decref(v);
    }
    return buf_to_str(&b);
  }
  case N_LIST: case N_TUPLE: {
    List *l = list_new(n->nv); l->h.type = n->k == N_LIST ? T_LIST : T_TUPLE;
    for (int i = 0; i < n->nv; i++) {
      Node *c = n->v[i];
      if (n->op == 1 && c->k == N_STAR) {
        Value seq = eval(c->a, env); Iter it; iter_init(&it, seq); Value x;
        while (iter_next(&it, &x)) list_append_own(l, x);
        iter_done(&it); decref(seq);
      } else list_append_own(l, eval(c, env));
    }
    return V_obj(l, l->h.type);
  }
  case N_SET: {
    Dict *d = dict_new(); d->kind = DK_SET; d->h.type = T_SET; Value dv = V_obj(d, T_SET);
    for (int i = 0; i < n->nv; i++) {
      Node *c = n->v[i];
      if (c->k == N_STAR) {
        Value seq = eval(c->a, env); Iter it; iter_init(&it, seq); Value x;
        while (iter_next(&it, &x)) { dict_set(d, x, V_none()); decref(x); }
        iter_done(&it); decref(seq);
      } else { Value k = eval(c, env); dict_set(d, k, V_none()); decref(k); }
    }
    return dv;
  }
  case N_DICT: {
    Dict *d = dict_new(); Value dv = V_obj(d, T_DICT);
    for (int i = 0; i < n->nv; i++) {
      if (!n->v[i]) {
        Value m = eval(n->v2[i], env);
        if (m.t != T_DICT) throw_error("TypeError", "'%s' object is not a mapping", type_name(m));
        Dict *sd = (Dict *)m.o;
        for (int q = 0; q < sd->n; q++) if (sd->e[q].key.t != T_UNDEF) dict_set(d, sd->e[q].key, sd->e[q].val);
        decref(m); continue;
      }
      Value k = eval(n->v[i], env), v = eval(n->v2[i], env);
      dict_set(d, k, v); decref(k); decref(v);
    }
    return dv;
  }
  case N_WALRUS: { Value v = eval(n->b, env); store_name(n->a, v, env); return v; }
  case N_YIELD: { Value v = n->a ? eval(n->a, env) : V_none(); g_line = n->line; return gen_yield(v); }
  case N_YIELDFROM: {
    Value src = eval(n->a, env); Value it = make_iterator(src); decref(src); Value x;
    g_line = n->line;
    while (iter_step(it, &x)) { Value sv = gen_yield(x); decref(sv); }
    Value r = it.t == T_GEN ? gen_retval(it) : V_none();
    decref(it); return r;
  }
  case N_GENEXP: {
    Value first = eval(n->v[0]->b, env);
    Env *par = env; while (par->is_class && par->parent) par = par->parent;
    Env *ce = env_new_func(par, env->globals, n->fd);
    Value it = make_comp_iter(n, ce, first);
    decref(first); odec(ce);
    return it;
  }
  case N_BINOP: {
    Value a = eval(n->a, env), b = eval(n->b, env);
    if (a.t == T_INT && b.t == T_INT) {
      int64_t r;
      switch (n->op) {
      case OP_ADD: if (!__builtin_add_overflow(a.i, b.i, &r)) return V_int(r); break;
      case OP_SUB: if (!__builtin_sub_overflow(a.i, b.i, &r)) return V_int(r); break;
      case OP_MUL: if (!__builtin_mul_overflow(a.i, b.i, &r)) return V_int(r); break;
      case OP_MOD: if (b.i > 0 && a.i >= 0) return V_int(a.i % b.i); break;
      case OP_FDIV: if (b.i > 0 && a.i >= 0) return V_int(a.i / b.i); break;
      default: break;
      }
    }
    g_line = n->line;
    Value r = binop(n->op, a, b); decref(a); decref(b); return r;
  }
  case N_UNARY: { Value a = eval(n->a, env); g_line = n->line; Value r = unaryop(n->op, a); decref(a); return r; }
  case N_AND: { Value a = eval(n->a, env); if (!val_truthy(a)) return a; decref(a); return eval(n->b, env); }
  case N_OR: { Value a = eval(n->a, env); if (val_truthy(a)) return a; decref(a); return eval(n->b, env); }
  case N_NOT: { Value a = eval(n->a, env); int t = val_truthy(a); decref(a); return V_bool(!t); }
  case N_CMP: {
    Value left = eval(n->a, env);
    if (n->nv == 1) {
      Value right = eval(n->v[0], env);
      if (left.t == T_INT && right.t == T_INT) {
        switch (n->ops[0]) {
        case C_EQ: return V_bool(left.i == right.i);
        case C_NE: return V_bool(left.i != right.i);
        case C_LT: return V_bool(left.i < right.i);
        case C_LE: return V_bool(left.i <= right.i);
        case C_GT: return V_bool(left.i > right.i);
        case C_GE: return V_bool(left.i >= right.i);
        default: break;
        }
      }
      g_line = n->line;
      int ok = cmp_op(n->ops[0], left, right);
      decref(left); decref(right);
      return V_bool(ok);
    }
    for (int i = 0; i < n->nv; i++) {
      Value right = eval(n->v[i], env);
      g_line = n->line;
      int ok = cmp_op(n->ops[i], left, right);
      decref(left);
      if (!ok) { decref(right); return V_bool(0); }
      left = right;
    }
    decref(left);
    return V_bool(1);
  }
  case N_IFEXP: { Value c = eval(n->a, env); int t = val_truthy(c); decref(c); return eval(t ? n->b : n->c, env); }
  case N_LAMBDA: return make_func(n->fd, env);
  case N_ATTR: { Value o = eval(n->a, env); g_line = n->line; Value r = get_attr(o, n->s); decref(o); return r; }
  case N_INDEX: {
    Value o = eval(n->a, env), r;
    if (n->b->k == N_SLICE) { g_line = n->line; r = eval_slice(n, env, o); }
    else { Value i = eval(n->b, env); g_line = n->line; r = index_get(o, i); decref(i); }
    decref(o); return r;
  }
  case N_CALL: return eval_call(n, env);
  case N_LISTCOMP: case N_DICTCOMP: case N_SETCOMP: {
    Value first = eval(n->v[0]->b, env);
    Env *par = env; while (par->is_class && par->parent) par = par->parent;
    Env *ce = env_new_func(par, env->globals, n->fd);
    int isd = n->k == N_DICTCOMP ? 1 : n->k == N_SETCOMP ? 2 : 0;
    Value out;
    if (isd == 2) { Dict *sd = dict_new(); sd->kind = DK_SET; sd->h.type = T_SET; out = V_obj(sd, T_SET); }
    else out = isd ? V_obj(dict_new(), T_DICT) : V_list_new();
    comp_run(n->v, n->nv, 0, n->a, n->b, ce, out, isd, first);
    decref(first);
    odec(ce);
    return out;
  }
  default:
    throw_error("RuntimeError", "cannot evaluate node kind %d", n->k);
  }
}


static int eval_cond(Node *n, Env *env);

Value eval(Node *n, Env *env) {
  switch (n->k) {
  case N_CONST: return inc(n->val);
  case N_NAME: return load_name(n, env);
  case N_BINOP: {
    Value a = EV(n->a, env), b = EV(n->b, env);
    if (a.t == T_INT && b.t == T_INT) {
      int64_t r;
      switch (n->op) {
      case OP_ADD: if (!__builtin_add_overflow(a.i, b.i, &r)) return V_int(r); break;
      case OP_SUB: if (!__builtin_sub_overflow(a.i, b.i, &r)) return V_int(r); break;
      case OP_MUL: if (!__builtin_mul_overflow(a.i, b.i, &r)) return V_int(r); break;
      case OP_MOD: if (b.i > 0 && a.i >= 0) return V_int(a.i % b.i); break;
      case OP_FDIV: if (b.i > 0 && a.i >= 0) return V_int(a.i / b.i); break;
      default: break;
      }
    }
    g_line = n->line;
    Value r = binop(n->op, a, b); decref(a); decref(b); return r;
  }
  case N_CMP: return V_bool(eval_cond(n, env));
  case N_CALL: return eval_call(n, env);
  case N_IFEXP: return eval(eval_cond(n->a, env) ? n->b : n->c, env);
  case N_ATTR: { Value o = eval(n->a, env); g_line = n->line; Value r = get_attr(o, n->s); decref(o); return r; }
  default: return eval_cold(n, env);
  }
}

/* truthiness of an expression without materialising bool values in the common cases */
static int eval_cond(Node *n, Env *env) {
  switch (n->k) {
  case N_CMP:
    if (n->nv == 1) {
      Value l = EV(n->a, env), r = EV(n->v[0], env);
      if (l.t == T_INT && r.t == T_INT) {
        switch (n->ops[0]) {
        case C_EQ: return l.i == r.i; case C_NE: return l.i != r.i;
        case C_LT: return l.i < r.i; case C_LE: return l.i <= r.i;
        case C_GT: return l.i > r.i; case C_GE: return l.i >= r.i;
        default: break;
        }
      }
      g_line = n->line;
      int ok = cmp_op(n->ops[0], l, r);
      decref(l); decref(r);
      return ok;
    }
    { Value v = eval_cold(n, env); int t = val_truthy(v); decref(v); return t; }
  case N_NOT: return !eval_cond(n->a, env);
  case N_AND: return eval_cond(n->a, env) && eval_cond(n->b, env);
  case N_OR: return eval_cond(n->a, env) || eval_cond(n->b, env);
  default: { Value v = EV(n, env); int t = val_truthy(v); decref(v); return t; }
  }
}

/* ------------------------------------------------------ statements */
static Value *name_slot(Node *t, Env *env) {
  switch (t->rk) {
  case RK_LOCAL: return &env->slots[t->ri];
  case RK_FREE: { Env *e = env; for (int h = t->rh; h; h--) e = e->parent; return &e->slots[t->ri]; }
  case RK_GLOBAL: return dict_find_str(env->globals->vars, t->s);
  case RK_CLASS: return dict_find_str(env->vars, t->s);
  default: return dict_find_str(env->vars, t->s);
  }
}

static int exec_stmt(Node *n, Env *env);
static int exec_cold(Node *n, Env *env);
static int exec_cold(Node *n, Env *env);

int exec_block(Node **v, int n, Env *env) {
  for (int i = 0; i < n; i++) { int s = exec_stmt(v[i], env); if (s) return s; }
  return 0;
}

static int exec_try_except(Node *n, Env *env) {
  if (n->nv2 == 0) return exec_block(n->v, n->nv, env);
  Handler h; volatile int status = 0; volatile int caught = 0;
  h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) { status = exec_block(n->v, n->nv, env); g_handler = h.prev; }
  else { g_handler = h.prev; g_frame = h.frame; g_depth = h.depth; caught = 1; }
  if (!caught) {
    if (status == 0 && n->c) return exec_block(n->c->v, n->c->nv, env);
    return status;
  }
  Value exc = h.exc;
  for (int i = 0; i < n->nv2; i++) {
    Node *hn = n->v2[i]; int match = 1;
    if (hn->a) { Value t = eval(hn->a, env); match = exc_matches(exc, t); decref(t); }
    if (!match) continue;
    clear_traceback();
    Value saved_exc = g_frame->exc;
    g_frame->exc = inc(exc);
    if (hn->b) store_name(hn->b, exc, env);
    int st = exec_block(hn->v, hn->nv, env);
    decref(g_frame->exc); g_frame->exc = saved_exc;
    decref(exc);
    return st;
  }
  throw_value(exc);
}

static int exec_try(Node *n, Env *env) {
  if (!n->d) return exec_try_except(n, env);
  Handler h; volatile int status = 0; volatile int caught = 0;
  h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) { status = exec_try_except(n, env); g_handler = h.prev; }
  else { g_handler = h.prev; g_frame = h.frame; g_depth = h.depth; caught = 1; }
  Value saved_ret = g_ret;
  int fs = exec_block(n->d->v, n->d->nv, env);
  if (fs) { if (caught) decref(h.exc); return fs; }
  g_ret = saved_ret;
  if (caught) throw_value(h.exc);
  return status;
}

static int exec_with(Node *n, Env *env) {
  Value ctx = eval(n->a, env), enter;
  if (ctx.t == T_FILE) enter = inc(ctx);
  else if (ctx.t == T_INST) {
    if (!call_dunder(ctx, "__enter__", NULL, 0, &enter)) throw_error("AttributeError", "'%s' object does not support the context manager protocol", type_name(ctx));
  } else throw_error("AttributeError", "'%s' object does not support the context manager protocol", type_name(ctx));
  if (n->b) store_name(n->b, enter, env);
  decref(enter);
  Handler h; volatile int status = 0; volatile int caught = 0;
  h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) { status = exec_block(n->v, n->nv, env); g_handler = h.prev; }
  else { g_handler = h.prev; g_frame = h.frame; g_depth = h.depth; caught = 1; }
  int suppress = 0;
  if (ctx.t == T_FILE) {
    File *f = (File *)ctx.o;
    if (!f->closed && f->fp) fclose(f->fp);
    f->closed = 1;
  } else {
    Value args[3], out;
    args[0] = caught ? inc(V_obj(((Inst *)h.exc.o)->cls, T_CLASS)) : V_none();
    args[1] = caught ? h.exc : V_none(); args[2] = V_none();
    if (call_dunder(ctx, "__exit__", args, 3, &out)) { suppress = caught && val_truthy(out); decref(out); }
    if (caught) decref(args[0]);
  }
  decref(ctx);
  if (caught) { if (suppress) { clear_traceback(); decref(h.exc); return 0; } throw_value(h.exc); }
  return status;
}

static void do_del(Node *t, Env *env) {
  switch (t->k) {
  case N_NAME:
    if (t->rk == RK_LOCAL || t->rk == RK_FREE) {
      Env *e = env;
      if (t->rk == RK_FREE) for (int h = t->rh; h; h--) e = e->parent;
      Value *slot = &e->slots[t->ri];
      if (slot->t == T_UNDEF) unbound_error(t);
      Value old = *slot; *slot = V_undef(); decref(old); return;
    }
    if (!dict_del(t->rk == RK_GLOBAL ? env->globals->vars : env->vars, V_obj(t->s, T_STR))) throw_error("NameError", "name '%s' is not defined", t->s->s);
    return;
  case N_INDEX: {
    Value o = eval(t->a, env);
    if (t->b->k == N_SLICE) {
      Node *sn = t->b;
      Value lo = sn->a ? eval(sn->a, env) : V_none(), hi = sn->b ? eval(sn->b, env) : V_none(), st = sn->c ? eval(sn->c, env) : V_none();
      slice_replace(o, lo, hi, st, NULL);
      decref(lo); decref(hi); decref(st); decref(o); return;
    }
    Value i = eval(t->b, env);
    if (o.t == T_LIST) {
      List *l = (List *)o.o; int64_t k = norm_index(as_index(i), l->len, "list assignment");
      Value old = l->items[k];
      memmove(l->items + k, l->items + k + 1, (l->len - k - 1) * sizeof(Value)); l->len--;
      decref(old);
    } else if (o.t == T_DICT) {
      if (!dict_del((Dict *)o.o, i)) throw_keyerror(i);
    } else throw_error("TypeError", "'%s' object doesn't support item deletion", type_name(o));
    decref(o); decref(i); return;
  }
  case N_ATTR: {
    Value o = eval(t->a, env);
    Dict *d = o.t == T_INST ? ((Inst *)o.o)->attrs : o.t == T_CLASS ? ((Class *)o.o)->attrs : NULL;
    if (!d || !dict_del(d, V_obj(t->s, T_STR))) throw_error("AttributeError", "%s", t->s->s);
    decref(o); return;
  }
  default: throw_error("SyntaxError", "cannot delete expression");
  }
}

Value import_module(Str *name);
static Str *resolve_relative(Str *mod, int level, Env *env);

static int __attribute__((noinline)) exec_cold(Node *n, Env *env) {
  g_line = n->line;
  switch (n->k) {
  case N_EXPR: { Value v = eval(n->a, env); decref(v); return 0; }
  case N_PASS: return 0;
  case N_ASSIGN: {
    Value v = eval(n->a, env);
    for (int i = 0; i < n->nv; i++) assign_target(n->v[i], v, env);
    decref(v); return 0;
  }
  case N_AUG: {
    Node *t = n->a; Value cur, rhs, res;
    if (t->k == N_NAME) {
      cur = load_name(t, env); rhs = eval(n->b, env); g_line = n->line;
      if (n->op == OP_ADD && cur.t == T_STR && rhs.t == T_STR && cur.o->rc == 2) {
        Value *slot = name_slot(t, env);
        if (slot && slot->t == T_STR && slot->o == cur.o) {
          Str *s0 = (Str *)cur.o, *r0 = (Str *)rhs.o;
          Str *ns = xrealloc(s0, sizeof(Str) + s0->len + r0->len + 1);
          memcpy(ns->s + ns->len, r0->s, r0->len); ns->len += r0->len; ns->s[ns->len] = 0;
          ns->ascii = ns->ascii && r0->ascii; ns->cplen += r0->cplen; ns->hash = 0;
          slot->o = (Obj *)ns; ns->h.rc = 1;   /* only the variable holds it now */
          decref(rhs);
          return 0;
        }
      }
      res = aug_apply(n->op, cur, rhs); store_name(t, res, env);
    } else if (t->k == N_ATTR) {
      Value o = eval(t->a, env); cur = get_attr(o, t->s); rhs = eval(n->b, env); g_line = n->line;
      res = aug_apply(n->op, cur, rhs); set_attr(o, t->s, res); decref(o);
    } else {
      Value o = eval(t->a, env), i = eval(t->b, env);
      cur = index_get(o, i); rhs = eval(n->b, env); g_line = n->line;
      res = aug_apply(n->op, cur, rhs); index_set(o, i, res); decref(o); decref(i);
    }
    decref(cur); decref(rhs); decref(res);
    return 0;
  }
  case N_IF: {
    Value c = eval(n->a, env); int t = val_truthy(c); decref(c);
    return t ? exec_block(n->v, n->nv, env) : exec_block(n->v2, n->nv2, env);
  }
  case N_WHILE: {
    int broke = 0;
    for (;;) {
      if (!eval_cond(n->a, env)) break;
      int s = exec_block(n->v, n->nv, env);
      if (s == ST_BREAK) { broke = 1; break; }
      if (s == ST_RET) return s;
    }
    if (!broke && n->nv2) return exec_block(n->v2, n->nv2, env);
    return 0;
  }
  case N_FOR: {
    Value seq = eval(n->b, env);
    g_line = n->line;
    if (seq.t == T_RANGE) {
      Range *r = (Range *)seq.o; int64_t cur = r->start, stp = r->step, stop = r->stop; int st = 0, broke = 0;
      for (; stp > 0 ? cur < stop : cur > stop; cur += stp) {
        assign_target(n->a, V_int(cur), env);
        int s2 = exec_block(n->v, n->nv, env);
        if (s2 == ST_BREAK) { broke = 1; break; }
        if (s2 == ST_RET) { st = s2; broke = 1; break; }
      }
      decref(seq);
      if (!broke && n->nv2) return exec_block(n->v2, n->nv2, env);
      return st;
    }
    Iter it; iter_init(&it, seq); decref(seq);
    Value x; int st = 0, broke = 0;
    while (iter_next(&it, &x)) {
      assign_target(n->a, x, env); decref(x);
      int s = exec_block(n->v, n->nv, env);
      if (s == ST_BREAK) { broke = 1; break; }
      if (s == ST_RET) { st = s; broke = 1; break; }
    }
    iter_done(&it);
    if (!broke && n->nv2) return exec_block(n->v2, n->nv2, env);
    return st;
  }
  case N_DEF: { Value f = apply_decorators(n, env, make_func(n->fd, env)); store_name(n->a, f, env); decref(f); return 0; }
  case N_CLASS: {
    Value bvals[16]; Class *bases[16]; int nb = 0;
    if (n->a) for (int i = 0; i < n->a->nv; i++) {
      if (nb >= 16) throw_error("TypeError", "too many base classes");
      Value bv = eval(n->a->v[i], env);
      if (bv.t == T_INST) { decref(bv); continue; }   /* typing.Generic[T] and friends */
      if (bv.t != T_CLASS) throw_error("TypeError", "bases must be classes, not %s", type_name(bv));
      bvals[nb] = bv; bases[nb] = (Class *)bv.o; nb++;
    }
    Env *ce = env_new_dict(env, env->globals); ce->is_class = 1;
    exec_block(n->v, n->nv, ce);
    oinc(ce->vars);
    if (nb == 0 && C_object) { bases[0] = C_object; nb = 1; bvals[0] = V_none(); }
    Class *c = make_class(n->s, bases, nb, ce->vars);
    if (!C_object && n->s->len == 6 && !strcmp(n->s->s, "object")) { C_object = c; oinc(c); }
    for (int i = 0; i < nb; i++) if (bases[i] != C_object || bvals[i].t != T_NONE) decref(bvals[i]);
    odec(ce);
    for (int i = 1; i < c->nmro; i++) {
      Value *hk = dict_find_str(c->mro[i]->attrs, S_init_subclass);
      if (hk) {
        Value cls_v = V_obj(c, T_CLASS);
        Value fnv = hk->t == T_DESCR ? ((Descr *)hk->o)->fget : *hk;
        Value r = call_value(fnv, &cls_v, 1, NULL); decref(r);
        break;
      }
    }
    Value cv = apply_decorators(n, env, V_obj(c, T_CLASS));
    store_name(n->b, cv, env); decref(cv);
    return 0;
  }
  case N_ANNASSIGN: {
    if (env->is_class && n->a->k == N_NAME) {
      Value *ann = dict_find_cstr(env->vars, "__annotations__");
      if (!ann) { Value d = V_obj(dict_new(), T_DICT); dict_set_cstr(env->vars, "__annotations__", d); decref(d); ann = dict_find_cstr(env->vars, "__annotations__"); }
      dict_set((Dict *)ann->o, V_obj(n->a->s, T_STR), V_none());
    }
    if (n->b) {
      Value v = eval(n->b, env);
      if (n->a->k == N_NAME) store_name(n->a, v, env);
      else { Value o = eval(n->a->a, env); set_attr(o, n->a->s, v); decref(o); }
      decref(v);
    }
    return 0;
  }
  case N_RETURN: g_ret = n->a ? eval(n->a, env) : V_none(); return ST_RET;
  case N_BREAK: return ST_BREAK;
  case N_CONTINUE: return ST_CONT;
  case N_TRY: return exec_try(n, env);
  case N_WITH: return exec_with(n, env);
  case N_RAISE: {
    if (!n->a) {
      Value cur = g_frame ? g_frame->exc : V_none();
      if (cur.t == T_NONE) throw_error("RuntimeError", "No active exception to reraise");
      throw_value(inc(cur));
    }
    Value e = eval(n->a, env);
    if (e.t == T_CLASS) { Value r = call_value(e, NULL, 0, NULL); decref(e); e = r; }
    g_line = n->line;
    if (e.t != T_INST || !is_instance_of(e, C_BaseException)) throw_error("TypeError", "exceptions must derive from BaseException");
    throw_value(e);
  }
  case N_ASSERT: {
    Value c = eval(n->a, env); int t = val_truthy(c); decref(c);
    if (!t) {
      if (n->b) { Value m = eval(n->b, env); Str *s = val_str(m); char tmp[512]; snprintf(tmp, sizeof tmp, "%s", s->s); odec(s); decref(m); throw_error("AssertionError", "%s", tmp); }
      throw_error("AssertionError", "%s", "");
    }
    return 0;
  }
  case N_IMPORT: {
    Value m = import_module(n->s);
    if (!n->s2) {   /* "import a.b.c" binds the top-level package "a" */
      const char *dot = strchr(n->s->s, '.');
      if (dot) { Value top = import_module(intern(n->s->s, (int)(dot - n->s->s))); decref(m); m = top; }
    }
    store_name(n->a, m, env); decref(m); return 0;
  }
  case N_FROM: {
    Str *full = resolve_relative(n->s, n->op, env);
    Value m = import_module(full);
    if (m.t != T_MODULE) throw_error("ImportError", "cannot import from '%s'", full->s);
    Module *mod = (Module *)m.o;
    for (int i = 0; i < n->nv; i++) {
      if (!strcmp(n->names[i]->s, "*")) {
        for (int k = 0; k < mod->attrs->n; k++) {
          DEntry *e = &mod->attrs->e[k];
          if (e->key.t == T_STR && ((Str *)e->key.o)->s[0] != '_') store_dynamic((Str *)e->key.o, e->val, env);
        }
        continue;
      }
      Value *v = dict_find_str(mod->attrs, n->names[i]);
      if (!v) {   /* maybe a submodule: from pkg import sub */
        char sub[1200]; snprintf(sub, sizeof sub, "%s.%s", full->s, n->names[i]->s);
        Value sm = import_module(intern(sub, (int)strlen(sub)));
        store_name(n->v[i], sm, env); decref(sm); continue;
      }
      store_name(n->v[i], *v, env);
    }
    decref(m); return 0;
  }
  case N_DEL: do_del(n->a, env); return 0;
  default: throw_error("RuntimeError", "cannot execute node kind %d", n->k);
  }
}


static int exec_stmt(Node *n, Env *env) {
  switch (n->k) {
  case N_EXPR: g_line = n->line; { Value v = eval(n->a, env); decref(v); } return 0;
  case N_RETURN: g_line = n->line; g_ret = n->a ? eval(n->a, env) : V_none(); return ST_RET;
  case N_IF: {
    g_line = n->line;
    return eval_cond(n->a, env) ? exec_block(n->v, n->nv, env) : exec_block(n->v2, n->nv2, env);
  }
  case N_ASSIGN:
    if (n->nv == 1 && n->v[0]->k == N_NAME) {
      g_line = n->line;
      Value v = eval(n->a, env); store_name(n->v[0], v, env); decref(v); return 0;
    }
    return exec_cold(n, env);
  case N_PASS: return 0;
  case N_BREAK: return ST_BREAK;
  case N_CONTINUE: return ST_CONT;
  default: return exec_cold(n, env);
  }
}

/* ---------------------------------------------------------- import */
char *read_file(const char *path, long *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = xmalloc(sz + 1);
  size_t got = fread(buf, 1, sz, f); buf[got] = 0; fclose(f);
  if (len) *len = (long)got;
  return buf;
}

void run_module_source(const char *src, const char *file, Env *env) {
  int n; Node **prog = parse_program(src, file, &n);
  Frame fr; fr.prev = g_frame; fr.fn = NULL; fr.env = env; fr.file = file; fr.name = "<module>"; fr.line = 0; fr.exc = V_none();
  if (g_frame) g_frame->line = g_line;
  int saved = g_line;
  g_frame = &fr;
  exec_block(prog, n, env);
  g_frame = fr.prev; g_line = saved;
}

extern const struct StdlibEntry { const char *name; const char *src; } g_stdlib[];
static Value g_sys_path;

Value get_sys_path(void) {
  if (g_sys_path.t != T_LIST) g_sys_path = V_list_new();
  return g_sys_path;
}
void interp_setup_path(const char *dir) {
  Value pl = get_sys_path();
  List *l = (List *)pl.o;
  for (int i = 0; i < l->len; i++) decref(l->items[i]);
  l->len = 0;
  list_append_own(l, V_str(dir));
  if (strcmp(dir, ".")) list_append_own(l, V_str("."));
  const char *cp = getenv("CPY_PATH");
  if (cp) { char *cc = xstrdup(cp); for (char *tok = strtok(cc, ":"); tok; tok = strtok(NULL, ":")) if (*tok) list_append_own(l, V_str(tok)); free(cc); }
  snprintf(g_script_dir, 1024, "%s", dir);
}

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }

/* returns malloc'd source and fills path/is_pkg, or NULL */
static char *find_module_source(const char *dotted, char *path, size_t psz, int *is_pkg) {
  char rel[1024]; size_t k = 0;
  for (const char *p = dotted; *p && k < sizeof rel - 1; p++) rel[k++] = *p == '.' ? '/' : *p;
  rel[k] = 0;
  List *sp = (List *)get_sys_path().o;
  static const char *exts[] = {".cpy", ".py"};
  for (int i = 0; i < sp->len; i++) {
    if (sp->items[i].t != T_STR) continue;
    const char *dir = ((Str *)sp->items[i].o)->s;
    for (int e = 0; e < 2; e++) {
      snprintf(path, psz, "%s/%s%s", dir, rel, exts[e]);
      if (file_exists(path)) { *is_pkg = 0; return read_file(path, NULL); }
    }
    for (int e = 0; e < 2; e++) {
      snprintf(path, psz, "%s/%s/__init__%s", dir, rel, exts[e]);
      if (file_exists(path)) { *is_pkg = 1; return read_file(path, NULL); }
    }
  }
  return NULL;
}

Value import_module(Str *name) {
  Value *cached = dict_find_str(g_modules, name);
  if (cached) return inc(*cached);
  const char *dot = strrchr(name->s, '.');
  Value parent = V_undef(); Str *leaf = NULL;
  if (dot) {
    Str *pn = intern(name->s, (int)(dot - name->s));
    leaf = intern(dot + 1, (int)strlen(dot + 1));
    parent = import_module(pn);
    if (parent.t == T_MODULE) {
      Value *attr = dict_find_str(((Module *)parent.o)->attrs, leaf);
      if (attr && attr->t == T_MODULE) { dict_set_str(g_modules, name, *attr); decref(parent); return inc(*attr); }
    }
  }
  Value bm = builtin_module(name->s);
  if (bm.t != T_UNDEF) { dict_set_str(g_modules, name, bm); if (parent.t == T_MODULE) dict_set_str(((Module *)parent.o)->attrs, leaf, bm); decref(parent); return bm; }
  char path[2048]; int is_pkg = 0; char *src = find_module_source(name->s, path, sizeof path, &is_pkg);
  const char *fname = path;
  if (!src) {
    for (int i = 0; g_stdlib[i].name; i++) if (!strcmp(g_stdlib[i].name, name->s)) { src = xstrdup(g_stdlib[i].src); snprintf(path, sizeof path, "<stdlib:%s>", name->s); fname = path; break; }
  }
  if (!src) { decref(parent); throw_error("ImportError", "No module named '%s'", name->s); }
  Env *menv = env_new_dict(NULL, NULL);
  { Value nm = V_str(name->s); dict_set_str(menv->vars, S_name, nm); decref(nm);
    Value fv = V_str(fname); dict_set_cstr(menv->vars, "__file__", fv); decref(fv);
    Value pk;
    if (is_pkg) pk = V_str(name->s);
    else if (dot) pk = V_strn(name->s, (int)(dot - name->s));
    else pk = V_str("");
    dict_set_cstr(menv->vars, "__package__", pk); decref(pk); }
  Module *m = xmalloc(sizeof(Module));
  m->h.rc = 1; m->h.type = T_MODULE; m->name = name; oinc(name); m->attrs = menv->vars; oinc(menv->vars);
  Value mv = V_obj(m, T_MODULE);
  dict_set_str(g_modules, name, mv);
  Handler h; h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) != 0) {           /* failed import: forget the half-initialised module */
    g_handler = h.prev; g_frame = h.frame; g_depth = h.depth;
    dict_del(g_modules, V_obj(name, T_STR));
    throw_value(h.exc);
  }
  run_module_source(src, xstrdup(fname), menv);
  g_handler = h.prev;
  free(src);
  odec(menv);
  if (parent.t == T_MODULE) dict_set_str(((Module *)parent.o)->attrs, leaf, mv);
  decref(parent);
  return mv;
}

/* resolve "from ..x import y" style names */
static Str *resolve_relative(Str *mod, int level, Env *env) {
  if (level == 0) return mod;
  Value *pk = dict_find_cstr(env->globals->vars, "__package__");
  if (!pk || pk->t != T_STR || ((Str *)pk->o)->len == 0) throw_error("ImportError", "attempted relative import with no known parent package");
  char base[1024]; snprintf(base, sizeof base, "%s", ((Str *)pk->o)->s);
  for (int i = 1; i < level; i++) { char *d = strrchr(base, '.'); if (!d) throw_error("ImportError", "attempted relative import beyond top-level package"); *d = 0; }
  char full[1200];
  if (mod->len) snprintf(full, sizeof full, "%s.%s", base, mod->s); else snprintf(full, sizeof full, "%s", base);
  return intern(full, (int)strlen(full));
}

/* ----------------------------------------------------------- init */
static const char *PRELUDE =
  "class object:\n    pass\n"
  "class BaseException:\n"
  "    def __init__(self, msg=\"\"):\n        self.msg = msg\n"
  "    def __str__(self):\n        return str(self.msg)\n"
  "class Exception(BaseException): pass\n"
  "class ArithmeticError(Exception): pass\n"
  "class ZeroDivisionError(ArithmeticError): pass\n"
  "class OverflowError(ArithmeticError): pass\n"
  "class LookupError(Exception): pass\n"
  "class IndexError(LookupError): pass\n"
  "class KeyError(LookupError):\n    def __str__(self):\n        return repr(self.msg)\n"
  "class ValueError(Exception): pass\n"
  "class TypeError(Exception): pass\n"
  "class NameError(Exception): pass\n"
  "class UnboundLocalError(NameError): pass\n"
  "class AttributeError(Exception): pass\n"
  "class RuntimeError(Exception): pass\n"
  "class RecursionError(RuntimeError): pass\n"
  "class NotImplementedError(RuntimeError): pass\n"
  "class ImportError(Exception): pass\n"
  "class SyntaxError(Exception): pass\n"
  "class AssertionError(Exception): pass\n"
  "class StopIteration(Exception): pass\n"
  "class EOFError(Exception): pass\n"
  "class OSError(Exception): pass\n"
  "class FileNotFoundError(OSError): pass\n"
  "class KeyboardInterrupt(BaseException): pass\n"
  "class GeneratorExit(BaseException): pass\n"
  "class SystemExit(BaseException): pass\n"
  "IOError = OSError\n";

void interp_init(void) {
  S_init = INTERN("__init__"); S_name = INTERN("__name__"); S_str = INTERN("__str__"); S_repr = INTERN("__repr__");
  S_class = INTERN("__class__"); S_dict = INTERN("__dict__"); S_mro = INTERN("__mro__"); S_bases = INTERN("__bases__");
  S_setter = INTERN("setter"); S_getter = INTERN("getter"); S_deleter = INTERN("deleter"); S_fget = INTERN("fget");
  S_self = INTERN("__self__"); S_func = INTERN("__func__"); S_getattr = INTERN("__getattr__"); S_setattr = INTERN("__setattr__"); S_new = INTERN("__new__"); S_doc = INTERN("__doc__");
  S_init_subclass = INTERN("__init_subclass__"); S_class_getitem = INTERN("__class_getitem__");
  g_builtins = dict_new(); g_modules = dict_new();
  builtins_init();
  g_benv = env_new_dict(NULL, NULL);
  odec(g_benv->vars); g_benv->vars = g_builtins; oinc(g_builtins);
  run_module_source(PRELUDE, "<prelude>", g_benv);
  C_BaseException = (Class *)dict_find_cstr(g_builtins, "BaseException")->o;
  g_globals = env_new_dict(NULL, NULL);
  Value mn = V_str("__main__"); dict_set_str(g_globals->vars, S_name, mn); decref(mn);
}

void eval_assign(Node *t, Value v, Env *env) { assign_target(t, v, env); }

Value run_gen_body(Func *f, Env *e) {
  int st = exec_block(f->def->body, f->def->nbody, e);
  if (st == ST_RET) { Value r = g_ret; g_ret = V_none(); return r; }
  return V_none();
}

int inst_iter_next(Value it, Value *out) {
  Handler h; h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) {
    if (!call_dunder(it, "__next__", NULL, 0, out)) { g_handler = h.prev; throw_error("TypeError", "'%s' object is not an iterator", type_name(it)); }
    g_handler = h.prev;
    return 1;
  }
  g_handler = h.prev; g_frame = h.frame; g_depth = h.depth;
  Value *sc = dict_find_cstr(g_builtins, "StopIteration");
  if (sc && is_instance_of(h.exc, (Class *)sc->o)) { decref(h.exc); clear_traceback(); return 0; }
  throw_value(h.exc);
}
