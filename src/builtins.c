#define _GNU_SOURCE
/* builtins.c - global builtin functions */
#include <errno.h>
#include <time.h>
#include "cpy.h"

/* ---------------------------------------------------------- helpers */
void chk(const char *name, int n, int lo, int hi) {
  if (n >= lo && n <= hi) return;
  if (lo == hi) throw_error("TypeError", "%s() takes exactly %d argument%s (%d given)", name, lo, lo == 1 ? "" : "s", n);
  throw_error("TypeError", "%s() takes %d to %d arguments (%d given)", name, lo, hi, n);
}
Value *kwget(Kw *kw, const char *name) {
  if (!kw) return NULL;
  for (int i = 0; i < kw->n; i++) if (!strcmp(kw->names[i]->s, name)) return &kw->vals[i];
  return NULL;
}
int64_t need_int(Value v, const char *ctx) {
  if (v.t == T_INT || v.t == T_BOOL) return v.i;
  throw_error("TypeError", "%s must be an integer, not %s", ctx, type_name(v));
}
double need_num(Value v, const char *ctx) {
  if (v.t == T_FLOAT) return v.d;
  if (v.t == T_INT || v.t == T_BOOL) return (double)v.i;
  throw_error("TypeError", "%s must be a number, not %s", ctx, type_name(v));
}
Str *need_str(Value v, const char *ctx) {
  if (v.t == T_STR) return (Str *)v.o;
  throw_error("TypeError", "%s must be str, not %s", ctx, type_name(v));
}

Value new_builtin(const char *name, BuiltinFn fn) {
  Builtin *b = xmalloc(sizeof(Builtin));
  b->h.rc = 1; b->h.type = T_BUILTIN; b->name = name; b->fn = fn;
  return V_obj(b, T_BUILTIN);
}
void reg_builtin(const char *name, BuiltinFn fn) {
  Value b = new_builtin(name, fn);
  dict_set_cstr(g_builtins, name, b);
  decref(b);
}

int try_get_attr(Value o, Str *name, Value *out) {
  Handler h; h.prev = g_handler; h.frame = g_frame; h.depth = g_depth; h.exc = V_none();
  g_handler = &h;
  if (setjmp(h.jb) == 0) { *out = get_attr(o, name); g_handler = h.prev; return 1; }
  g_handler = h.prev; g_frame = h.frame; g_depth = h.depth;
  Value *ac = dict_find_cstr(g_builtins, "AttributeError");
  if (ac && is_instance_of(h.exc, (Class *)ac->o)) { decref(h.exc); clear_traceback(); return 0; }
  throw_value(h.exc);
}

Value make_file(FILE *fp) {
  File *f = xmalloc(sizeof(File));
  f->h.rc = 1; f->h.type = T_FILE; f->fp = fp; f->closed = 0;
  return V_obj(f, T_FILE);
}

#define ARGS Value *a, int n, Kw *kw
#define UNUSED (void)a; (void)n; (void)kw

/* ------------------------------------------------------------ print */
static Value bi_print(ARGS) {
  const char *sep = " ", *end = "\n"; int seplen = 1, endlen = 1;
  Value *k;
  if ((k = kwget(kw, "sep")) && k->t == T_STR) { sep = ((Str *)k->o)->s; seplen = ((Str *)k->o)->len; }
  if ((k = kwget(kw, "end")) && k->t == T_STR) { end = ((Str *)k->o)->s; endlen = ((Str *)k->o)->len; }
  FILE *out = stdout;
  if ((k = kwget(kw, "file")) && k->t == T_FILE && ((File *)k->o)->fp) out = ((File *)k->o)->fp;
  for (int i = 0; i < n; i++) {
    if (i) fwrite(sep, 1, seplen, out);
    Str *s = val_str(a[i]); fwrite(s->s, 1, s->len, out); odec(s);
  }
  fwrite(end, 1, endlen, out);
  if ((k = kwget(kw, "flush")) && val_truthy(*k)) fflush(out);
  return V_none();
}

static Value bi_input(ARGS) {
  UNUSED; chk("input", n, 0, 1);
  if (n == 1) { Str *s = val_str(a[0]); fwrite(s->s, 1, s->len, stdout); odec(s); }
  fflush(stdout);
  Buf b; buf_init(&b); int c, any = 0;
  while ((c = fgetc(stdin)) != EOF) { any = 1; if (c == '\n') break; buf_addc(&b, (char)c); }
  if (!any) { free(b.p); throw_error("EOFError", "EOF when reading a line"); }
  if (b.len && b.p[b.len - 1] == '\r') b.len--;
  return buf_to_str(&b);
}

/* --------------------------------------------------------- basic types */
static Value bi_len(ARGS) {
  UNUSED; chk("len", n, 1, 1);
  Value v = a[0];
  switch (v.t) {
  case T_STR: return V_int(((Str *)v.o)->cplen);
  case T_LIST: case T_TUPLE: return V_int(((List *)v.o)->len);
  case T_DICT: case T_SET: return V_int(((Dict *)v.o)->live);
  case T_RANGE: return V_int(range_len((Range *)v.o));
  case T_INST: { Value out; if (call_dunder(v, "__len__", NULL, 0, &out)) return out; }
  /* fallthrough */
  default: throw_error("TypeError", "object of type '%s' has no len()", type_name(v));
  }
}

