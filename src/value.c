/* value.c - objects, strings, lists, dicts, equality, repr, formatting */
#include "cpy.h"

/* ------------------------------------------------------------ memory */
void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { fputs("cpy: out of memory\n", stderr); exit(1); }
  return p;
}
void *xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) { fputs("cpy: out of memory\n", stderr); exit(1); }
  return p;
}
void *xcalloc(size_t a, size_t b) {
  void *p = calloc(a ? a : 1, b ? b : 1);
  if (!p) { fputs("cpy: out of memory\n", stderr); exit(1); }
  return p;
}
char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = xmalloc(n); memcpy(p, s, n); return p; }

/* -------------------------------------------------------------- Buf */
void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }
static void buf_need(Buf *b, int add) {
  if (b->len + add + 1 > b->cap) {
    int nc = b->cap ? b->cap * 2 : 64;
    while (nc < b->len + add + 1) nc *= 2;
    b->p = xrealloc(b->p, nc); b->cap = nc;
  }
}
void buf_add(Buf *b, const char *s, int n) {
  buf_need(b, n); if (n) memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
void buf_adds(Buf *b, const char *s) { buf_add(b, s, (int)strlen(s)); }
void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }
void buf_addf(Buf *b, const char *fmt, ...) {
  char tmp[512]; va_list ap;
  va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
  if (n < (int)sizeof tmp) { buf_add(b, tmp, n); return; }
  char *big = xmalloc(n + 1);
  va_start(ap, fmt); vsnprintf(big, n + 1, fmt, ap); va_end(ap);
  buf_add(b, big, n); free(big);
}
Value buf_to_str(Buf *b) {
  Value v = V_strn(b->p ? b->p : "", b->len);
  free(b->p); b->p = NULL; b->len = b->cap = 0;
  return v;
}

/* -------------------------------------------------------------- Str */
int utf8_clen(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c >> 5) == 6) return 2;
  if ((c >> 4) == 14) return 3;
  if ((c >> 3) == 30) return 4;
  return 1;
}
Str *str_new(const char *s, int len) {
  Str *o = xmalloc(sizeof(Str) + len + 1);
  o->h.rc = 1; o->h.type = T_STR; o->len = len; o->hash = 0;
  if (len) memcpy(o->s, s, len);
  o->s[len] = 0;
  int ascii = 1, cp = 0;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x80) ascii = 0;
    if ((c & 0xC0) != 0x80) cp++;
  }
  o->ascii = ascii; o->cplen = cp;
  return o;
}
Value V_str(const char *s) { return V_obj(str_new(s, (int)strlen(s)), T_STR); }
Value V_strn(const char *s, int len) { return V_obj(str_new(s, len), T_STR); }
Value V_strf(const char *fmt, ...) {
  char tmp[512]; va_list ap;
  va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
  if (n < (int)sizeof tmp) return V_strn(tmp, n);
  char *big = xmalloc(n + 1);
  va_start(ap, fmt); vsnprintf(big, n + 1, fmt, ap); va_end(ap);
  Value v = V_strn(big, n); free(big); return v;
}
uint32_t str_hash(Str *s) {
  if (s->hash) return s->hash;
  uint32_t h = 2166136261u;
  for (int i = 0; i < s->len; i++) { h ^= (unsigned char)s->s[i]; h *= 16777619u; }
  if (!h) h = 1;
  s->hash = h; return h;
}
int str_cp_offset(Str *s, int cp) {
  if (s->ascii) return cp;
  int off = 0;
  while (cp > 0 && off < s->len) { off += utf8_clen((unsigned char)s->s[off]); cp--; }
  return off > s->len ? s->len : off;
}

static Dict *intern_tab;
Str *intern(const char *s, int len) {
  if (!intern_tab) intern_tab = dict_new();
  Str *tmp = str_new(s, len);
  Value key = V_obj(tmp, T_STR);
  Value *f = dict_find(intern_tab, key);
  if (f) { Str *r = (Str *)f->o; decref(key); return r; }
  dict_set(intern_tab, key, key);
  decref(key);
  return tmp; /* held forever by the intern table */
}

/* ------------------------------------------------------------- List */
List *list_new(int cap) {
  List *l = xmalloc(sizeof(List));
  l->h.rc = 1; l->h.type = T_LIST; l->len = 0; l->cap = cap;
  l->items = cap ? xmalloc(cap * sizeof(Value)) : NULL;
  return l;
}
void list_append_own(List *l, Value v) {
  if (l->len >= l->cap) {
    l->cap = l->cap ? l->cap * 2 : 4;
    l->items = xrealloc(l->items, l->cap * sizeof(Value));
  }
  l->items[l->len++] = v;
}
void list_append(List *l, Value v) { incref(v); list_append_own(l, v); }
Value V_list_new(void) { return V_obj(list_new(0), T_LIST); }
Value V_tuple(Value *items, int n) {
  List *l = list_new(n); l->h.type = T_TUPLE;
  for (int i = 0; i < n; i++) list_append(l, items[i]);
  return V_obj(l, T_TUPLE);
}

