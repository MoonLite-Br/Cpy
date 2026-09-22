/* gen.c - iterators, generators (thread based) and generator expressions */
#include <pthread.h>
#include <semaphore.h>
#include "cpy.h"

#define GEN_STACK ((size_t)8 << 20)
#define GEN_DEPTH_LIMIT 400

/* ============================================================ generators */
typedef struct Gen {
  Obj h;
  Func *fn; Env *env;
  int state;              /* 0 new, 1 suspended, 2 running, 3 finished */
  int started;
  pthread_t th;
  sem_t sem_resume, sem_yield;
  Value yielded, sent, retval, exc_out, exc_in;
  int has_exc, has_in, kill;
  Handler *top;
  Frame *s_frame; int s_depth, s_line; Handler *s_handler;
} Gen;

static Gen *g_cur_gen;

Value make_generator(Func *f, Env *e) {
  Gen *g = xcalloc(1, sizeof(Gen));
  g->h.rc = 1; g->h.type = T_GEN;
  g->fn = f; oinc(f); g->env = e;
  g->yielded = g->sent = g->retval = g->exc_out = g->exc_in = V_none();
  sem_init(&g->sem_resume, 0, 0); sem_init(&g->sem_yield, 0, 0);
  return V_obj(g, T_GEN);
}

static void *gen_main(void *arg) {
  Gen *g = arg;
  Handler top; top.prev = NULL; top.frame = NULL; top.depth = 0; top.exc = V_undef();
  g->top = &top;
  g_handler = &top; g_frame = NULL; g_depth = g_max_depth - GEN_DEPTH_LIMIT; g_line = 0;
  if (setjmp(top.jb) == 0) {
    Frame fr; fr.prev = NULL; fr.fn = g->fn; fr.env = g->env; fr.file = g->fn->def->file; fr.name = g->fn->name->s; fr.line = 0; fr.exc = V_none();
    g_frame = &fr;
    g->retval = run_gen_body(g->fn, g->env);
    g_frame = NULL;
  } else if (top.exc.t != T_UNDEF) {
    g->exc_out = top.exc; g->has_exc = 1;
  }
  g->state = 3;
  sem_post(&g->sem_yield);
  return NULL;
}

/* run the generator until its next yield.  1 = produced *out, 0 = finished */
static int gen_resume(Gen *g, Value sendv, Value *out) {
  if (g->state == 3) return 0;
  if (g->state == 2) throw_error("ValueError", "generator already executing");
  if (g->state == 0 && sendv.t != T_NONE) throw_error("TypeError", "can't send non-None value to a just-started generator");
  Frame *cf = g_frame; int cd = g_depth, cl = g_line; Handler *ch = g_handler; Gen *prev = g_cur_gen; Value cret = g_ret_get();
  g->sent = inc(sendv); g->state = 2; g_cur_gen = g;
  if (!g->started) {
    g->started = 1;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, GEN_STACK);
    if (pthread_create(&g->th, &at, gen_main, g) != 0) { g->state = 3; g->started = 0; g_cur_gen = prev; throw_error("RuntimeError", "can't start a new thread for the generator"); }
    pthread_attr_destroy(&at);
  } else sem_post(&g->sem_resume);
  sem_wait(&g->sem_yield);
  g_frame = cf; g_depth = cd; g_line = cl; g_handler = ch; g_cur_gen = prev; g_ret_set(cret);
  if (g->state == 3) pthread_join(g->th, NULL);
  if (g->has_exc) { Value e = g->exc_out; g->exc_out = V_none(); g->has_exc = 0; throw_value(e); }
  if (g->state == 3) return 0;
  *out = g->yielded; g->yielded = V_none();
  return 1;
}

Value gen_yield(Value v) {
  Gen *g = g_cur_gen;
  if (!g) throw_error("RuntimeError", "'yield' outside a generator");
  g->yielded = v; g->state = 1;
  g->s_frame = g_frame; g->s_depth = g_depth; g->s_line = g_line; g->s_handler = g_handler;
  sem_post(&g->sem_yield);
  sem_wait(&g->sem_resume);
  g_frame = g->s_frame; g_depth = g->s_depth; g_line = g->s_line; g_handler = g->s_handler;
  if (g->kill) { g_handler = g->top; longjmp(g->top->jb, 1); }
  if (g->has_in) { Value e = g->exc_in; g->exc_in = V_none(); g->has_in = 0; throw_value(e); }
  Value s = g->sent; g->sent = V_none();
  return s;
}