static Value str_to_int(Str *s, int base) {
  const char *p = s->s; while (isspace((unsigned char)*p)) p++;
  char buf[128]; int k = 0;
  for (; *p && k < 120; p++) if (*p != '_') buf[k++] = *p;
  buf[k] = 0;
  while (k > 0 && isspace((unsigned char)buf[k - 1])) buf[--k] = 0;
  errno = 0; char *end;
  long long v = strtoll(buf, &end, base);
  if (k == 0 || *end || errno) throw_error("ValueError", "invalid literal for int() with base %d: '%s'", base, s->s);
  return V_int(v);
}
static Value bi_int(ARGS) {
  UNUSED; chk("int", n, 0, 2);
  if (n == 0) return V_int(0);
  Value v = a[0];
  int base = n == 2 ? (int)need_int(a[1], "base") : 10;
  switch (v.t) {
  case T_BOOL: case T_INT: return V_int(v.i);
  case T_FLOAT:
    if (isnan(v.d)) throw_error("ValueError", "cannot convert float NaN to integer");
    if (isinf(v.d) || fabs(v.d) >= 9.2e18) throw_error("OverflowError", "cannot convert float to integer");
    return V_int((int64_t)v.d);
  case T_STR: return str_to_int((Str *)v.o, base);
  case T_INST: { Value out; if (call_dunder(v, "__int__", NULL, 0, &out)) return out; }
  /* fallthrough */
  default: throw_error("TypeError", "int() argument must be a string or a number, not '%s'", type_name(v));
  }
}
static Value bi_float(ARGS) {
  UNUSED; chk("float", n, 0, 1);
  if (n == 0) return V_float(0.0);
  Value v = a[0];
  if (v.t == T_FLOAT) return v;
  if (v.t == T_INT || v.t == T_BOOL) return V_float((double)v.i);
  if (v.t == T_STR) {
    Str *s = (Str *)v.o; const char *p = s->s;
    while (isspace((unsigned char)*p)) p++;
    char *end; double d = strtod(p, &end);
    while (isspace((unsigned char)*end)) end++;
    if (end == p || *end || strpbrk(p, "xX")) throw_error("ValueError", "could not convert string to float: '%s'", s->s);
    return V_float(d);
  }
  if (v.t == T_INST) { Value out; if (call_dunder(v, "__float__", NULL, 0, &out)) return out; }
  throw_error("TypeError", "float() argument must be a string or a number, not '%s'", type_name(v));
}
static Value bi_str(ARGS) {
  UNUSED; chk("str", n, 0, 1);
  if (n == 0) return V_str("");
  return V_obj(val_str(a[0]), T_STR);
}
static Value bi_repr(ARGS) { UNUSED; chk("repr", n, 1, 1); return V_obj(val_repr(a[0]), T_STR); }
static Value bi_bool(ARGS) { UNUSED; chk("bool", n, 0, 1); return V_bool(n ? val_truthy(a[0]) : 0); }
static Value bi_list(ARGS) {
  UNUSED; chk("list", n, 0, 1);
  if (n == 0) return V_list_new();
  return list_from_iter(a[0]);
}
static Value bi_tuple(ARGS) {
  UNUSED; chk("tuple", n, 0, 1);
  if (n == 0) return V_tuple(NULL, 0);
  Value l = list_from_iter(a[0]); ((List *)l.o)->h.type = T_TUPLE; l.t = T_TUPLE;
  return l;
}
static Value bi_dict(ARGS) {
  chk("dict", n, 0, 1);
  Dict *d = dict_new(); Value dv = V_obj(d, T_DICT);
  if (n == 1) {
    if (a[0].t == T_DICT) {
      Dict *s = (Dict *)a[0].o;
      for (int i = 0; i < s->n; i++) if (s->e[i].key.t != T_UNDEF) dict_set(d, s->e[i].key, s->e[i].val);
    } else {
      Iter it; iter_init(&it, a[0]); Value x;
      while (iter_next(&it, &x)) {
        if ((x.t != T_LIST && x.t != T_TUPLE) || ((List *)x.o)->len != 2) { decref(x); iter_done(&it); decref(dv); throw_error("ValueError", "dictionary update sequence element has wrong length"); }
        dict_set(d, ((List *)x.o)->items[0], ((List *)x.o)->items[1]); decref(x);
      }
      iter_done(&it);
    }
  }
  if (kw) for (int i = 0; i < kw->n; i++) dict_set_str(d, kw->names[i], kw->vals[i]);
  return dv;
}
static Value bi_range(ARGS) {
  UNUSED; chk("range", n, 1, 3);
  int64_t s = 0, e, st = 1;
  if (n == 1) e = need_int(a[0], "range()");
  else { s = need_int(a[0], "range()"); e = need_int(a[1], "range()"); if (n == 3) st = need_int(a[2], "range()"); }
  if (st == 0) throw_error("ValueError", "range() arg 3 must not be zero");
  return make_range(s, e, st);
}

static const char *builtin_type_name(Value v) { return v.t == T_BUILTIN ? ((Builtin *)v.o)->name : NULL; }