/* ------------------------------------------------------------- hash */
static uint32_t mix(uint64_t x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
  return (uint32_t)x;
}
uint32_t val_hash(Value v) {
  switch (v.t) {
  case T_NONE: return 0x9e3779b9u;
  case T_BOOL: case T_INT: return mix((uint64_t)v.i);
  case T_FLOAT: {
    if (v.d == floor(v.d) && fabs(v.d) < 9.2e18) return mix((uint64_t)(int64_t)v.d);
    uint64_t b; memcpy(&b, &v.d, 8); return mix(b);
  }
  case T_STR: return str_hash((Str *)v.o);
  case T_TUPLE: {
    List *l = (List *)v.o; uint32_t h = 0x345678u;
    for (int i = 0; i < l->len; i++) h = (h ^ val_hash(l->items[i])) * 1000003u;
    return h;
  }
  case T_LIST: throw_error("TypeError", "unhashable type: 'list'");
  case T_DICT: throw_error("TypeError", "unhashable type: 'dict'");
  case T_SET: {
    Dict *d = (Dict *)v.o;
    if (d->kind != DK_FROZEN) throw_error("TypeError", "unhashable type: 'set'");
    uint32_t h = 0x55555u;
    for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) h ^= mix(d->e[i].hash);
    return h;
  }
  case T_INST: {
    Value out;
    if (call_dunder(v, "__hash__", NULL, 0, &out)) { uint32_t h = out.t == T_INT ? mix((uint64_t)out.i) : 0; decref(out); return h; }
    return mix((uint64_t)(uintptr_t)v.o);
  }
  default: return mix((uint64_t)(uintptr_t)v.o);
  }
}

/* --------------------------------------------------------- equality */
static double num_d(Value v) { return v.t == T_FLOAT ? v.d : (double)v.i; }
int inst_eq_hook(Value a, Value b);

int val_eq(Value a, Value b) {
  if (a.t == T_UNDEF || b.t == T_UNDEF) return a.t == b.t;
  if (IS_NUM(a) && IS_NUM(b)) {
    if (a.t == T_FLOAT || b.t == T_FLOAT) return num_d(a) == num_d(b);
    return a.i == b.i;
  }
  if (a.t != b.t) {
    if (a.t == T_INST) return inst_eq_hook(a, b);
    if (b.t == T_INST) return inst_eq_hook(b, a);
    return 0;
  }
  switch (a.t) {
  case T_NONE: return 1;
  case T_STR: {
    Str *x = (Str *)a.o, *y = (Str *)b.o;
    return x == y || (x->len == y->len && memcmp(x->s, y->s, x->len) == 0);
  }
  case T_LIST: case T_TUPLE: {
    List *x = (List *)a.o, *y = (List *)b.o;
    if (x == y) return 1;
    if (x->len != y->len) return 0;
    for (int i = 0; i < x->len; i++) if (!val_eq(x->items[i], y->items[i])) return 0;
    return 1;
  }
  case T_DICT: {
    Dict *x = (Dict *)a.o, *y = (Dict *)b.o;
    if (x == y) return 1;
    if (x->live != y->live) return 0;
    for (int i = 0; i < x->n; i++) {
      if (x->e[i].key.t == T_UNDEF) continue;
      Value *o = dict_find(y, x->e[i].key);
      if (!o || !val_eq(x->e[i].val, *o)) return 0;
    }
    return 1;
  }
  case T_SET: {
    Dict *x = (Dict *)a.o, *y = (Dict *)b.o;
    if (x == y) return 1;
    if (x->live != y->live) return 0;
    for (int i = 0; i < x->n; i++) if (x->e[i].key.t != T_UNDEF && !dict_find(y, x->e[i].key)) return 0;
    return 1;
  }
  case T_RANGE: {
    Range *x = (Range *)a.o, *y = (Range *)b.o;
    return x->start == y->start && x->stop == y->stop && x->step == y->step;
  }
  case T_INST: return inst_eq_hook(a, b);
  default: return a.o == b.o;
  }
}