static void gen_switch_in(Gen *g) {
  /* resume the generator thread and wait until it yields or finishes (interpreter state is saved/restored by callers) */
  sem_post(&g->sem_resume);
  sem_wait(&g->sem_yield);
}

static void gen_kill(Gen *g) {
  if (!(g->started && g->state == 1)) return;
  Frame *cf = g_frame; int cd = g_depth, cl = g_line; Handler *ch = g_handler; Gen *prev = g_cur_gen; Value cret = g_ret_get();
  g_cur_gen = g;
  /* first try a polite GeneratorExit so that finally-blocks run */
  Value *ge = dict_find_cstr(g_builtins, "GeneratorExit");
  if (ge) {
    Handler h; h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
    g_handler = &h;
    if (setjmp(h.jb) == 0) {
      g->exc_in = call_value(*ge, NULL, 0, NULL); g->has_in = 1;
      g_handler = h.prev;
    } else { g_handler = h.prev; g->has_in = 0; }
    if (g->has_in) { g->state = 2; gen_switch_in(g); }
  }
  if (g->state == 1) {            /* it yielded again (or GeneratorExit unavailable): force termination */
    g->kill = 1; g->state = 2; gen_switch_in(g);
  }
  g_frame = cf; g_depth = cd; g_line = cl; g_handler = ch; g_cur_gen = prev; g_ret_set(cret);
  pthread_join(g->th, NULL);
  if (g->has_exc) { decref(g->exc_out); g->exc_out = V_none(); g->has_exc = 0; }   /* GeneratorExit or errors during cleanup are ignored */
  g->state = 3;
}

void gen_free(Obj *o) {
  Gen *g = (Gen *)o;
  gen_kill(g);
  decref(g->yielded); decref(g->sent); decref(g->retval); decref(g->exc_out); decref(g->exc_in);
  odec(g->env); odec(g->fn);
  sem_destroy(&g->sem_resume); sem_destroy(&g->sem_yield);
  free(g);
}

Value gen_retval(Value gv) { return inc(((Gen *)gv.o)->retval); }

/* =============================================================== iterators */
typedef struct Iterobj {
  Obj h; int kind;
  Value a, b, c;
  int64_t i, n1, n2, n3;
  int flag;
  Node *node; Env *env; Iter *its; int fwd;
} Iterobj;

Value make_iterobj(int kind, Value a, Value b, Value c) {
  Iterobj *o = xcalloc(1, sizeof(Iterobj));
  o->h.rc = 1; o->h.type = T_ITER; o->kind = kind;
  o->a = inc(a); o->b = inc(b); o->c = inc(c);
  return V_obj(o, T_ITER);
}
void iterobj_free(Obj *ob) {
  Iterobj *o = (Iterobj *)ob;
  decref(o->a); decref(o->b); decref(o->c);
  if (o->kind == IT_COMP) {
    for (int k = 0; k < o->node->nv; k++) if (o->its[k].seq.t != T_NONE && o->its[k].seq.t != T_UNDEF) iter_done(&o->its[k]);
    free(o->its); odec(o->env);
  }
  free(o);
}

int iter_step(Value it, Value *out);
static int step_iterobj(Iterobj *o, Value *out);

int iter_step(Value it, Value *out) {
  switch (it.t) {
  case T_ITER: return step_iterobj((Iterobj *)it.o, out);
  case T_GEN: return gen_resume((Gen *)it.o, V_none(), out);
  case T_INST: return inst_iter_next(it, out);
  default: {
    Iter t; t.seq = it; t.i = 0;   /* should not happen: plain containers are wrapped */
    return iter_next(&t, out);
  }
  }
}

Value make_iterator(Value seq) {
  switch (seq.t) {
  case T_ITER: case T_GEN: return inc(seq);
  case T_INST: {
    Value out;
    if (call_dunder(seq, "__iter__", NULL, 0, &out)) return out;
    throw_error("TypeError", "'%s' object is not iterable", type_name(seq));
  }
  case T_LIST: case T_TUPLE: case T_STR: case T_DICT: case T_SET: case T_RANGE: case T_FILE: {
    Value it = make_iterobj(IT_SEQ, seq, V_none(), V_none());
    return it;
  }
  default: throw_error("TypeError", "'%s' object is not iterable", type_name(seq));
  }
}

static Value zero(void) { return V_none(); }