static Value bi_type(ARGS) {
  UNUSED; chk("type", n, 1, 3);
  if (n == 3) {
    Str *nm = need_str(a[0], "type() name");
    if (a[1].t != T_TUPLE || a[2].t != T_DICT) throw_error("TypeError", "type() takes 1 or 3 arguments");
    List *bl = (List *)a[1].o; Class *bases[16];
    if (bl->len > 16) throw_error("TypeError", "too many base classes");
    for (int i = 0; i < bl->len; i++) { if (bl->items[i].t != T_CLASS) throw_error("TypeError", "bases must be classes"); bases[i] = (Class *)bl->items[i].o; }
    Dict *at = dict_new(); Dict *src = (Dict *)a[2].o;
    for (int i = 0; i < src->n; i++) if (src->e[i].key.t != T_UNDEF) dict_set(at, src->e[i].key, src->e[i].val);
    return V_obj(make_class(intern(nm->s, nm->len), bases, bl->len, at), T_CLASS);
  }
  if (n == 2) throw_error("TypeError", "type() takes 1 or 3 arguments");
  Value v = a[0];
  if (v.t == T_INST) return inc(V_obj(((Inst *)v.o)->cls, T_CLASS));
  const char *nm = type_name(v);
  if (v.t == T_BUILTIN) nm = "function";
  Value *b = dict_find_cstr(g_builtins, nm);
  if (b) return inc(*b);
  return V_str(nm);
}

static int isinstance_of(Value x, Value t) {
  if (t.t == T_TUPLE) {
    List *l = (List *)t.o;
    for (int i = 0; i < l->len; i++) if (isinstance_of(x, l->items[i])) return 1;
    return 0;
  }
  if (t.t == T_CLASS) {
    Class *c = (Class *)t.o;
    if (!c->base && !strcmp(c->name->s, "object")) return 1;
    return is_instance_of(x, c);
  }
  const char *nm = builtin_type_name(t);
  if (!nm) throw_error("TypeError", "isinstance() arg 2 must be a type or tuple of types");
  if (!strcmp(nm, "int")) return x.t == T_INT || x.t == T_BOOL;
  if (!strcmp(nm, "float")) return x.t == T_FLOAT;
  if (!strcmp(nm, "str")) return x.t == T_STR;
  if (!strcmp(nm, "bool")) return x.t == T_BOOL;
  if (!strcmp(nm, "list")) return x.t == T_LIST;
  if (!strcmp(nm, "tuple")) return x.t == T_TUPLE;
  if (!strcmp(nm, "dict")) return x.t == T_DICT;
  if (!strcmp(nm, "set")) return x.t == T_SET && ((Dict *)x.o)->kind == DK_SET;
  if (!strcmp(nm, "frozenset")) return x.t == T_SET && ((Dict *)x.o)->kind == DK_FROZEN;
  if (!strcmp(nm, "defaultdict")) return x.t == T_DICT && ((Dict *)x.o)->kind == DK_DEFAULT;
  if (!strcmp(nm, "Counter")) return x.t == T_DICT && ((Dict *)x.o)->kind == DK_COUNTER;
  if (!strcmp(nm, "OrderedDict")) return x.t == T_DICT && ((Dict *)x.o)->kind == DK_ORDERED;
  if (!strcmp(nm, "range")) return x.t == T_RANGE;
  if (!strcmp(nm, "NoneType")) return x.t == T_NONE;
  if (!strcmp(nm, "function")) return x.t == T_FUNC || x.t == T_BUILTIN || x.t == T_BOUND;
  return 0;
}
static Value bi_isinstance(ARGS) { UNUSED; chk("isinstance", n, 2, 2); return V_bool(isinstance_of(a[0], a[1])); }
static Value bi_dummy_none(ARGS) { UNUSED; return V_none(); }

/* ---------------------------------------------------------- numeric */
static Value bi_abs(ARGS) {
  UNUSED; chk("abs", n, 1, 1); Value v = a[0];
  if (v.t == T_INT || v.t == T_BOOL) { if (v.i == INT64_MIN) throw_error("OverflowError", "integer overflow"); return V_int(v.i < 0 ? -v.i : v.i); }
  if (v.t == T_FLOAT) return V_float(fabs(v.d));
  Value out; if (call_dunder(v, "__abs__", NULL, 0, &out)) return out;
  throw_error("TypeError", "bad operand type for abs(): '%s'", type_name(v));
}

static Value minmax(const char *name, int ismax, Value *a, int n, Kw *kw) {
  Value *key = kwget(kw, "key"), *dflt = kwget(kw, "default");
  Value seq = n == 1 ? inc(a[0]) : V_tuple(a, n);
  Iter it; iter_init(&it, seq); decref(seq);
  Value best = V_undef(), bestk = V_undef(), x;
  while (iter_next(&it, &x)) {
    Value k = (key && key->t != T_NONE) ? call_value(*key, &x, 1, NULL) : inc(x);
    if (best.t == T_UNDEF) { best = x; bestk = k; continue; }
    if (ismax ? val_lt(bestk, k) : val_lt(k, bestk)) { decref(best); decref(bestk); best = x; bestk = k; }
    else { decref(x); decref(k); }
  }
  iter_done(&it);
  if (best.t == T_UNDEF) {
    if (dflt) return inc(*dflt);
    throw_error("ValueError", "%s() arg is an empty sequence", name);
  }
  decref(bestk);
  return best;
}
static Value bi_min(ARGS) { if (n == 0) throw_error("TypeError", "min expected at least 1 argument, got 0"); return minmax("min", 0, a, n, kw); }
static Value bi_max(ARGS) { if (n == 0) throw_error("TypeError", "max expected at least 1 argument, got 0"); return minmax("max", 1, a, n, kw); }