/* -------------------------------------------------------------- Dict */
Dict *dict_new(void) {
  Dict *d = xmalloc(sizeof(Dict));
  d->h.rc = 1; d->h.type = T_DICT;
  d->e = NULL; d->n = d->cap = d->live = 0; d->idx = NULL; d->icap = 0;
  d->kind = DK_PLAIN; d->factory = V_none();
  return d;
}
static int key_eq(Value a, Value b) {
  if (a.t == T_STR && b.t == T_STR) {
    Str *x = (Str *)a.o, *y = (Str *)b.o;
    return x == y || (x->len == y->len && memcmp(x->s, y->s, x->len) == 0);
  }
  if (IS_NUM(a) && IS_NUM(b)) return val_eq(a, b);
  if (a.t != b.t) return 0;
  if (a.t == T_TUPLE || a.t == T_NONE || a.t == T_SET) return val_eq(a, b);
  if (a.t == T_INST) return a.o == b.o || val_eq(a, b);
  return a.o == b.o;
}
static int dict_lookup(Dict *d, Value key, uint32_t h) {
  if (!d->idx) return -1;
  uint32_t mask = d->icap - 1, i = h & mask;
  for (;;) {
    int ix = d->idx[i];
    if (ix == -1) return -1;
    DEntry *e = &d->e[ix];
    if (e->key.t != T_UNDEF && e->hash == h && key_eq(e->key, key)) return ix;
    i = (i + 1) & mask;
  }
}
static void dict_rebuild(Dict *d) {
  int j = 0;
  for (int i = 0; i < d->n; i++)
    if (d->e[i].key.t != T_UNDEF) { if (i != j) d->e[j] = d->e[i]; j++; }
  d->n = j;
  int nc = 8;
  while (nc < d->live + d->live / 2 + 2) nc *= 2;
  if (nc != d->cap) { d->e = xrealloc(d->e, nc * sizeof(DEntry)); d->cap = nc; }
  free(d->idx);
  d->icap = nc * 2;
  d->idx = xmalloc(d->icap * sizeof(int));
  for (int i = 0; i < d->icap; i++) d->idx[i] = -1;
  uint32_t mask = d->icap - 1;
  for (int i = 0; i < d->n; i++) {
    uint32_t k = d->e[i].hash & mask;
    while (d->idx[k] != -1) k = (k + 1) & mask;
    d->idx[k] = i;
  }
}
Value *dict_find(Dict *d, Value key) {
  int ix = dict_lookup(d, key, val_hash(key));
  return ix < 0 ? NULL : &d->e[ix].val;
}
Value *dict_find_str(Dict *d, Str *s) {
  int ix = dict_lookup(d, V_obj(s, T_STR), str_hash(s));
  return ix < 0 ? NULL : &d->e[ix].val;
}
Value *dict_find_cstr(Dict *d, const char *s) { return dict_find_str(d, INTERN(s)); }
void dict_set(Dict *d, Value key, Value val) {
  uint32_t h = val_hash(key);
  int ix = dict_lookup(d, key, h);
  if (ix >= 0) { Value old = d->e[ix].val; d->e[ix].val = inc(val); decref(old); return; }
  if (d->n >= d->cap) dict_rebuild(d);
  DEntry *e = &d->e[d->n];
  e->key = inc(key); e->val = inc(val); e->hash = h;
  uint32_t mask = d->icap - 1, i = h & mask;
  while (d->idx[i] != -1) i = (i + 1) & mask;
  d->idx[i] = d->n; d->n++; d->live++;
}
void dict_set_str(Dict *d, Str *s, Value val) { dict_set(d, V_obj(s, T_STR), val); }
void dict_set_cstr(Dict *d, const char *s, Value val) { dict_set_str(d, INTERN(s), val); }
int dict_index_str(Dict *d, Str *s) { return dict_lookup(d, V_obj(s, T_STR), str_hash(s)); }
void dict_set_hint(Dict *d, Str *s, Value val, int *hint) {
  int h = *hint;
  if (h < d->n && d->e[h].key.t == T_STR && d->e[h].key.o == (Obj *)s) {
    Value old = d->e[h].val; d->e[h].val = inc(val); decref(old); return;
  }
  dict_set(d, V_obj(s, T_STR), val);
  int ix = dict_lookup(d, V_obj(s, T_STR), str_hash(s));
  if (ix >= 0) *hint = ix;
}
int dict_del(Dict *d, Value key) {
  int ix = dict_lookup(d, key, val_hash(key));
  if (ix < 0) return 0;
  Value k = d->e[ix].key, v = d->e[ix].val;
  d->e[ix].key.t = T_UNDEF; d->e[ix].val = V_none();
  d->live--;
  decref(k); decref(v);
  return 1;
}

/* ------------------------------------------------------------- misc */
Value make_bound(Value self, Value func) {
  Bound *b = xmalloc(sizeof(Bound));
  b->h.rc = 1; b->h.type = T_BOUND; b->self = inc(self); b->func = inc(func);
  return V_obj(b, T_BOUND);
}
int64_t range_len(Range *r) {
  if (r->step > 0) return r->start >= r->stop ? 0 : (r->stop - r->start + r->step - 1) / r->step;
  return r->start <= r->stop ? 0 : (r->start - r->stop - r->step - 1) / (-r->step);
}
Value make_range(int64_t a, int64_t b, int64_t c) {
  Range *r = xmalloc(sizeof(Range));
  r->h.rc = 1; r->h.type = T_RANGE; r->start = a; r->stop = b; r->step = c;
  return V_obj(r, T_RANGE);
}

void *env_freelist;
void free_obj(Obj *o) {
  switch (o->type) {
  case T_LIST: case T_TUPLE: {
    List *l = (List *)o;
    for (int i = 0; i < l->len; i++) decref(l->items[i]);
    free(l->items); break;
  }
  case T_DICT: case T_SET: {
    Dict *d = (Dict *)o;
    for (int i = 0; i < d->n; i++)
      if (d->e[i].key.t != T_UNDEF) { decref(d->e[i].key); decref(d->e[i].val); }
    decref(d->factory);
    free(d->e); free(d->idx); break;
  }
  case T_DESCR: { Descr *d = (Descr *)o; decref(d->fget); decref(d->fset); decref(d->fdel); break; }
  case T_GEN: gen_free(o); return;
  case T_ITER: iterobj_free(o); return;
  case T_FUNC: {
    Func *f = (Func *)o;
    odec(f->closure);
    for (int i = 0; i < f->def->nparams; i++) decref(f->defaults[i]);
    free(f->defaults); odec(f->owner); odec(f->name); odec(f->attrs); break;
  }
  case T_BUILTIN: o->rc = 1 << 20; return;
  case T_CLASS: {
    Class *c = (Class *)o; odec(c->name); odec(c->attrs);
    for (int i = 0; i < c->nbases; i++) odec(c->bases[i]);
    free(c->bases); free(c->mro); break;
  }
  case T_INST: { Inst *i = (Inst *)o; odec(i->cls); odec(i->attrs); break; }
  case T_BOUND: { Bound *b = (Bound *)o; decref(b->self); decref(b->func); break; }
  case T_MODULE: { Module *m = (Module *)o; odec(m->name); odec(m->attrs); break; }
  case T_FILE: { File *f = (File *)o; if (f->fp && !f->closed && f->fp != stdin && f->fp != stdout && f->fp != stderr) fclose(f->fp); break; }
  case T_SUPER: { Super *s = (Super *)o; decref(s->self); odec(s->owner); break; }
  case T_ENV: {
    Env *e = (Env *)o;
    for (int i = 0; i < e->nslots; i++) decref(e->slots[i]);
    odec(e->vars); odec(e->parent);
    if (e->nslots <= ENV_INLINE) { e->h.rc = 0; e->parent = (Env *)env_freelist; env_freelist = e; return; }
    break;
  }
  default: break;
  }
  free(o);
}