static int step_iterobj(Iterobj *o, Value *out) {
  switch (o->kind) {
  case IT_SEQ: {
    Iter t; t.seq = o->a; t.i = o->i;
    int r = iter_next(&t, out); o->i = t.i; return r;
  }
  case IT_MAP: {
    List *its = (List *)o->b.o; Value args[8]; int n = its->len, got = 0;
    if (n > 8) throw_error("TypeError", "map() supports at most 8 iterables");
    for (int i = 0; i < n; i++) { if (!iter_step(its->items[i], &args[i])) { for (int k = 0; k < got; k++) decref(args[k]); return 0; } got++; }
    *out = call_value(o->a, args, n, NULL);
    for (int i = 0; i < n; i++) decref(args[i]);
    return 1;
  }
  case IT_FILTER: {
    Value x;
    while (iter_step(o->b, &x)) {
      int keep;
      if (o->a.t == T_NONE) keep = val_truthy(x);
      else { Value r = call_value(o->a, &x, 1, NULL); keep = val_truthy(r); decref(r); }
      if (o->flag) keep = !keep;
      if (keep) { *out = x; return 1; }
      decref(x);
    }
    return 0;
  }
  case IT_ZIP: {
    List *its = (List *)o->b.o; int n = its->len;
    if (n == 0) return 0;
    Value *row = xmalloc(n * sizeof(Value)); int got = 0;
    for (int i = 0; i < n; i++) { if (!iter_step(its->items[i], &row[i])) { for (int k = 0; k < got; k++) decref(row[k]); free(row); return 0; } got++; }
    Value t = V_tuple(row, n); for (int i = 0; i < n; i++) decref(row[i]);
    free(row); *out = t; return 1;
  }
  case IT_ZIPLONG: {
    List *its = (List *)o->b.o; int n = its->len, alive = 0;
    if (n == 0) return 0;
    Value *row = xmalloc(n * sizeof(Value)); List *done = (List *)o->c.o;
    for (int i = 0; i < n; i++) {
      if (done->items[i].i) { row[i] = inc(o->a); continue; }
      if (iter_step(its->items[i], &row[i])) alive++;
      else { done->items[i] = V_int(1); row[i] = inc(o->a); }
    }
    if (!alive) { for (int i = 0; i < n; i++) decref(row[i]); free(row); return 0; }
    Value t = V_tuple(row, n); for (int i = 0; i < n; i++) decref(row[i]);
    free(row); *out = t; return 1;
  }
  case IT_ENUM: {
    Value x;
    if (!iter_step(o->b, &x)) return 0;
    Value pair[2] = {V_int(o->i++), x};
    *out = V_tuple(pair, 2); decref(x); return 1;
  }
  case IT_CALLIT: {
    Value r = call_value(o->a, NULL, 0, NULL);
    if (val_eq(r, o->b)) { decref(r); return 0; }
    *out = r; return 1;
  }
  case IT_COMP: {
    int ncl = o->node->nv; Node **cl = o->node->v; Iter *its = o->its; Env *ce = o->env;
    int i = (int)o->i; int fwd = o->fwd;
    for (;;) {
      if (i < 0) { o->i = -1; return 0; }
      if (i == ncl) {
        *out = eval(o->node->a, ce);
        o->i = ncl - 1; o->fwd = 0;
        return 1;
      }
      Node *c = cl[i];
      if (c->k == N_COMPFOR) {
        if (fwd) {
          Value seq = i == 0 ? inc(o->a) : eval(c->b, ce);
          iter_init(&its[i], seq); decref(seq);
        }
        Value x;
        if (!iter_next(&its[i], &x)) { iter_done(&its[i]); its[i].seq = V_none(); i--; fwd = 0; continue; }
        eval_assign(c->a, x, ce); decref(x);
        i++; fwd = 1;
      } else {
        if (fwd) {
          Value t = eval(c->a, ce); int ok = val_truthy(t); decref(t);
          if (ok) i++; else { i--; fwd = 0; }
        } else { i--; }
      }
      o->i = i; o->fwd = fwd;
    }
  }
  case IT_COUNT: {
    *out = inc(o->a);
    Value nx = binop(OP_ADD, o->a, o->b); decref(o->a); o->a = nx;
    return 1;
  }
  case IT_CYCLE: {
    List *saved = (List *)o->a.o;
    if (!o->flag) {
      Value x;
      if (iter_step(o->b, &x)) { list_append(saved, x); *out = x; return 1; }
      o->flag = 1; o->i = 0;
    }
    if (saved->len == 0) return 0;
    *out = inc(saved->items[o->i % saved->len]); o->i++;
    return 1;
  }
  case IT_REPEAT:
    if (o->i == 0) return 0;
    if (o->i > 0) o->i--;
    *out = inc(o->a); return 1;
  case IT_CHAIN: {
    for (;;) {
      if (o->b.t == T_NONE) {
        Value nx;
        if (!iter_step(o->a, &nx)) return 0;
        o->b = make_iterator(nx); decref(nx);
      }
      Value x;
      if (iter_step(o->b, &x)) { *out = x; return 1; }
      decref(o->b); o->b = V_none();
    }
  }
  case IT_ISLICE: {
    /* n1 = next index to yield, n2 = stop (-1 none), n3 = step, i = consumed so far */
    if (o->n2 >= 0 && o->n1 >= o->n2) return 0;
    Value x;
    while (o->i < o->n1) { if (!iter_step(o->b, &x)) return 0; decref(x); o->i++; }
    if (!iter_step(o->b, &x)) return 0;
    o->i++; o->n1 += o->n3;
    *out = x; return 1;
  }
  case IT_TAKEWHILE: {
    if (o->flag) return 0;
    Value x;
    if (!iter_step(o->b, &x)) return 0;
    Value r = call_value(o->a, &x, 1, NULL); int ok = val_truthy(r); decref(r);
    if (!ok) { decref(x); o->flag = 1; return 0; }
    *out = x; return 1;
  }
  case IT_DROPWHILE: {
    Value x;
    while (iter_step(o->b, &x)) {
      if (o->flag) { *out = x; return 1; }
      Value r = call_value(o->a, &x, 1, NULL); int drop = val_truthy(r); decref(r);
      if (!drop) { o->flag = 1; *out = x; return 1; }
      decref(x);
    }
    return 0;
  }
  case IT_STARMAP: {
    Value x;
    if (!iter_step(o->b, &x)) return 0;
    Value lv = list_from_iter(x); decref(x);
    *out = call_value(o->a, ((List *)lv.o)->items, ((List *)lv.o)->len, NULL);
    decref(lv); return 1;
  }
  case IT_ACCUM: {
    Value x;
    if (o->flag == 0 && o->c.t == T_UNDEF) {           /* first element */
      if (!iter_step(o->b, &x)) return 0;
      o->c = x; o->flag = 1; *out = inc(x); return 1;
    }
    if (o->flag == 0) { o->flag = 1; *out = inc(o->c); return 1; }   /* initial value */
    if (!iter_step(o->b, &x)) return 0;
    Value r = o->a.t == T_NONE ? binop(OP_ADD, o->c, x) : ({ Value args[2] = {o->c, x}; call_value(o->a, args, 2, NULL); });
    decref(x); decref(o->c); o->c = r; *out = inc(r); return 1;
  }
  case IT_COMPRESS: {
    Value d, s;
    for (;;) {
      if (!iter_step(o->b, &d)) return 0;
      if (!iter_step(o->c, &s)) { decref(d); return 0; }
      int ok = val_truthy(s); decref(s);
      if (ok) { *out = d; return 1; }
      decref(d);
    }
  }
  case IT_PAIRWISE: {
    Value x;
    if (o->a.t == T_UNDEF || o->a.t == T_NONE) {
      Value first;
      if (!iter_step(o->b, &first)) return 0;
      o->a = first;
    }
    if (!iter_step(o->b, &x)) return 0;
    Value pair[2] = {o->a, x}; *out = V_tuple(pair, 2);
    decref(o->a); o->a = x; return 1;
  }
  default: return 0;
  }
  (void)zero;
}