static Value bi_sum(ARGS) {
  chk("sum", n, 1, 2);
  Value *st = kwget(kw, "start");
  Value acc = n == 2 ? inc(a[1]) : st ? inc(*st) : V_int(0);
  Iter it; iter_init(&it, a[0]); Value x;
  while (iter_next(&it, &x)) { Value r = binop(OP_ADD, acc, x); decref(acc); decref(x); acc = r; }
  iter_done(&it);
  return acc;
}

static Value bi_round(ARGS) {
  UNUSED; chk("round", n, 1, 2);
  Value x = a[0];
  if (n == 1 || a[1].t == T_NONE) {
    if (x.t == T_INT || x.t == T_BOOL) return V_int(x.i);
    double d = need_num(x, "round()");
    if (isnan(d)) throw_error("ValueError", "cannot convert float NaN to integer");
    if (isinf(d) || fabs(d) >= 9.2e18) throw_error("OverflowError", "cannot convert float to integer");
    return V_int((int64_t)nearbyint(d));
  }
  int64_t nd = need_int(a[1], "ndigits");
  if (x.t == T_INT || x.t == T_BOOL) return V_int(x.i);
  double d = need_num(x, "round()");
  if (isnan(d) || isinf(d)) return V_float(d);
  if (nd >= 0) {
    if (nd > 300) return V_float(d);
    char tmp[400]; snprintf(tmp, sizeof tmp, "%.*f", (int)nd, d);
    return V_float(strtod(tmp, NULL));
  }
  double p = pow(10.0, (double)-nd);
  return V_float(nearbyint(d / p) * p);
}
static Value bi_divmod(ARGS) {
  UNUSED; chk("divmod", n, 2, 2);
  Value q = binop(OP_FDIV, a[0], a[1]), r = binop(OP_MOD, a[0], a[1]);
  Value items[2] = {q, r}; Value t = V_tuple(items, 2); decref(q); decref(r); return t;
}
static Value bi_pow(ARGS) {
  UNUSED; chk("pow", n, 2, 3);
  if (n == 2) return binop(OP_POW, a[0], a[1]);
  int64_t b = need_int(a[0], "pow()"), e = need_int(a[1], "pow()"), m = need_int(a[2], "pow()");
  if (m == 0) throw_error("ValueError", "pow() 3rd argument cannot be 0");
  if (e < 0) throw_error("ValueError", "pow() 2nd argument cannot be negative when 3rd argument specified");
  __int128 r = 1, base = ((b % m) + m) % m;
  while (e > 0) { if (e & 1) r = r * base % m; base = base * base % m; e >>= 1; }
  int64_t res = (int64_t)r; if (m < 0 && res > 0) res += m;
  return V_int(res);
}
static Value bi_chr(ARGS) {
  UNUSED; chk("chr", n, 1, 1);
  int64_t c = need_int(a[0], "chr()");
  if (c < 0 || c > 0x10FFFF) throw_error("ValueError", "chr() arg not in range(0x110000)");
  char b[5]; int len;
  if (c < 0x80) { b[0] = (char)c; len = 1; }
  else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); len = 2; }
  else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); len = 3; }
  else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); len = 4; }
  return V_strn(b, len);
}
static Value bi_ord(ARGS) {
  UNUSED; chk("ord", n, 1, 1);
  Str *s = need_str(a[0], "ord() arg");
  if (s->cplen != 1) throw_error("TypeError", "ord() expected a character, but string of length %d found", s->cplen);
  const unsigned char *p = (const unsigned char *)s->s; int l = utf8_clen(p[0]);
  if (l == 1) return V_int(p[0]);
  if (l == 2) return V_int(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
  if (l == 3) return V_int(((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
  return V_int(((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F));
}
static Value based(int64_t v, int base, const char *prefix) {
  Buf b; buf_init(&b);
  int neg = v < 0; unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
  char t[80]; int k = 0; if (!u) t[k++] = '0';
  while (u) { int d = (int)(u % base); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= base; }
  if (neg) buf_addc(&b, '-');
  buf_adds(&b, prefix);
  while (k) buf_addc(&b, t[--k]);
  return buf_to_str(&b);
}
static Value bi_hex(ARGS) { UNUSED; chk("hex", n, 1, 1); return based(need_int(a[0], "hex()"), 16, "0x"); }
static Value bi_bin(ARGS) { UNUSED; chk("bin", n, 1, 1); return based(need_int(a[0], "bin()"), 2, "0b"); }
static Value bi_oct(ARGS) { UNUSED; chk("oct", n, 1, 1); return based(need_int(a[0], "oct()"), 8, "0o"); }

/* --------------------------------------------------- sequences */
typedef struct { Value *keys; int *idx, *tmp; int reverse; } SortCtx;

Value sorted_list(Value seq, Value keyfn, int reverse) {
  Value lv = list_from_iter(seq); List *l = (List *)lv.o; int nn = l->len;
  if (nn < 2) return lv;
  Value *keys = xmalloc(nn * sizeof(Value));
  int *idx = xmalloc(nn * sizeof(int)), *tmp = xmalloc(nn * sizeof(int));
  int hasfn = keyfn.t != T_NONE && keyfn.t != T_UNDEF;
  for (int i = 0; i < nn; i++) { keys[i] = hasfn ? call_value(keyfn, &l->items[i], 1, NULL) : inc(l->items[i]); idx[i] = i; }
  for (int w = 1; w < nn; w *= 2) {
    for (int lo = 0; lo < nn; lo += 2 * w) {
      int mid = lo + w < nn ? lo + w : nn, hi = lo + 2 * w < nn ? lo + 2 * w : nn;
      int i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        int takeright = reverse ? val_lt(keys[idx[i]], keys[idx[j]]) : val_lt(keys[idx[j]], keys[idx[i]]);
        tmp[k++] = takeright ? idx[j++] : idx[i++];
      }
      while (i < mid) tmp[k++] = idx[i++];
      while (j < hi) tmp[k++] = idx[j++];
    }
    memcpy(idx, tmp, nn * sizeof(int));
  }
  Value *sorted = xmalloc(nn * sizeof(Value));
  for (int i = 0; i < nn; i++) sorted[i] = l->items[idx[i]];
  memcpy(l->items, sorted, nn * sizeof(Value));
  for (int i = 0; i < nn; i++) decref(keys[i]);
  free(sorted); free(keys); free(idx); free(tmp);
  return lv;
}
static Value bi_sorted(ARGS) {
  chk("sorted", n, 1, 1);
  Value *k = kwget(kw, "key"), *r = kwget(kw, "reverse");
  return sorted_list(a[0], k ? *k : V_none(), r ? val_truthy(*r) : 0);
}
static Value bi_reversed(ARGS) {
  UNUSED; chk("reversed", n, 1, 1);
  Value lv = list_from_iter(a[0]); List *l = (List *)lv.o;
  for (int i = 0, j = l->len - 1; i < j; i++, j--) { Value t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; }
  Value it = make_iterator(lv); decref(lv);
  return it;
}
static Value iters_tuple(Value *a, int n) {
  List *l = list_new(n); l->h.type = T_TUPLE;
  for (int i = 0; i < n; i++) list_append_own(l, make_iterator(a[i]));
  return V_obj(l, T_TUPLE);
}
static Value bi_enumerate(ARGS) {
  chk("enumerate", n, 1, 2);
  int64_t start = 0; Value *k = kwget(kw, "start");
  if (n == 2) start = need_int(a[1], "start"); else if (k) start = need_int(*k, "start");
  Value src = make_iterator(a[0]);
  Value r = make_iterobj(IT_ENUM, V_none(), src, V_none()); decref(src);
  iterobj_init_nums(r, start, 0, 0, 0, 0);
  return r;
}
static Value bi_zip(ARGS) {
  UNUSED;
  Value tv = iters_tuple(a, n); Value r = make_iterobj(IT_ZIP, V_none(), tv, V_none()); decref(tv); return r;
}
static Value bi_map(ARGS) {
  UNUSED;
  if (n < 2) throw_error("TypeError", "map() must have at least two arguments.");
  Value tv = iters_tuple(a + 1, n - 1); Value r = make_iterobj(IT_MAP, a[0], tv, V_none()); decref(tv); return r;
}
static Value bi_filter(ARGS) {
  UNUSED; chk("filter", n, 2, 2);
  Value src = make_iterator(a[1]); Value r = make_iterobj(IT_FILTER, a[0], src, V_none()); decref(src); return r;
}
static Value bi_iter(ARGS) {
  UNUSED; chk("iter", n, 1, 2);
  if (n == 2) return make_iterobj(IT_CALLIT, a[0], a[1], V_none());
  return make_iterator(a[0]);
}
static Value bi_next(ARGS) {
  UNUSED; chk("next", n, 1, 2);
  Value it = a[0], x;
  if (it.t != T_ITER && it.t != T_GEN && it.t != T_INST) throw_error("TypeError", "'%s' object is not an iterator", type_name(it));
  if (iter_step(it, &x)) return x;
  if (n == 2) return inc(a[1]);
  throw_error("StopIteration", "%s", "");
}
static Value bi_any(ARGS) {
  UNUSED; chk("any", n, 1, 1);
  Iter it; iter_init(&it, a[0]); Value x;
  while (iter_next(&it, &x)) { int t = val_truthy(x); decref(x); if (t) { iter_done(&it); return V_bool(1); } }
  iter_done(&it); return V_bool(0);
}
static Value bi_all(ARGS) {
  UNUSED; chk("all", n, 1, 1);
  Iter it; iter_init(&it, a[0]); Value x;
  while (iter_next(&it, &x)) { int t = val_truthy(x); decref(x); if (!t) { iter_done(&it); return V_bool(0); } }
  iter_done(&it); return V_bool(1);
}

/* ---------------------------------------------------- reflection */
static Value bi_hasattr(ARGS) {
  UNUSED; chk("hasattr", n, 2, 2);
  Value out; Str *nm = intern(need_str(a[1], "attribute name")->s, ((Str *)a[1].o)->len);
  if (try_get_attr(a[0], nm, &out)) { decref(out); return V_bool(1); }
  return V_bool(0);
}
static Value bi_getattr(ARGS) {
  UNUSED; chk("getattr", n, 2, 3);
  Value out; Str *nm = intern(need_str(a[1], "attribute name")->s, ((Str *)a[1].o)->len);
  if (n == 2) return get_attr(a[0], nm);
  if (try_get_attr(a[0], nm, &out)) return out;
  return inc(a[2]);
}
static Value bi_setattr(ARGS) {
  UNUSED; chk("setattr", n, 3, 3);
  Str *nm = intern(need_str(a[1], "attribute name")->s, ((Str *)a[1].o)->len);
  set_attr(a[0], nm, a[2]); return V_none();
}
static Value bi_callable(ARGS) {
  UNUSED; chk("callable", n, 1, 1);
  Value v = a[0];
  if (v.t == T_FUNC || v.t == T_BUILTIN || v.t == T_BOUND || v.t == T_CLASS) return V_bool(1);
  if (v.t == T_INST) { Value out; (void)out; return V_bool(dict_find_cstr(((Inst *)v.o)->cls->attrs, "__call__") != NULL); }
  return V_bool(0);
}
static Value bi_hash(ARGS) {
  UNUSED; chk("hash", n, 1, 1);
  if (a[0].t == T_INT || a[0].t == T_BOOL) return V_int(a[0].i);
  return V_int((int64_t)val_hash(a[0]));
}
static Value bi_id(ARGS) { UNUSED; chk("id", n, 1, 1); return V_int(IS_OBJ(a[0]) ? (int64_t)(uintptr_t)a[0].o : a[0].i); }
static Value bi_format(ARGS) {
  UNUSED; chk("format", n, 1, 2);
  return format_value(a[0], n == 2 ? need_str(a[1], "format spec")->s : NULL);
}
static Value bi_super(ARGS) {
  UNUSED; chk("super", n, 0, 2);
  Super *s = xmalloc(sizeof(Super));
  s->h.rc = 1; s->h.type = T_SUPER;
  if (n == 2) {
    if (a[0].t != T_CLASS) { free(s); throw_error("TypeError", "super() argument 1 must be a type"); }
    s->self = inc(a[1]); s->owner = (Class *)a[0].o;
  } else {
    Frame *fr = g_frame;
    if (!fr || !fr->fn || !fr->fn->owner || fr->fn->def->nparams < 1) { free(s); throw_error("RuntimeError", "super(): no arguments and not inside a method"); }
    Value *self = &fr->env->slots[0];
    if (self->t == T_UNDEF) { free(s); throw_error("RuntimeError", "super(): missing self"); }
    s->self = inc(*self); s->owner = fr->fn->owner;
  }
  oinc(s->owner);
  return V_obj(s, T_SUPER);
}

static Value bi_exit(ARGS) {
  UNUSED; chk("exit", n, 0, 1);
  fflush(stdout);
  int code = 0;
  if (n == 1) {
    if (a[0].t == T_INT) code = (int)a[0].i;
    else if (a[0].t != T_NONE) { Str *s = val_str(a[0]); fprintf(stderr, "%s\n", s->s); odec(s); code = 1; }
  }
  exit(code);
}

static Value bi_open(ARGS) {
  UNUSED; chk("open", n, 1, 2);
  Str *path = need_str(a[0], "open() path");
  const char *mode = "r";
  if (n == 2) mode = need_str(a[1], "open() mode")->s;
  char m[8]; int k = 0;
  for (const char *p = mode; *p && k < 6; p++) if (strchr("rwax+", *p)) m[k++] = *p;
  m[k] = 0;
  if (!k) throw_error("ValueError", "invalid mode: '%s'", mode);
  FILE *fp = fopen(path->s, m);
  if (!fp) {
    int e = errno;
    throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), path->s);
  }
  return make_file(fp);
}

/* convenience helpers kept from early cpy versions: read("file") / write("file", text) */
static Value bi_read(ARGS) {
  UNUSED; chk("read", n, 1, 1);
  Str *path = need_str(a[0], "read() path");
  long len; char *src = read_file(path->s, &len);
  if (!src) { int e = errno; throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), path->s); }
  Value v = V_strn(src, (int)len); free(src); return v;
}
static Value bi_write(ARGS) {
  UNUSED; chk("write", n, 2, 2);
  Str *path = need_str(a[0], "write() path"), *text = need_str(a[1], "write() text");
  FILE *fp = fopen(path->s, "wb");
  if (!fp) { int e = errno; throw_error("OSError", "[Errno %d] %s: '%s'", e, strerror(e), path->s); }
  fwrite(text->s, 1, text->len, fp); fclose(fp);
  return V_int(text->cplen);
}
static Value bi_globals(ARGS) {
  UNUSED; chk("globals", n, 0, 0);
  Dict *d = dict_new(); Value dv = V_obj(d, T_DICT);
  Dict *src = g_globals->vars;
  for (int i = 0; i < src->n; i++) if (src->e[i].key.t != T_UNDEF) dict_set(d, src->e[i].key, src->e[i].val);
  return dv;
}
static Value bi_help(ARGS) {
  UNUSED;
  puts("cpy " CPY_VERSION " - a small Python-like language\n"
       "\n"
       "  statements : if/elif/else, while, for, def, class, try/except/finally, with,\n"
       "               import, from, return, break, continue, pass, raise, assert, del\n"
       "  types      : int (64-bit), float, str, bool, None, list, tuple, dict, range\n"
       "  builtins   : print input len int float str bool list tuple dict range type\n"
       "               isinstance abs min max sum sorted reversed enumerate zip map\n"
       "               filter any all round divmod pow chr ord hex bin oct repr\n"
       "               hasattr getattr setattr callable open read write globals exit\n"
       "  modules    : math time random sys os  (or your own .cpy files)\n"
       "\n"
       "  read(path) / write(path, text) are shortcuts for whole-file I/O.\n"
       "  Full reference: docs/language.md and docs/builtins.md");
  return V_none();
}