int inst_eq_hook(Value a, Value b) {
  Value out;
  if (a.t == T_INST && call_dunder(a, "__eq__", &b, 1, &out)) {
    int r = val_truthy(out); decref(out); return r;
  }
  return b.t == T_INST && a.o == b.o;
}

int val_truthy(Value v) {
  switch (v.t) {
  case T_NONE: return 0;
  case T_BOOL: case T_INT: return v.i != 0;
  case T_FLOAT: return v.d != 0.0;
  case T_STR: return ((Str *)v.o)->len > 0;
  case T_LIST: case T_TUPLE: return ((List *)v.o)->len > 0;
  case T_DICT: case T_SET: return ((Dict *)v.o)->live > 0;
  case T_RANGE: return range_len((Range *)v.o) > 0;
  case T_INST: {
    Value out;
    if (call_dunder(v, "__bool__", NULL, 0, &out)) { int r = val_truthy(out); decref(out); return r; }
    if (call_dunder(v, "__len__", NULL, 0, &out)) { int r = out.t == T_INT && out.i != 0; decref(out); return r; }
    return 1;
  }
  default: return 1;
  }
}

const char *type_name(Value v) {
  switch (v.t) {
  case T_NONE: return "NoneType"; case T_BOOL: return "bool"; case T_INT: return "int";
  case T_FLOAT: return "float"; case T_STR: return "str"; case T_LIST: return "list";
  case T_TUPLE: return "tuple";
  case T_DICT: { switch (((Dict *)v.o)->kind) { case DK_DEFAULT: return "defaultdict"; case DK_COUNTER: return "Counter"; case DK_ORDERED: return "OrderedDict"; default: return "dict"; } }
  case T_SET: return ((Dict *)v.o)->kind == DK_FROZEN ? "frozenset" : "set";
  case T_RANGE: return "range";
  case T_DESCR: return "property"; case T_ITER: return "iterator"; case T_GEN: return "generator"; case T_BIGINT: return "int";
  case T_FUNC: return "function"; case T_BUILTIN: return "builtin_function_or_method";
  case T_CLASS: return "type"; case T_INST: return ((Inst *)v.o)->cls->name->s;
  case T_BOUND: return "method"; case T_MODULE: return "module"; case T_FILE: return "file";
  case T_SUPER: return "super"; default: return "?";
  }
}

/* ---------------------------------------------------------- compare */
static int cmp_result(int op, int c) {
  switch (op) {
  case C_LT: return c < 0; case C_LE: return c <= 0;
  case C_GT: return c > 0; default: return c >= 0;
  }
}
static const char *op_sym(int op) {
  switch (op) { case C_LT: return "<"; case C_LE: return "<="; case C_GT: return ">"; default: return ">="; }
}
static int reflect_op(int op) {
  switch (op) { case C_LT: return C_GT; case C_LE: return C_GE; case C_GT: return C_LT; default: return C_LE; }
}
static const char *dunder_for(int op) {
  switch (op) { case C_LT: return "__lt__"; case C_LE: return "__le__"; case C_GT: return "__gt__"; default: return "__ge__"; }
}

int val_cmpop(int op, Value a, Value b) {
  if (IS_NUM(a) && IS_NUM(b)) {
    if (a.t == T_FLOAT || b.t == T_FLOAT) {
      double x = num_d(a), y = num_d(b);
      switch (op) { case C_LT: return x < y; case C_LE: return x <= y; case C_GT: return x > y; default: return x >= y; }
    }
    return cmp_result(op, a.i < b.i ? -1 : a.i > b.i ? 1 : 0);
  }
  if (a.t == T_STR && b.t == T_STR) {
    Str *x = (Str *)a.o, *y = (Str *)b.o;
    int m = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->s, y->s, m);
    if (c == 0) c = x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
    return cmp_result(op, c);
  }
  if (a.t == T_SET && b.t == T_SET) {
    Dict *x = (Dict *)a.o, *y = (Dict *)b.o;
    int sub_xy = 1, sub_yx = 1;
    for (int i = 0; i < x->n; i++) if (x->e[i].key.t != T_UNDEF && !dict_find(y, x->e[i].key)) { sub_xy = 0; break; }
    for (int i = 0; i < y->n; i++) if (y->e[i].key.t != T_UNDEF && !dict_find(x, y->e[i].key)) { sub_yx = 0; break; }
    switch (op) {
    case C_LE: return sub_xy; case C_GE: return sub_yx;
    case C_LT: return sub_xy && x->live < y->live; default: return sub_yx && y->live < x->live;
    }
  }
  if ((a.t == T_LIST && b.t == T_LIST) || (a.t == T_TUPLE && b.t == T_TUPLE)) {
    List *x = (List *)a.o, *y = (List *)b.o;
    int m = x->len < y->len ? x->len : y->len;
    for (int i = 0; i < m; i++)
      if (!val_eq(x->items[i], y->items[i])) return val_cmpop(op, x->items[i], y->items[i]);
    return cmp_result(op, x->len < y->len ? -1 : x->len > y->len ? 1 : 0);
  }
  if (a.t == T_INST || b.t == T_INST) {
    Value out;
    if (a.t == T_INST && call_dunder(a, dunder_for(op), &b, 1, &out)) {
      int r = val_truthy(out); decref(out); return r;
    }
    if (b.t == T_INST && call_dunder(b, dunder_for(reflect_op(op)), &a, 1, &out)) {
      int r = val_truthy(out); decref(out); return r;
    }
    if (op == C_LE) return val_cmpop(C_LT, a, b) || val_eq(a, b);
    if (op == C_GE) return val_cmpop(C_GT, a, b) || val_eq(a, b);
  }
  throw_error("TypeError", "'%s' not supported between instances of '%s' and '%s'",
              op_sym(op), type_name(a), type_name(b));
}
int val_lt(Value a, Value b) { return val_cmpop(C_LT, a, b); }