/* generator expression: lazily evaluated comprehension */
Value make_comp_iter(Node *n, Env *env, Value first) {
  Iterobj *o = xcalloc(1, sizeof(Iterobj));
  o->h.rc = 1; o->h.type = T_ITER; o->kind = IT_COMP;
  o->a = inc(first); o->b = V_none(); o->c = V_none();
  o->node = n; o->env = env; oinc(env);
  o->its = xcalloc(n->nv, sizeof(Iter));
  for (int k = 0; k < n->nv; k++) o->its[k].seq = V_none();
  o->i = 0; o->fwd = 1;
  return V_obj(o, T_ITER);
}

/* ------------------------------------------------------- methods */
static Value m_next(Value *a, int n, Kw *kw) {
  (void)n; (void)kw; Value x;
  if (iter_step(a[0], &x)) return x;
  throw_error("StopIteration", "%s", "");
}
static Value m_iter(Value *a, int n, Kw *kw) { (void)n; (void)kw; return inc(a[0]); }
static Value m_send(Value *a, int n, Kw *kw) {
  (void)kw; chk("send", n, 2, 2); Value x;
  if (a[0].t != T_GEN) throw_error("AttributeError", "send");
  if (gen_resume((Gen *)a[0].o, a[1], &x)) return x;
  throw_error("StopIteration", "%s", "");
}
static Value m_close(Value *a, int n, Kw *kw) {
  (void)n; (void)kw; Gen *g = (Gen *)a[0].o; gen_kill(g); if (g->state != 2) g->state = 3; return V_none();
}
static Value m_throw(Value *a, int n, Kw *kw) {
  (void)kw; chk("throw", n, 2, 2); Gen *g = (Gen *)a[0].o; Value e = a[1];
  if (e.t == T_CLASS) e = call_value(e, NULL, 0, NULL); else incref(e);
  if (g->state == 0 || g->state == 3) { g->state = 3; throw_value(e); }
  g->exc_in = e; g->has_in = 1;
  Value x;
  if (gen_resume(g, V_none(), &x)) return x;
  throw_error("StopIteration", "%s", "");
}