/* ------------------------------------------------ descriptors / sets / dict kinds */
static Value make_descr(int kind, Value f, Value s, Value d) {
  Descr *ds = xmalloc(sizeof(Descr));
  ds->h.rc = 1; ds->h.type = T_DESCR; ds->kind = kind; ds->fget = inc(f); ds->fset = inc(s); ds->fdel = inc(d);
  return V_obj(ds, T_DESCR);
}
static Value bi_staticmethod(ARGS) { UNUSED; chk("staticmethod", n, 1, 1); return make_descr(DS_STATIC, a[0], V_none(), V_none()); }
static Value bi_classmethod(ARGS) { UNUSED; chk("classmethod", n, 1, 1); return make_descr(DS_CLASS, a[0], V_none(), V_none()); }
static Value bi_property(ARGS) {
  chk("property", n, 0, 4);
  Value g = n > 0 ? a[0] : V_none(), s = n > 1 ? a[1] : V_none(), d = n > 2 ? a[2] : V_none(), *k;
  if ((k = kwget(kw, "fget"))) g = *k;
  if ((k = kwget(kw, "fset"))) s = *k;
  if ((k = kwget(kw, "fdel"))) d = *k;
  return make_descr(DS_PROP, g, s, d);
}
static Value prop_with(Value *a, int n, int which) {
  chk("setter", n, 2, 2);
  Descr *d = (Descr *)a[0].o;
  return make_descr(DS_PROP, which == 0 ? a[1] : d->fget, which == 1 ? a[1] : d->fset, which == 2 ? a[1] : d->fdel);
}
static Value bi_prop_getter(ARGS) { UNUSED; return prop_with(a, n, 0); }
static Value bi_prop_setter(ARGS) { UNUSED; return prop_with(a, n, 1); }
static Value bi_prop_deleter(ARGS) { UNUSED; return prop_with(a, n, 2); }