/* ------------------------------------------------------- repr / str */
void fmt_float(double d, char *out) {
  if (isnan(d)) { strcpy(out, "nan"); return; }
  if (isinf(d)) { strcpy(out, d < 0 ? "-inf" : "inf"); return; }
  if (d == 0) { strcpy(out, signbit(d) ? "-0.0" : "0.0"); return; }
  char buf[48]; int p;
  for (p = 1; p <= 17; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, d);
    if (strtod(buf, NULL) == d) break;
  }
  const char *s = buf; int neg = 0;
  if (*s == '-') { neg = 1; s++; }
  char digits[24]; int nd = 0;
  while (*s && *s != 'e') { if (*s != '.') digits[nd++] = *s; s++; }
  digits[nd] = 0;
  int ex = (*s == 'e') ? atoi(s + 1) : 0;
  char *o = out;
  if (neg) *o++ = '-';
  if (ex >= -4 && ex < 16) {
    if (ex >= 0) {
      for (int i = 0; i <= ex; i++) *o++ = i < nd ? digits[i] : '0';
      *o++ = '.';
      if (nd > ex + 1) { for (int i = ex + 1; i < nd; i++) *o++ = digits[i]; } else *o++ = '0';
    } else {
      *o++ = '0'; *o++ = '.';
      for (int i = 0; i < -ex - 1; i++) *o++ = '0';
      for (int i = 0; i < nd; i++) *o++ = digits[i];
    }
    *o = 0;
  } else {
    *o++ = digits[0];
    if (nd > 1) { *o++ = '.'; for (int i = 1; i < nd; i++) *o++ = digits[i]; }
    sprintf(o, "e%c%02d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
  }
}

static void repr_str(Buf *b, Str *s) {
  char q = '\'';
  if (memchr(s->s, '\'', s->len) && !memchr(s->s, '"', s->len)) q = '"';
  buf_addc(b, q);
  for (int i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->s[i];
    if (c == (unsigned char)q || c == '\\') { buf_addc(b, '\\'); buf_addc(b, (char)c); }
    else if (c == '\n') buf_adds(b, "\\n");
    else if (c == '\t') buf_adds(b, "\\t");
    else if (c == '\r') buf_adds(b, "\\r");
    else if (c < 0x20 || c == 0x7f) buf_addf(b, "\\x%02x", c);
    else buf_addc(b, (char)c);
  }
  buf_addc(b, q);
}