Value gen_get_method(Value obj, Str *name) {
  static Value b_next, b_iter, b_send, b_close, b_throw;
  if (b_next.t != T_BUILTIN) {
    b_next = new_builtin("__next__", m_next); b_iter = new_builtin("__iter__", m_iter);
    b_send = new_builtin("send", m_send); b_close = new_builtin("close", m_close); b_throw = new_builtin("throw", m_throw);
  }
  const char *s = name->s; Value f = V_undef();
  if (!strcmp(s, "__next__")) f = b_next; else if (!strcmp(s, "__iter__")) f = b_iter;
  else if (obj.t == T_GEN) { if (!strcmp(s, "send")) f = b_send; else if (!strcmp(s, "close")) f = b_close; else if (!strcmp(s, "throw")) f = b_throw; }
  if (f.t == T_UNDEF) return V_undef();
  return make_bound(obj, f);
}

void iterobj_init_nums(Value it, int64_t i, int64_t n1, int64_t n2, int64_t n3, int flag) {
  Iterobj *o = (Iterobj *)it.o; o->i = i; o->n1 = n1; o->n2 = n2; o->n3 = n3; o->flag = flag;
}

/* ------------------------------------------------ native _itertools module */
#define ARGS Value *a, int n, Kw *kw
#define UNUSED (void)a; (void)n; (void)kw
static Value tuple_of_iters(Value *a, int n) {
  List *l = list_new(n); l->h.type = T_TUPLE;
  for (int i = 0; i < n; i++) list_append_own(l, make_iterator(a[i]));
  return V_obj(l, T_TUPLE);
}
static Value it_count(ARGS) {
  Value st = V_int(0), sp = V_int(1), *k;
  if (n > 0) st = a[0]; if (n > 1) sp = a[1];
  if ((k = kwget(kw, "start"))) st = *k; if ((k = kwget(kw, "step"))) sp = *k;
  return make_iterobj(IT_COUNT, st, sp, V_none());
}
static Value it_cycle(ARGS) {
  UNUSED; chk("cycle", n, 1, 1);
  Value saved = V_list_new(), src = make_iterator(a[0]);
  Value r = make_iterobj(IT_CYCLE, saved, src, V_none()); decref(saved); decref(src); return r;
}
static Value it_repeat(ARGS) {
  chk("repeat", n, 1, 2); Value *k = kwget(kw, "times");
  Value r = make_iterobj(IT_REPEAT, a[0], V_none(), V_none());
  int64_t times = -1; if (n == 2) times = need_int(a[1], "times"); else if (k) times = need_int(*k, "times");
  iterobj_init_nums(r, times, 0, 0, 0, 0); return r;
}
static Value it_chain(ARGS) {
  UNUSED; List *l = list_new(n); l->h.type = T_TUPLE;
  for (int i = 0; i < n; i++) list_append(l, a[i]);
  Value tv = V_obj(l, T_TUPLE); Value src = make_iterator(tv); decref(tv);
  Value r = make_iterobj(IT_CHAIN, src, V_none(), V_none()); decref(src); return r;
}
static Value it_chain_from(ARGS) {
  UNUSED; chk("from_iterable", n, 1, 1);
  Value src = make_iterator(a[0]); Value r = make_iterobj(IT_CHAIN, src, V_none(), V_none()); decref(src); return r;
}
static Value it_islice(ARGS) {
  UNUSED; chk("islice", n, 2, 4);
  int64_t start = 0, stop = -1, step = 1;
  if (n == 2) { if (a[1].t != T_NONE) stop = need_int(a[1], "stop"); }
  else { if (a[1].t != T_NONE) start = need_int(a[1], "start"); if (a[2].t != T_NONE) stop = need_int(a[2], "stop"); if (n == 4 && a[3].t != T_NONE) step = need_int(a[3], "step"); }
  if (start < 0 || step <= 0) throw_error("ValueError", "Indices for islice() must be None or an integer: 0 <= x <= sys.maxsize.");
  Value src = make_iterator(a[0]); Value r = make_iterobj(IT_ISLICE, V_none(), src, V_none()); decref(src);
  iterobj_init_nums(r, 0, start, stop, step, 0); return r;
}
static Value it_pair(int kind, const char *nm, Value *a, int n) {
  chk(nm, n, 2, 2);
  Value src = make_iterator(a[1]); Value r = make_iterobj(kind, a[0], src, V_none()); decref(src); return r;
}
static Value it_takewhile(ARGS) { UNUSED; return it_pair(IT_TAKEWHILE, "takewhile", a, n); }
static Value it_dropwhile(ARGS) { UNUSED; return it_pair(IT_DROPWHILE, "dropwhile", a, n); }
static Value it_starmap(ARGS) { UNUSED; return it_pair(IT_STARMAP, "starmap", a, n); }
static Value it_filterfalse(ARGS) {
  UNUSED; Value r = it_pair(IT_FILTER, "filterfalse", a, n); iterobj_init_nums(r, 0, 0, 0, 0, 1); return r;
}
static Value it_zip_longest(ARGS) {
  Value fill = V_none(), *k = kwget(kw, "fillvalue"); if (k) fill = *k;
  Value tv = tuple_of_iters(a, n); List *flags = list_new(n);
  for (int i = 0; i < n; i++) list_append_own(flags, V_int(0));
  Value fv = V_obj(flags, T_LIST);
  Value r = make_iterobj(IT_ZIPLONG, fill, tv, fv); decref(tv); decref(fv); return r;
}
static Value it_accumulate(ARGS) {
  chk("accumulate", n, 1, 2);
  Value fn = n > 1 ? a[1] : V_none(), init = V_undef(), *k;
  if ((k = kwget(kw, "func"))) fn = *k;
  if ((k = kwget(kw, "initial")) && k->t != T_NONE) init = *k;
  Value src = make_iterator(a[0]); Value r = make_iterobj(IT_ACCUM, fn, src, init); decref(src); return r;
}
static Value it_compress(ARGS) {
  UNUSED; chk("compress", n, 2, 2);
  Value d = make_iterator(a[0]), s = make_iterator(a[1]); Value r = make_iterobj(IT_COMPRESS, V_none(), d, s); decref(d); decref(s); return r;
}
static Value it_pairwise(ARGS) {
  UNUSED; chk("pairwise", n, 1, 1);
  Value src = make_iterator(a[0]); Value r = make_iterobj(IT_PAIRWISE, V_undef(), src, V_none()); decref(src); return r;
}

Value iter_module(void) {
  Module *m = xmalloc(sizeof(Module));
  m->h.rc = 1; m->h.type = T_MODULE; m->name = INTERN("_itertools"); oinc(m->name); m->attrs = dict_new();
  struct { const char *n; BuiltinFn f; } fns[] = {
    {"count", it_count}, {"cycle", it_cycle}, {"repeat", it_repeat}, {"chain", it_chain}, {"islice", it_islice},
    {"takewhile", it_takewhile}, {"dropwhile", it_dropwhile}, {"starmap", it_starmap}, {"filterfalse", it_filterfalse},
    {"zip_longest", it_zip_longest}, {"accumulate", it_accumulate}, {"compress", it_compress}, {"pairwise", it_pairwise},
    {"chain_from_iterable", it_chain_from}};
  for (unsigned i = 0; i < sizeof fns / sizeof fns[0]; i++) { Value b = new_builtin(fns[i].n, fns[i].f); dict_set_cstr(m->attrs, fns[i].n, b); decref(b); }
  return V_obj(m, T_MODULE);
}