static Value bi_set(ARGS) { UNUSED; chk("set", n, 0, 1); return n ? new_set_from_iter(a[0], DK_SET) : new_set(DK_SET); }
static Value bi_frozenset(ARGS) { UNUSED; chk("frozenset", n, 0, 1); return n ? new_set_from_iter(a[0], DK_FROZEN) : new_set(DK_FROZEN); }

static Value dict_kind(int kind, Value factory, Value *a, int n, Kw *kw) {
  if (kind == DK_COUNTER) {
    Dict *d = dict_new(); d->kind = DK_COUNTER; Value dv = V_obj(d, T_DICT);
    if (n >= 1 && a[0].t == T_DICT) {
      Dict *s = (Dict *)a[0].o;
      for (int i = 0; i < s->n; i++) if (s->e[i].key.t != T_UNDEF) dict_set(d, s->e[i].key, s->e[i].val);
    } else if (n >= 1 && a[0].t != T_NONE) {
      Iter it; iter_init(&it, a[0]); Value x;
      while (iter_next(&it, &x)) { Value *cur = dict_find(d, x); dict_set(d, x, V_int(cur && cur->t == T_INT ? cur->i + 1 : 1)); decref(x); }
      iter_done(&it);
    }
    if (kw) for (int i = 0; i < kw->n; i++) dict_set_str(d, kw->names[i], kw->vals[i]);
    return dv;
  }
  Value dv = bi_dict(a, n, kw);
  Dict *d = (Dict *)dv.o; d->kind = kind; d->factory = inc(factory);
  return dv;
}
static Value bi_defaultdict(ARGS) {
  Value factory = n > 0 ? a[0] : V_none();
  return dict_kind(DK_DEFAULT, factory, a + (n > 0), n > 0 ? n - 1 : 0, kw);
}
static Value bi_counter(ARGS) { return dict_kind(DK_COUNTER, V_none(), a, n, kw); }
static Value bi_ordereddict(ARGS) { return dict_kind(DK_ORDERED, V_none(), a, n, kw); }