static void repr_into(Buf *b, Value v, int depth);
static void str_into(Buf *b, Value v, int depth) {
  if (v.t == T_STR) { buf_add(b, ((Str *)v.o)->s, ((Str *)v.o)->len); return; }
  repr_into(b, v, depth);
}
static void repr_into(Buf *b, Value v, int depth) {
  if (depth > 60) { buf_adds(b, "..."); return; }
  switch (v.t) {
  case T_NONE: buf_adds(b, "None"); break;
  case T_BOOL: buf_adds(b, v.i ? "True" : "False"); break;
  case T_INT: buf_addf(b, "%lld", (long long)v.i); break;
  case T_FLOAT: { char t[48]; fmt_float(v.d, t); buf_adds(b, t); break; }
  case T_STR: repr_str(b, (Str *)v.o); break;
  case T_LIST: {
    List *l = (List *)v.o; buf_addc(b, '[');
    for (int i = 0; i < l->len; i++) { if (i) buf_adds(b, ", "); repr_into(b, l->items[i], depth + 1); }
    buf_addc(b, ']'); break;
  }
  case T_TUPLE: {
    List *l = (List *)v.o; buf_addc(b, '(');
    for (int i = 0; i < l->len; i++) { if (i) buf_adds(b, ", "); repr_into(b, l->items[i], depth + 1); }
    if (l->len == 1) buf_addc(b, ',');
    buf_addc(b, ')'); break;
  }
  case T_DICT: {
    Dict *d = (Dict *)v.o; int first = 1;
    if (d->kind == DK_DEFAULT) { buf_adds(b, "defaultdict("); repr_into(b, d->factory, depth + 1); buf_adds(b, ", "); }
    else if (d->kind == DK_COUNTER) buf_adds(b, "Counter(");
    else if (d->kind == DK_ORDERED) buf_adds(b, "OrderedDict(");
    buf_addc(b, '{');
    int *order = NULL; int cnt = 0;
    if (d->kind == DK_COUNTER) { /* most common first (stable) */
      order = xmalloc((d->live + 1) * sizeof(int));
      for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) order[cnt++] = i;
      for (int i = 1; i < cnt; i++) {
        int x = order[i], j = i - 1;
        while (j >= 0 && d->e[order[j]].val.t == T_INT && d->e[x].val.t == T_INT && d->e[order[j]].val.i < d->e[x].val.i) { order[j + 1] = order[j]; j--; }
        order[j + 1] = x;
      }
    }
    for (int q = 0; q < (order ? cnt : d->n); q++) {
      int i = order ? order[q] : q;
      if (d->e[i].key.t == T_UNDEF) continue;
      if (!first) buf_adds(b, ", ");
      first = 0;
      repr_into(b, d->e[i].key, depth + 1); buf_adds(b, ": "); repr_into(b, d->e[i].val, depth + 1);
    }
    free(order);
    buf_addc(b, '}');
    if (d->kind == DK_DEFAULT || d->kind == DK_COUNTER || d->kind == DK_ORDERED) buf_addc(b, ')');
    break;
  }
  case T_SET: {
    Dict *d = (Dict *)v.o; int first = 1;
    if (d->live == 0) { buf_adds(b, d->kind == DK_FROZEN ? "frozenset()" : "set()"); break; }
    if (d->kind == DK_FROZEN) buf_adds(b, "frozenset(");
    buf_addc(b, '{');
    for (int i = 0; i < d->n; i++) {
      if (d->e[i].key.t == T_UNDEF) continue;
      if (!first) buf_adds(b, ", ");
      first = 0; repr_into(b, d->e[i].key, depth + 1);
    }
    buf_addc(b, '}');
    if (d->kind == DK_FROZEN) buf_addc(b, ')');
    break;
  }
  case T_DESCR: buf_adds(b, "<property>"); break;
  case T_ITER: buf_adds(b, "<iterator object>"); break;
  case T_GEN: buf_adds(b, "<generator object>"); break;
  case T_RANGE: {
    Range *r = (Range *)v.o;
    if (r->step == 1) buf_addf(b, "range(%lld, %lld)", (long long)r->start, (long long)r->stop);
    else buf_addf(b, "range(%lld, %lld, %lld)", (long long)r->start, (long long)r->stop, (long long)r->step);
    break;
  }
  case T_FUNC: buf_addf(b, "<function %s>", ((Func *)v.o)->name->s); break;
  case T_BUILTIN: {
    static const char *types[] = {"int", "float", "str", "bool", "list", "tuple", "dict", "range", "NoneType", "function", "type", NULL};
    const char *nm = ((Builtin *)v.o)->name; int is_type = 0;
    for (int i = 0; types[i]; i++) if (!strcmp(nm, types[i])) is_type = 1;
    if (!strcmp(nm, "Ellipsis")) buf_adds(b, "Ellipsis");
    else if (is_type) buf_addf(b, "<class '%s'>", nm); else buf_addf(b, "<built-in function %s>", nm);
    break;
  }
  case T_CLASS: buf_addf(b, "<class '%s'>", ((Class *)v.o)->name->s); break;
  case T_INST: {
    Value out;
    if (call_dunder(v, "__repr__", NULL, 0, &out)) {
      if (out.t == T_STR) buf_add(b, ((Str *)out.o)->s, ((Str *)out.o)->len);
      decref(out); break;
    }
    buf_addf(b, "<%s object>", ((Inst *)v.o)->cls->name->s); break;
  }
  case T_BOUND: buf_adds(b, "<bound method>"); break;
  case T_MODULE: buf_addf(b, "<module '%s'>", ((Module *)v.o)->name->s); break;
  case T_FILE: buf_adds(b, "<file>"); break;
  case T_SUPER: buf_adds(b, "<super>"); break;
  default: buf_adds(b, "<?>"); break;
  }
}
Str *val_repr(Value v) {
  Buf b; buf_init(&b); repr_into(&b, v, 0);
  Value s = buf_to_str(&b); return (Str *)s.o;
}
Str *val_str(Value v) {
  if (v.t == T_STR) { v.o->rc++; return (Str *)v.o; }
  if (v.t == T_INST) {
    Value out;
    if (call_dunder(v, "__str__", NULL, 0, &out)) {
      if (out.t == T_STR) return (Str *)out.o;
      decref(out); throw_error("TypeError", "__str__ returned non-string (type %s)", type_name(out));
    }
  }
  Buf b; buf_init(&b); str_into(&b, v, 0);
  Value s = buf_to_str(&b); return (Str *)s.o;
}

/* --------------------------------------------------------- iteration */
void iter_init(Iter *it, Value seq) {
  switch (seq.t) {
  case T_LIST: case T_TUPLE: case T_STR: case T_DICT: case T_SET: case T_RANGE: case T_FILE: case T_ITER: case T_GEN: break;
  case T_INST: it->seq = make_iterator(seq); it->i = 0; return;
  case T_CLASS: {
    Value ml = class_member_list(seq);
    if (ml.t == T_UNDEF) throw_error("TypeError", "'type' object is not iterable");
    it->seq = ml; it->i = 0; return;
  }
  default: throw_error("TypeError", "'%s' object is not iterable", type_name(seq));
  }
  it->seq = inc(seq); it->i = 0;
}
int iter_next(Iter *it, Value *out) {
  Value s = it->seq;
  switch (s.t) {
  case T_LIST: case T_TUPLE: {
    List *l = (List *)s.o;
    if (it->i >= l->len) return 0;
    *out = inc(l->items[it->i++]); return 1;
  }
  case T_STR: {
    Str *st = (Str *)s.o;
    if (it->i >= st->len) return 0;
    int n = utf8_clen((unsigned char)st->s[it->i]);
    if (it->i + n > st->len) n = st->len - (int)it->i;
    *out = V_strn(st->s + it->i, n); it->i += n; return 1;
  }
  case T_DICT: case T_SET: {
    Dict *d = (Dict *)s.o;
    while (it->i < d->n && d->e[it->i].key.t == T_UNDEF) it->i++;
    if (it->i >= d->n) return 0;
    *out = inc(d->e[it->i++].key); return 1;
  }
  case T_RANGE: {
    Range *r = (Range *)s.o;
    int64_t cur = r->start + it->i * r->step;
    if (r->step > 0 ? cur >= r->stop : cur <= r->stop) return 0;
    it->i++; *out = V_int(cur); return 1;
  }
  case T_ITER: case T_GEN: case T_INST: return iter_step(s, out);
  case T_FILE: {
    File *f = (File *)s.o;
    if (f->closed || !f->fp) return 0;
    Buf b; buf_init(&b); int c, any = 0;
    while ((c = fgetc(f->fp)) != EOF) { any = 1; buf_addc(&b, (char)c); if (c == '\n') break; }
    if (!any) { free(b.p); return 0; }
    *out = buf_to_str(&b); return 1;
  }
  default: return 0;
  }
}
void iter_done(Iter *it) { decref(it->seq); it->seq = V_none(); }
Value list_from_iter(Value seq) {
  if (seq.t == T_LIST) {
    List *src = (List *)seq.o; List *l = list_new(src->len);
    for (int i = 0; i < src->len; i++) list_append(l, src->items[i]);
    return V_obj(l, T_LIST);
  }
  List *l = list_new(0);
  Iter it; iter_init(&it, seq); Value x;
  while (iter_next(&it, &x)) list_append_own(l, x);
  iter_done(&it);
  return V_obj(l, T_LIST);
}