static Value bi_issubclass(ARGS) {
  UNUSED; chk("issubclass", n, 2, 2);
  if (a[0].t != T_CLASS) throw_error("TypeError", "issubclass() arg 1 must be a class");
  Class *c = (Class *)a[0].o;
  if (a[1].t == T_TUPLE) { List *l = (List *)a[1].o; for (int i = 0; i < l->len; i++) if (l->items[i].t == T_CLASS && class_is_subclass(c, (Class *)l->items[i].o)) return V_bool(1); return V_bool(0); }
  if (a[1].t != T_CLASS) throw_error("TypeError", "issubclass() arg 2 must be a class");
  Class *b = (Class *)a[1].o;
  if (!b->base && b->nmro == 1 && !strcmp(b->name->s, "object")) return V_bool(1);
  return V_bool(class_is_subclass(c, b));
}

/* default methods every object has (object.__init__, __repr__, ...) */
static Dict *object_methods;
static Value obj_new(ARGS) {
  UNUSED; if (n < 1 || a[0].t != T_CLASS) throw_error("TypeError", "object.__new__(X): X is not a type object");
  Inst *in = xmalloc(sizeof(Inst)); in->h.rc = 1; in->h.type = T_INST; in->cls = (Class *)a[0].o; oinc(in->cls); in->attrs = dict_new();
  return V_obj(in, T_INST);
}
static Value obj_init(ARGS) { UNUSED; return V_none(); }
static Value obj_repr(ARGS) { UNUSED; if (a[0].t == T_INST) return V_strf("<%s object>", ((Inst *)a[0].o)->cls->name->s); return V_obj(val_repr(a[0]), T_STR); }
static Value obj_str(ARGS) { UNUSED; return V_obj(val_repr(a[0]), T_STR); }
static Value obj_eq(ARGS) { UNUSED; chk("__eq__", n, 2, 2); return V_bool(IS_OBJ(a[0]) && IS_OBJ(a[1]) && a[0].o == a[1].o); }
static Value obj_ne(ARGS) { UNUSED; chk("__ne__", n, 2, 2); return V_bool(!(IS_OBJ(a[0]) && IS_OBJ(a[1]) && a[0].o == a[1].o)); }
static Value obj_hash(ARGS) { UNUSED; return V_int((int64_t)((uintptr_t)a[0].o >> 4)); }
static Value obj_setattr(ARGS) {
  UNUSED; chk("__setattr__", n, 3, 3);
  if (a[0].t == T_INST) dict_set(((Inst *)a[0].o)->attrs, a[1], a[2]);
  else if (a[0].t == T_CLASS) dict_set(((Class *)a[0].o)->attrs, a[1], a[2]);
  else throw_error("AttributeError", "cannot set attribute");
  return V_none();
}
Value default_object_method(Value self, Str *name) {
  Value *b = dict_find_str(object_methods, name);
  if (!b) return V_undef();
  if (self.t == T_UNDEF) return inc(*b);
  return make_bound(self, *b);
}