/* ------------------------------------------------- format / % helpers */
static int cp_count(const char *s, int len) {
  int n = 0;
  for (int i = 0; i < len; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
  return n;
}
/* pad body to width; align: '<' '>' '^' '=' ; sign_len used for '=' */
static Value pad_str(const char *body, int len, int width, char fill, char align, int sign_len) {
  int cps = cp_count(body, len);
  if (cps >= width) return V_strn(body, len);
  int pad = width - cps;
  Buf b; buf_init(&b);
  int left = 0, right = 0;
  if (align == '<') right = pad;
  else if (align == '>') left = pad;
  else if (align == '^') { left = pad / 2; right = pad - left; }
  else if (align == '=') {
    buf_add(&b, body, sign_len);
    for (int i = 0; i < pad; i++) buf_addc(&b, fill);
    buf_add(&b, body + sign_len, len - sign_len);
    return buf_to_str(&b);
  }
  for (int i = 0; i < left; i++) buf_addc(&b, fill);
  buf_add(&b, body, len);
  for (int i = 0; i < right; i++) buf_addc(&b, fill);
  return buf_to_str(&b);
}

Value format_value(Value v, const char *spec) {
  if (!spec || !*spec) { Str *s = val_str(v); return V_obj(s, T_STR); }
  char fill = ' ', align = 0, sign = '-', type = 0; int zero = 0, width = 0, prec = -1, comma = 0;
  const char *p = spec;
  int fill_given = 0;
  if (p[0] && p[1] && strchr("<>^=", p[1])) { fill = p[0]; align = p[1]; p += 2; fill_given = 1; }
  else if (p[0] && strchr("<>^=", p[0])) { align = p[0]; p++; }
  if (*p == '+' || *p == '-' || *p == ' ') sign = *p++;
  if (*p == '0') { zero = 1; p++; }
  while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
  if (*p == ',') { comma = 1; p++; }
  if (*p == '.') { p++; prec = 0; while (isdigit((unsigned char)*p)) prec = prec * 10 + (*p++ - '0'); }
  if (*p) type = *p++;
  if (*p) throw_error("ValueError", "Invalid format specifier '%s'", spec);
  Buf b; buf_init(&b);
  int numeric = 0;
  if (v.t == T_INT || v.t == T_BOOL) {
    numeric = 1;
    long long x = (long long)v.i;
    if (type == 0 || type == 'd' || type == 'n') buf_addf(&b, "%lld", x);
    else if (type == 'x') buf_addf(&b, "%llx", x);
    else if (type == 'X') buf_addf(&b, "%llX", x);
    else if (type == 'o') buf_addf(&b, "%llo", x);
    else if (type == 'b') {
      unsigned long long u = x < 0 ? (unsigned long long)(-x) : (unsigned long long)x;
      char t[80]; int n = 0; if (!u) t[n++] = '0';
      while (u) { t[n++] = '0' + (u & 1); u >>= 1; }
      if (x < 0) buf_addc(&b, '-');
      while (n) buf_addc(&b, t[--n]);
    }
    else if (type == 'c') { buf_addc(&b, (char)x); numeric = 0; }
    else if (strchr("feEgG%", type)) {
      double d = (double)x; int pr = prec < 0 ? 6 : prec;
      if (type == '%') buf_addf(&b, "%.*f%%", pr, d * 100); else { char f[8] = {'%', '.', '*', type, 0}; buf_addf(&b, f, pr, d); }
    } else throw_error("ValueError", "Unknown format code '%c' for object of type 'int'", type);
  } else if (v.t == T_FLOAT) {
    numeric = 1; double d = v.d;
    if (type == 0) {
      if (prec < 0) { char t[48]; fmt_float(d, t); buf_adds(&b, t); }
      else buf_addf(&b, "%.*g", prec, d);
    } else if (type == '%') buf_addf(&b, "%.*f%%", prec < 0 ? 6 : prec, d * 100);
    else if (strchr("feEgG", type)) { char f[8] = {'%', '.', '*', type, 0}; buf_addf(&b, f, prec < 0 ? 6 : prec, d); }
    else throw_error("ValueError", "Unknown format code '%c' for object of type 'float'", type);
  } else {
    Str *s = val_str(v);
    if (type != 0 && type != 's') { odec(s); free(b.p); throw_error("ValueError", "Unknown format code '%c' for object of type '%s'", type, type_name(v)); }
    int len = s->len;
    if (prec >= 0 && prec < s->cplen) len = str_cp_offset(s, prec);
    buf_add(&b, s->s, len); odec(s);
  }
  if (numeric && comma) {
    /* insert thousands separators into the integer part */
    char *t = b.p; int start = (t[0] == '-') ? 1 : 0, end = start;
    while (isdigit((unsigned char)t[end])) end++;
    Buf o; buf_init(&o); buf_add(&o, t, start);
    for (int i = start; i < end; i++) { if (i > start && (end - i) % 3 == 0) buf_addc(&o, ','); buf_addc(&o, t[i]); }
    buf_adds(&o, t + end); free(b.p); b = o;
  }
  if (numeric && sign != '-' && b.p[0] != '-') {
    Buf o; buf_init(&o); buf_addc(&o, sign); buf_add(&o, b.p, b.len); free(b.p); b = o;
  }
  if (zero && numeric) { if (!align) align = '='; if (!fill_given) fill = '0'; }
  if (!align) align = numeric ? '>' : '<';
  int sl = 0;
  if (align == '=') sl = (b.p[0] == '-' || b.p[0] == '+' || b.p[0] == ' ') ? 1 : 0;
  Value r = pad_str(b.p, b.len, width, fill, align, sl);
  free(b.p);
  return r;
}

Value str_percent(Str *fmt, Value args) {
  Value *argv; int argc;
  if (args.t == T_TUPLE) { argv = ((List *)args.o)->items; argc = ((List *)args.o)->len; }
  else { argv = &args; argc = 1; }
  int ai = 0;
  Buf b; buf_init(&b);
  const char *s = fmt->s;
  for (int i = 0; i < fmt->len; i++) {
    if (s[i] != '%') { buf_addc(&b, s[i]); continue; }
    i++;
    if (i >= fmt->len) { free(b.p); throw_error("ValueError", "incomplete format"); }
    if (s[i] == '%') { buf_addc(&b, '%'); continue; }
    char flags[8]; int nf = 0; int width = -1, prec = -1;
    while (i < fmt->len && strchr("-+ 0#", s[i]) && nf < 7) flags[nf++] = s[i++];
    flags[nf] = 0;
    if (i < fmt->len && isdigit((unsigned char)s[i])) { width = 0; while (isdigit((unsigned char)s[i])) width = width * 10 + (s[i++] - '0'); }
    if (i < fmt->len && s[i] == '.') { i++; prec = 0; while (isdigit((unsigned char)s[i])) prec = prec * 10 + (s[i++] - '0'); }
    char t = s[i];
    if (ai >= argc) { free(b.p); throw_error("TypeError", "not enough arguments for format string"); }
    Value a = argv[ai++];
    int left = strchr(flags, '-') != NULL;
    char tmp[64], f[32];
    if (t == 's' || t == 'r') {
      Str *st = t == 's' ? val_str(a) : val_repr(a);
      int len = st->len;
      if (prec >= 0 && prec < st->cplen) len = str_cp_offset(st, prec);
      Value pv = pad_str(st->s, len, width < 0 ? 0 : width, ' ', left ? '<' : '>', 0);
      buf_add(&b, ((Str *)pv.o)->s, ((Str *)pv.o)->len); decref(pv); odec(st);
    } else if (t == 'd' || t == 'i' || t == 'x' || t == 'X' || t == 'o' || t == 'c') {
      long long x;
      if (a.t == T_INT || a.t == T_BOOL) x = a.i;
      else if (a.t == T_FLOAT) x = (long long)a.d;
      else { free(b.p); throw_error("TypeError", "%%%c format: a number is required, not %s", t, type_name(a)); }
      if (t == 'c') { buf_addc(&b, (char)x); continue; }
      snprintf(f, sizeof f, "%%%s%s%s%s%s", flags, width >= 0 ? "*" : "", prec >= 0 ? ".*" : "", "ll", t == 'i' ? "d" : (char[]){t, 0});
      int n;
      if (width >= 0 && prec >= 0) n = snprintf(tmp, sizeof tmp, f, width, prec, x);
      else if (width >= 0) n = snprintf(tmp, sizeof tmp, f, width, x);
      else if (prec >= 0) n = snprintf(tmp, sizeof tmp, f, prec, x);
      else n = snprintf(tmp, sizeof tmp, f, x);
      buf_add(&b, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
    } else if (strchr("feEgG", t)) {
      double d;
      if (a.t == T_FLOAT) d = a.d; else if (a.t == T_INT || a.t == T_BOOL) d = (double)a.i;
      else { free(b.p); throw_error("TypeError", "must be real number, not %s", type_name(a)); }
      snprintf(f, sizeof f, "%%%s%s%s%c", flags, width >= 0 ? "*" : "", ".*", t);
      int pr = prec < 0 ? 6 : prec, n;
      if (width >= 0) n = snprintf(tmp, sizeof tmp, f, width, pr, d); else n = snprintf(tmp, sizeof tmp, f, pr, d);
      buf_add(&b, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
    } else { free(b.p); throw_error("ValueError", "unsupported format character '%c'", t); }
  }
  if (ai < argc && args.t == T_TUPLE) { free(b.p); throw_error("TypeError", "not all arguments converted during string formatting"); }
  return buf_to_str(&b);
}