static Value bi_noop_init(ARGS) { UNUSED; return V_none(); }

void builtins_init(void) {
  reg_builtin("print", bi_print); reg_builtin("input", bi_input); reg_builtin("len", bi_len);
  reg_builtin("int", bi_int); reg_builtin("float", bi_float); reg_builtin("str", bi_str);
  reg_builtin("repr", bi_repr); reg_builtin("bool", bi_bool); reg_builtin("list", bi_list);
  reg_builtin("tuple", bi_tuple); reg_builtin("dict", bi_dict); reg_builtin("range", bi_range);
  reg_builtin("type", bi_type); reg_builtin("isinstance", bi_isinstance);
  reg_builtin("NoneType", bi_dummy_none); reg_builtin("function", bi_dummy_none);
  reg_builtin("abs", bi_abs); reg_builtin("min", bi_min); reg_builtin("max", bi_max);
  reg_builtin("sum", bi_sum); reg_builtin("round", bi_round); reg_builtin("divmod", bi_divmod);
  reg_builtin("pow", bi_pow); reg_builtin("chr", bi_chr); reg_builtin("ord", bi_ord);
  reg_builtin("hex", bi_hex); reg_builtin("bin", bi_bin); reg_builtin("oct", bi_oct);
  reg_builtin("sorted", bi_sorted); reg_builtin("reversed", bi_reversed); reg_builtin("enumerate", bi_enumerate);
  reg_builtin("zip", bi_zip); reg_builtin("map", bi_map); reg_builtin("filter", bi_filter);
  reg_builtin("iter", bi_iter); reg_builtin("next", bi_next); reg_builtin("any", bi_any); reg_builtin("all", bi_all);
  reg_builtin("hasattr", bi_hasattr); reg_builtin("getattr", bi_getattr); reg_builtin("setattr", bi_setattr);
  reg_builtin("callable", bi_callable); reg_builtin("hash", bi_hash); reg_builtin("id", bi_id); reg_builtin("format", bi_format);
  reg_builtin("super", bi_super); reg_builtin("exit", bi_exit); reg_builtin("quit", bi_exit);
  reg_builtin("open", bi_open); reg_builtin("__noop_init", bi_noop_init);
  reg_builtin("read", bi_read); reg_builtin("write", bi_write);
  reg_builtin("staticmethod", bi_staticmethod); reg_builtin("classmethod", bi_classmethod); reg_builtin("property", bi_property);
  reg_builtin("__prop_getter", bi_prop_getter); reg_builtin("__prop_setter", bi_prop_setter); reg_builtin("__prop_deleter", bi_prop_deleter);
  reg_builtin("set", bi_set); reg_builtin("frozenset", bi_frozenset);
  reg_builtin("defaultdict", bi_defaultdict); reg_builtin("Counter", bi_counter); reg_builtin("OrderedDict", bi_ordereddict);
  reg_builtin("issubclass", bi_issubclass); reg_builtin("Ellipsis", bi_dummy_none);
  object_methods = dict_new();
  { struct { const char *n; BuiltinFn f; } om[] = {{"__init__", obj_init}, {"__repr__", obj_repr}, {"__str__", obj_str}, {"__eq__", obj_eq}, {"__ne__", obj_ne}, {"__hash__", obj_hash}, {"__setattr__", obj_setattr}, {"__new__", obj_new}};
    for (unsigned i = 0; i < sizeof om / sizeof om[0]; i++) { Value bv = new_builtin(om[i].n, om[i].f); dict_set_cstr(object_methods, om[i].n, bv); decref(bv); } }
  reg_builtin("globals", bi_globals); reg_builtin("vars", bi_globals); reg_builtin("help", bi_help);
  methods_init();
}
