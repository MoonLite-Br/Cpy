#define _GNU_SOURCE
/* methods.c - methods of builtin types (str, list, tuple, dict, file) and builtin modules */
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include "cpy.h"

#define ARGS Value *a, int n, Kw *kw
#define UNUSED (void)a; (void)n; (void)kw
#define SELF ((Str *)a[0].o)

static Dict *str_methods, *list_methods, *tuple_methods, *dict_methods, *file_methods, *set_methods;

static void regm(Dict *t, const char *name, BuiltinFn fn) {
  Value b = new_builtin(name, fn);
  dict_set_cstr(t, name, b); decref(b);
}

Value *builtin_method_raw(Value obj, Str *name) {
  Dict *t = NULL;
  switch (obj.t) {
  case T_STR: t = str_methods; break;
  case T_LIST: t = list_methods; break;
  case T_TUPLE: t = tuple_methods; break;
  case T_DICT: t = dict_methods; break;
  case T_FILE: t = file_methods; break;
  case T_SET: t = set_methods; break;
  default: return NULL;
  }
  return dict_find_str(t, name);
}

Value builtin_method(Value obj, Str *name) {
  Dict *t = NULL;
  switch (obj.t) {
  case T_STR: t = str_methods; break;
  case T_LIST: t = list_methods; break;
  case T_TUPLE: t = tuple_methods; break;
  case T_DICT: t = dict_methods; break;
  case T_FILE: t = file_methods; break;
  case T_SET: t = set_methods; break;
  case T_ITER: case T_GEN: return gen_get_method(obj, name);
  default: return V_undef();
  }
  Value *f = dict_find_str(t, name);
  if (!f) return V_undef();
  return make_bound(obj, *f);
}

static int cp_of(Str *s, int byteoff) {
  if (s->ascii) return byteoff;
  int c = 0;
  for (int i = 0; i < byteoff && i < s->len; i++) if (((unsigned char)s->s[i] & 0xC0) != 0x80) c++;
  return c;
}

/* ================================================================ str */
/* --- minimal Unicode case mapping: Latin (incl. Vietnamese), Greek, Cyrillic --- */
static int cp_decode(const char *s, int len, int *clen) {
  unsigned char c = (unsigned char)s[0]; int l = utf8_clen(c);
  if (l > len) l = 1;
  *clen = l;
  if (l == 1) return c;
  if (l == 2) return ((c & 0x1F) << 6) | (s[1] & 0x3F);
  if (l == 3) return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
}
static void cp_encode(Buf *b, int c) {
  if (c < 0x80) buf_addc(b, (char)c);
  else if (c < 0x800) { buf_addc(b, (char)(0xC0 | (c >> 6))); buf_addc(b, (char)(0x80 | (c & 0x3F))); }
  else if (c < 0x10000) { buf_addc(b, (char)(0xE0 | (c >> 12))); buf_addc(b, (char)(0x80 | ((c >> 6) & 0x3F))); buf_addc(b, (char)(0x80 | (c & 0x3F))); }
  else { buf_addc(b, (char)(0xF0 | (c >> 18))); buf_addc(b, (char)(0x80 | ((c >> 12) & 0x3F))); buf_addc(b, (char)(0x80 | ((c >> 6) & 0x3F))); buf_addc(b, (char)(0x80 | (c & 0x3F))); }
}
static int cp_to_upper(int c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
  if (c == 0xFF) return 0x178;
  if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && (c & 1) && c != 0x131) return c - 1;
  if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1)) return c - 1;
  if (c == 0x1A1 || c == 0x1B0) return c - 1;
  if (c >= 0x1EA0 && c <= 0x1EF9 && (c & 1)) return c - 1;
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
  if (c >= 0x430 && c <= 0x44F) return c - 0x20;
  if (c >= 0x450 && c <= 0x45F) return c - 0x50;
  return c;
}
static int cp_to_lower(int c) {
  if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
  if (c == 0x178) return 0xFF;
  if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && !(c & 1) && c != 0x130) return c + 1;
  if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1)) return c + 1;
  if (c == 0x1A0 || c == 0x1AF) return c + 1;
  if (c >= 0x1EA0 && c <= 0x1EF9 && !(c & 1)) return c + 1;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
  if (c >= 0x410 && c <= 0x42F) return c + 0x20;
  if (c >= 0x400 && c <= 0x40F) return c + 0x50;
  return c;
}
static int cp_is_alpha(int c) {
  if (c < 0x80) return isalpha(c);
  return (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) || (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x4FF) ||
         (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x3040 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || c == 0xAA || c == 0xBA || c == 0xB5;
}
/* mode: 0 upper, 1 lower, 2 swapcase, 3 capitalize, 4 title */
static Value str_case(Str *s, int mode) {
  Buf b; buf_init(&b); buf_add(&b, "", 0);
  int i = 0, first = 1, start = 1;
  while (i < s->len) {
    int cl; int c = cp_decode(s->s + i, s->len - i, &cl); i += cl;
    int r = c;
    switch (mode) {
    case 0: r = cp_to_upper(c); break;
    case 1: r = cp_to_lower(c); break;
    case 2: r = cp_to_upper(c) != c ? cp_to_upper(c) : cp_to_lower(c); break;
    case 3: r = first ? cp_to_upper(c) : cp_to_lower(c); break;
    case 4: r = start ? cp_to_upper(c) : cp_to_lower(c); start = !cp_is_alpha(c); break;
    }
    first = 0;
    cp_encode(&b, r);
  }
  return buf_to_str(&b);
}
static Value s_upper(ARGS) { UNUSED; return str_case(SELF, 0); }
static Value s_lower(ARGS) { UNUSED; return str_case(SELF, 1); }
static Value s_swapcase(ARGS) { UNUSED; return str_case(SELF, 2); }
static Value s_capitalize(ARGS) { UNUSED; return str_case(SELF, 3); }
static Value s_title(ARGS) { UNUSED; return str_case(SELF, 4); }
static Value strip_impl(Value *a, int n, int left, int right) {
  chk("strip", n, 1, 2);
  Str *s = SELF; int lo = 0, hi = s->len;
  Str *chars = (n == 2 && a[1].t == T_STR) ? (Str *)a[1].o : NULL;
#define IS_STRIP(c) (chars ? memchr(chars->s, (c), chars->len) != NULL : isspace((unsigned char)(c)))
  if (left) while (lo < hi && IS_STRIP(s->s[lo])) lo++;
  if (right) while (hi > lo && IS_STRIP(s->s[hi - 1])) hi--;
  return V_strn(s->s + lo, hi - lo);
}
static Value s_strip(ARGS) { (void)kw; return strip_impl(a, n, 1, 1); }
static Value s_lstrip(ARGS) { (void)kw; return strip_impl(a, n, 1, 0); }
static Value s_rstrip(ARGS) { (void)kw; return strip_impl(a, n, 0, 1); }

static Value s_split(ARGS) {
  chk("split", n, 1, 3);
  Str *s = SELF; Value *k;
  Value sepv = n >= 2 ? a[1] : V_none();
  if ((k = kwget(kw, "sep"))) sepv = *k;
  int64_t maxs = -1;
  if (n == 3) maxs = need_int(a[2], "maxsplit");
  if ((k = kwget(kw, "maxsplit"))) maxs = need_int(*k, "maxsplit");
  List *out = list_new(0); Value res = V_obj(out, T_LIST);
  if (sepv.t == T_NONE) {
    int i = 0;
    while (i < s->len) {
      while (i < s->len && isspace((unsigned char)s->s[i])) i++;
      if (i >= s->len) break;
      if (maxs >= 0 && out->len >= maxs) { int e = s->len; while (e > i && isspace((unsigned char)s->s[e - 1])) e--; list_append_own(out, V_strn(s->s + i, e - i)); return res; }
      int j = i; while (j < s->len && !isspace((unsigned char)s->s[j])) j++;
      list_append_own(out, V_strn(s->s + i, j - i)); i = j;
    }
    return res;
  }
  Str *sep = need_str(sepv, "sep");
  if (sep->len == 0) { decref(res); throw_error("ValueError", "empty separator"); }
  int i = 0;
  for (;;) {
    const char *f = (maxs >= 0 && out->len >= maxs) ? NULL : memmem(s->s + i, s->len - i, sep->s, sep->len);
    if (!f) { list_append_own(out, V_strn(s->s + i, s->len - i)); break; }
    list_append_own(out, V_strn(s->s + i, (int)(f - (s->s + i)))); i = (int)(f - s->s) + sep->len;
  }
  return res;
}
static Value s_join(ARGS) {
  UNUSED; chk("join", n, 2, 2);
  Str *sep = SELF; Buf b; buf_init(&b); buf_add(&b, "", 0);
  Iter it; iter_init(&it, a[1]); Value x; int first = 1;
  while (iter_next(&it, &x)) {
    if (x.t != T_STR) { char t[96]; snprintf(t, sizeof t, "%s", type_name(x)); decref(x); iter_done(&it); free(b.p); throw_error("TypeError", "sequence item: expected str instance, %s found", t); }
    if (!first) buf_add(&b, sep->s, sep->len);
    first = 0; buf_add(&b, ((Str *)x.o)->s, ((Str *)x.o)->len); decref(x);
  }
  iter_done(&it);
  return buf_to_str(&b);
}
static Value s_replace(ARGS) {
  UNUSED; chk("replace", n, 3, 4);
  Str *s = SELF, *o = need_str(a[1], "old"), *r = need_str(a[2], "new");
  int64_t cnt = n == 4 ? need_int(a[3], "count") : -1;
  if (o->len == 0) throw_error("ValueError", "empty pattern in replace()");
  Buf b; buf_init(&b); buf_add(&b, "", 0); int i = 0;
  while (i < s->len) {
    const char *f = (cnt == 0) ? NULL : memmem(s->s + i, s->len - i, o->s, o->len);
    if (!f) break;
    buf_add(&b, s->s + i, (int)(f - (s->s + i))); buf_add(&b, r->s, r->len);
    i = (int)(f - s->s) + o->len; if (cnt > 0) cnt--;
  }
  buf_add(&b, s->s + i, s->len - i);
  return buf_to_str(&b);
}
static int find_impl(Value *a, int n, int rev) {
  Str *s = SELF, *t = need_str(a[1], "argument");
  int start = 0;
  if (n >= 3 && a[2].t != T_NONE) { int64_t st = need_int(a[2], "start"); if (st < 0) st += s->cplen; if (st < 0) st = 0; if (st > s->cplen) return -1; start = str_cp_offset(s, (int)st); }
  int end = s->len;
  if (n >= 4 && a[3].t != T_NONE) { int64_t en = need_int(a[3], "end"); if (en < 0) en += s->cplen; if (en < 0) en = 0; if (en > s->cplen) en = s->cplen; end = str_cp_offset(s, (int)en); }
  if (end < start) return -1;
  const char *f = NULL;
  if (!rev) f = memmem(s->s + start, end - start, t->s, t->len);
  else for (int i = end - t->len; i >= start; i--) if (memcmp(s->s + i, t->s, t->len) == 0) { f = s->s + i; break; }
  if (!f) return -1;
  return cp_of(s, (int)(f - s->s));
}
static Value s_find(ARGS) { (void)kw; chk("find", n, 2, 4); return V_int(find_impl(a, n, 0)); }
static Value s_rfind(ARGS) { (void)kw; chk("rfind", n, 2, 4); return V_int(find_impl(a, n, 1)); }
static Value s_index(ARGS) { (void)kw; chk("index", n, 2, 4); int r = find_impl(a, n, 0); if (r < 0) throw_error("ValueError", "substring not found"); return V_int(r); }
static Value s_rindex(ARGS) { (void)kw; chk("rindex", n, 2, 4); int r = find_impl(a, n, 1); if (r < 0) throw_error("ValueError", "substring not found"); return V_int(r); }
static Value s_count(ARGS) {
  UNUSED; chk("count", n, 2, 2);
  Str *s = SELF, *t = need_str(a[1], "argument");
  if (t->len == 0) return V_int(s->cplen + 1);
  int c = 0, i = 0;
  for (;;) { const char *f = memmem(s->s + i, s->len - i, t->s, t->len); if (!f) break; c++; i = (int)(f - s->s) + t->len; }
  return V_int(c);
}
static int affix(Value *a, int n, int suffix) {
  chk(suffix ? "endswith" : "startswith", n, 2, 2);
  Str *s = SELF;
  if (a[1].t == T_TUPLE) {
    List *l = (List *)a[1].o;
    for (int i = 0; i < l->len; i++) { Value args[2] = {a[0], l->items[i]}; if (affix(args, 2, suffix)) return 1; }
    return 0;
  }
  Str *t = need_str(a[1], "prefix/suffix");
  if (t->len > s->len) return 0;
  return memcmp(suffix ? s->s + s->len - t->len : s->s, t->s, t->len) == 0;
}
static Value s_startswith(ARGS) { (void)kw; return V_bool(affix(a, n, 0)); }
static Value s_endswith(ARGS) { (void)kw; return V_bool(affix(a, n, 1)); }

static Value s_isdigit(ARGS) {
  UNUSED; Str *s = SELF;
  if (s->len == 0) return V_bool(0);
  for (int i = 0; i < s->len; i++) if (!isdigit((unsigned char)s->s[i])) return V_bool(0);
  return V_bool(1);
}
static Value s_isspace(ARGS) {
  UNUSED; Str *s = SELF;
  if (s->len == 0) return V_bool(0);
  for (int i = 0; i < s->len; i++) if (!isspace((unsigned char)s->s[i])) return V_bool(0);
  return V_bool(1);
}
static Value alpha_impl(Value *a, int digits_ok) {
  Str *s = SELF; int i = 0;
  if (s->len == 0) return V_bool(0);
  while (i < s->len) {
    int cl; int c = cp_decode(s->s + i, s->len - i, &cl); i += cl;
    if (!(cp_is_alpha(c) || (digits_ok && c < 0x80 && isdigit(c)))) return V_bool(0);
  }
  return V_bool(1);
}
static Value s_isalpha(ARGS) { UNUSED; return alpha_impl(a, 0); }
static Value s_isalnum(ARGS) { UNUSED; return alpha_impl(a, 1); }
static Value s_isupper(ARGS) {
  UNUSED; Str *s = SELF; int any = 0, i = 0;
  while (i < s->len) { int cl; int c = cp_decode(s->s + i, s->len - i, &cl); i += cl; if (cp_to_lower(c) != c) any = 1; else if (cp_to_upper(c) != c) return V_bool(0); }
  return V_bool(any);
}
static Value s_islower(ARGS) {
  UNUSED; Str *s = SELF; int any = 0, i = 0;
  while (i < s->len) { int cl; int c = cp_decode(s->s + i, s->len - i, &cl); i += cl; if (cp_to_upper(c) != c) any = 1; else if (cp_to_lower(c) != c) return V_bool(0); }
  return V_bool(any);
}
static Value justify(Value *a, int n, char mode) {
  chk("ljust", n, 2, 3);
  Str *s = SELF; int64_t w = need_int(a[1], "width");
  char fill = ' '; if (n == 3) { Str *f = need_str(a[2], "fillchar"); if (f->len != 1) throw_error("TypeError", "The fill character must be exactly one character long"); fill = f->s[0]; }
  if (w <= s->cplen) return inc(a[0]);
  int pad = (int)(w - s->cplen), l = 0, r = 0;
  if (mode == 'l') r = pad; else if (mode == 'r') l = pad; else { l = pad / 2; r = pad - l; }
  Buf b; buf_init(&b); buf_add(&b, "", 0);
  for (int i = 0; i < l; i++) buf_addc(&b, fill);
  buf_add(&b, s->s, s->len);
  for (int i = 0; i < r; i++) buf_addc(&b, fill);
  return buf_to_str(&b);
}
static Value s_ljust(ARGS) { (void)kw; return justify(a, n, 'l'); }
static Value s_rjust(ARGS) { (void)kw; return justify(a, n, 'r'); }
static Value s_center(ARGS) { (void)kw; return justify(a, n, 'c'); }
static Value s_zfill(ARGS) {
  UNUSED; chk("zfill", n, 2, 2);
  Str *s = SELF; int64_t w = need_int(a[1], "width");
  if (w <= s->cplen) return inc(a[0]);
  Buf b; buf_init(&b); buf_add(&b, "", 0); int i = 0;
  if (s->len && (s->s[0] == '-' || s->s[0] == '+')) { buf_addc(&b, s->s[0]); i = 1; }
  for (int k = 0; k < w - s->cplen; k++) buf_addc(&b, '0');
  buf_add(&b, s->s + i, s->len - i);
  return buf_to_str(&b);
}
static Value s_splitlines(ARGS) {
  UNUSED; Str *s = SELF; List *out = list_new(0); int i = 0;
  while (i < s->len) {
    int j = i; while (j < s->len && s->s[j] != '\n' && s->s[j] != '\r') j++;
    list_append_own(out, V_strn(s->s + i, j - i));
    if (j < s->len && s->s[j] == '\r' && j + 1 < s->len && s->s[j + 1] == '\n') j++;
    i = j + 1;
  }
  return V_obj(out, T_LIST);
}
static Value s_partition(ARGS) {
  UNUSED; chk("partition", n, 2, 2);
  Str *s = SELF, *t = need_str(a[1], "sep");
  if (t->len == 0) throw_error("ValueError", "empty separator");
  const char *f = memmem(s->s, s->len, t->s, t->len);
  Value items[3];
  if (!f) { items[0] = inc(a[0]); items[1] = V_str(""); items[2] = V_str(""); }
  else { items[0] = V_strn(s->s, (int)(f - s->s)); items[1] = inc(a[1]); items[2] = V_strn(f + t->len, s->len - (int)(f - s->s) - t->len); }
  Value r = V_tuple(items, 3); for (int i = 0; i < 3; i++) decref(items[i]);
  return r;
}
static Value s_format(ARGS) {
  Str *t = SELF; Buf b; buf_init(&b); buf_add(&b, "", 0); int auto_i = 0;
  for (int i = 0; i < t->len; i++) {
    char c = t->s[i];
    if (c == '{' && i + 1 < t->len && t->s[i + 1] == '{') { buf_addc(&b, '{'); i++; continue; }
    if (c == '}' && i + 1 < t->len && t->s[i + 1] == '}') { buf_addc(&b, '}'); i++; continue; }
    if (c != '{') { buf_addc(&b, c); continue; }
    int j = i + 1; while (j < t->len && t->s[j] != '}') j++;
    if (j >= t->len) { free(b.p); throw_error("ValueError", "Single '{' encountered in format string"); }
    char field[128]; int fl = j - i - 1; if (fl > 127) fl = 127;
    memcpy(field, t->s + i + 1, fl); field[fl] = 0;
    char *spec = strchr(field, ':'); if (spec) *spec++ = 0;
    char *conv = strchr(field, '!'); int cv = 0; if (conv) { *conv++ = 0; cv = *conv; }
    Value v;
    if (!field[0]) { if (auto_i + 1 >= n) { free(b.p); throw_error("IndexError", "Replacement index %d out of range for positional args tuple", auto_i); } v = inc(a[1 + auto_i++]); }
    else if (isdigit((unsigned char)field[0])) { int ix = atoi(field); if (ix + 1 >= n) { free(b.p); throw_error("IndexError", "Replacement index %d out of range for positional args tuple", ix); } v = inc(a[1 + ix]); }
    else { Value *k = kwget(kw, field); if (!k) { free(b.p); throw_error("KeyError", "'%s'", field); } v = inc(*k); }
    if (cv == 'r') { Str *r = val_repr(v); decref(v); v = V_obj(r, T_STR); }
    Value f = format_value(v, spec);
    buf_add(&b, ((Str *)f.o)->s, ((Str *)f.o)->len); decref(f); decref(v);
    i = j;
  }
  return buf_to_str(&b);
}

/* =============================================================== list */
#define LSELF ((List *)a[0].o)
static Value l_append(ARGS) { UNUSED; chk("append", n, 2, 2); list_append(LSELF, a[1]); return V_none(); }
static Value l_extend(ARGS) {
  UNUSED; chk("extend", n, 2, 2);
  Value src = a[1].o == a[0].o ? list_from_iter(a[1]) : inc(a[1]);
  Iter it; iter_init(&it, src); decref(src); Value x;
  while (iter_next(&it, &x)) list_append_own(LSELF, x);
  iter_done(&it); return V_none();
}
static Value l_insert(ARGS) {
  UNUSED; chk("insert", n, 3, 3);
  List *l = LSELF; int64_t i = need_int(a[1], "index");
  if (i < 0) { i += l->len; if (i < 0) i = 0; }
  if (i > l->len) i = l->len;
  list_append_own(l, V_none());
  memmove(l->items + i + 1, l->items + i, (l->len - 1 - i) * sizeof(Value));
  l->items[i] = inc(a[2]); return V_none();
}
static Value l_pop(ARGS) {
  UNUSED; chk("pop", n, 1, 2);
  List *l = LSELF;
  if (l->len == 0) throw_error("IndexError", "pop from empty list");
  int64_t i = n == 2 ? need_int(a[1], "index") : l->len - 1;
  if (i < 0) i += l->len;
  if (i < 0 || i >= l->len) throw_error("IndexError", "pop index out of range");
  Value v = l->items[i];
  memmove(l->items + i, l->items + i + 1, (l->len - i - 1) * sizeof(Value)); l->len--;
  return v;
}
static Value l_remove(ARGS) {
  UNUSED; chk("remove", n, 2, 2); List *l = LSELF;
  for (int i = 0; i < l->len; i++) if (val_eq(l->items[i], a[1])) {
    Value old = l->items[i];
    memmove(l->items + i, l->items + i + 1, (l->len - i - 1) * sizeof(Value)); l->len--;
    decref(old); return V_none();
  }
  throw_error("ValueError", "list.remove(x): x not in list");
}
static Value l_index(ARGS) {
  UNUSED; chk("index", n, 2, 2); List *l = LSELF;
  for (int i = 0; i < l->len; i++) if (val_eq(l->items[i], a[1])) return V_int(i);
  throw_error("ValueError", "value is not in list");
}
static Value l_count(ARGS) {
  UNUSED; chk("count", n, 2, 2); List *l = LSELF; int c = 0;
  for (int i = 0; i < l->len; i++) if (val_eq(l->items[i], a[1])) c++;
  return V_int(c);
}
static Value l_sort(ARGS) {
  chk("sort", n, 1, 1);
  Value *k = kwget(kw, "key"), *r = kwget(kw, "reverse");
  Value sorted = sorted_list(a[0], k ? *k : V_none(), r ? val_truthy(*r) : 0);
  List *l = LSELF, *s = (List *)sorted.o;
  Value *old = l->items; int oldn = l->len;
  l->items = s->items; l->len = s->len; l->cap = s->cap;
  s->items = old; s->len = oldn; s->cap = oldn;
  decref(sorted); return V_none();
}
static Value l_reverse(ARGS) {
  UNUSED; List *l = LSELF;
  for (int i = 0, j = l->len - 1; i < j; i++, j--) { Value t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; }
  return V_none();
}
static Value l_copy(ARGS) { UNUSED; return list_from_iter(a[0]); }
static Value l_clear(ARGS) {
  UNUSED; List *l = LSELF;
  for (int i = 0; i < l->len; i++) decref(l->items[i]);
  l->len = 0; return V_none();
}

/* =============================================================== dict */
#define DSELF ((Dict *)a[0].o)
static Value d_keys(ARGS) {
  UNUSED; Dict *d = DSELF; List *l = list_new(d->live);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) list_append(l, d->e[i].key);
  return V_obj(l, T_LIST);
}
static Value d_values(ARGS) {
  UNUSED; Dict *d = DSELF; List *l = list_new(d->live);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) list_append(l, d->e[i].val);
  return V_obj(l, T_LIST);
}
static Value d_items(ARGS) {
  UNUSED; Dict *d = DSELF; List *l = list_new(d->live);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) {
    Value kv[2] = {d->e[i].key, d->e[i].val}; list_append_own(l, V_tuple(kv, 2));
  }
  return V_obj(l, T_LIST);
}
static Value d_get(ARGS) {
  UNUSED; chk("get", n, 2, 3);
  Value *v = dict_find(DSELF, a[1]);
  if (v) return inc(*v);
  return n == 3 ? inc(a[2]) : V_none();
}
static Value d_pop(ARGS) {
  UNUSED; chk("pop", n, 2, 3);
  Value *v = dict_find(DSELF, a[1]);
  if (v) { Value r = inc(*v); dict_del(DSELF, a[1]); return r; }
  if (n == 3) return inc(a[2]);
  throw_keyerror(a[1]);
}
static Value d_counter_update(Value *a, int n, Kw *kw, int sign);
static Value d_update(ARGS) {
  if (DSELF->kind == DK_COUNTER) return d_counter_update(a, n, kw, 1);
  chk("update", n, 1, 2);
  if (n == 2) {
    if (a[1].t == T_DICT) {
      Dict *s = (Dict *)a[1].o;
      for (int i = 0; i < s->n; i++) if (s->e[i].key.t != T_UNDEF) dict_set(DSELF, s->e[i].key, s->e[i].val);
    } else {
      Iter it; iter_init(&it, a[1]); Value x;
      while (iter_next(&it, &x)) {
        if ((x.t != T_LIST && x.t != T_TUPLE) || ((List *)x.o)->len != 2) { decref(x); iter_done(&it); throw_error("ValueError", "dictionary update sequence element has wrong length"); }
        dict_set(DSELF, ((List *)x.o)->items[0], ((List *)x.o)->items[1]); decref(x);
      }
      iter_done(&it);
    }
  }
  if (kw) for (int i = 0; i < kw->n; i++) dict_set_str(DSELF, kw->names[i], kw->vals[i]);
  return V_none();
}
static Value d_setdefault(ARGS) {
  UNUSED; chk("setdefault", n, 2, 3);
  Value *v = dict_find(DSELF, a[1]);
  if (v) return inc(*v);
  Value d = n == 3 ? a[2] : V_none();
  dict_set(DSELF, a[1], d); return inc(d);
}
static Value d_copy(ARGS) {
  UNUSED; Dict *d = DSELF; Dict *c = dict_new(); Value cv = V_obj(c, T_DICT);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) dict_set(c, d->e[i].key, d->e[i].val);
  return cv;
}
static Value d_clear(ARGS) {
  UNUSED; Dict *d = DSELF;
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) { Value k = d->e[i].key, v = d->e[i].val; d->e[i].key.t = T_UNDEF; d->e[i].val = V_none(); decref(k); decref(v); }
  d->live = 0; return V_none();
}


/* =============================================================== sets */
Value new_set(int kind) {
  Dict *d = dict_new(); d->kind = kind; d->h.type = T_SET;
  return V_obj(d, T_SET);
}
Value new_set_from_iter(Value seq, int kind) {
  Value sv = new_set(kind); Dict *d = (Dict *)sv.o;
  Iter it; iter_init(&it, seq); Value x;
  while (iter_next(&it, &x)) { dict_set(d, x, V_none()); decref(x); }
  iter_done(&it);
  return sv;
}
#define SETSELF ((Dict *)a[0].o)
static Dict *as_setdict(Value v, Value *tmp) {
  *tmp = V_undef();
  if (v.t == T_SET) return (Dict *)v.o;
  *tmp = new_set_from_iter(v, DK_SET);
  return (Dict *)tmp->o;
}
static void set_frozen_check(Value *a) {
  if (SETSELF->kind == DK_FROZEN) throw_error("AttributeError", "'frozenset' object has no attribute to modify");
}
static Value st_add(ARGS) { UNUSED; chk("add", n, 2, 2); set_frozen_check(a); dict_set(SETSELF, a[1], V_none()); return V_none(); }
static Value st_remove(ARGS) { UNUSED; chk("remove", n, 2, 2); set_frozen_check(a); if (!dict_del(SETSELF, a[1])) throw_keyerror(a[1]); return V_none(); }
static Value st_discard(ARGS) { UNUSED; chk("discard", n, 2, 2); set_frozen_check(a); dict_del(SETSELF, a[1]); return V_none(); }
static Value st_pop(ARGS) {
  UNUSED; Dict *d = SETSELF; set_frozen_check(a);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) { Value k = inc(d->e[i].key); dict_del(d, k); return k; }
  throw_error("KeyError", "pop from an empty set");
}
static Value st_clear(ARGS) {
  UNUSED; Dict *d = SETSELF; set_frozen_check(a);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) { Value k = d->e[i].key, v = d->e[i].val; d->e[i].key.t = T_UNDEF; d->e[i].val = V_none(); decref(k); decref(v); }
  d->live = 0; return V_none();
}
static Value st_copy(ARGS) { UNUSED; return new_set_from_iter(a[0], SETSELF->kind); }
static Value st_update(ARGS) {
  UNUSED; set_frozen_check(a);
  for (int i = 1; i < n; i++) { Value tmp; Dict *o = as_setdict(a[i], &tmp); for (int k = 0; k < o->n; k++) if (o->e[k].key.t != T_UNDEF) dict_set(SETSELF, o->e[k].key, V_none()); decref(tmp); }
  return V_none();
}
static Value st_union(ARGS) {
  UNUSED; Value r = new_set_from_iter(a[0], SETSELF->kind);
  for (int i = 1; i < n; i++) { Value tmp; Dict *o = as_setdict(a[i], &tmp); for (int k = 0; k < o->n; k++) if (o->e[k].key.t != T_UNDEF) dict_set((Dict *)r.o, o->e[k].key, V_none()); decref(tmp); }
  return r;
}
static Value st_intersection(ARGS) {
  UNUSED; Value r = new_set_from_iter(a[0], SETSELF->kind);
  for (int i = 1; i < n; i++) {
    Value tmp; Dict *o = as_setdict(a[i], &tmp); Dict *rd = (Dict *)r.o;
    for (int k = 0; k < rd->n; k++) if (rd->e[k].key.t != T_UNDEF && !dict_find(o, rd->e[k].key)) { Value key = inc(rd->e[k].key); dict_del(rd, key); decref(key); }
    decref(tmp);
  }
  return r;
}
static Value st_difference(ARGS) {
  UNUSED; Value r = new_set_from_iter(a[0], SETSELF->kind);
  for (int i = 1; i < n; i++) {
    Value tmp; Dict *o = as_setdict(a[i], &tmp);
    for (int k = 0; k < o->n; k++) if (o->e[k].key.t != T_UNDEF) dict_del((Dict *)r.o, o->e[k].key);
    decref(tmp);
  }
  return r;
}
static Value st_symdiff(ARGS) {
  UNUSED; chk("symmetric_difference", n, 2, 2);
  Value tmp; Dict *o = as_setdict(a[1], &tmp); Value r = new_set_from_iter(a[0], SETSELF->kind); Dict *rd = (Dict *)r.o;
  for (int k = 0; k < o->n; k++) if (o->e[k].key.t != T_UNDEF) { if (dict_find(SETSELF, o->e[k].key)) dict_del(rd, o->e[k].key); else dict_set(rd, o->e[k].key, V_none()); }
  decref(tmp); return r;
}
static Value st_issubset(ARGS) {
  UNUSED; chk("issubset", n, 2, 2); Value tmp; Dict *o = as_setdict(a[1], &tmp); Dict *d = SETSELF; int ok = 1;
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF && !dict_find(o, d->e[i].key)) { ok = 0; break; }
  decref(tmp); return V_bool(ok);
}
static Value st_issuperset(ARGS) {
  UNUSED; chk("issuperset", n, 2, 2); Value tmp; Dict *o = as_setdict(a[1], &tmp); Dict *d = SETSELF; int ok = 1;
  for (int i = 0; i < o->n; i++) if (o->e[i].key.t != T_UNDEF && !dict_find(d, o->e[i].key)) { ok = 0; break; }
  decref(tmp); return V_bool(ok);
}
static Value st_isdisjoint(ARGS) {
  UNUSED; chk("isdisjoint", n, 2, 2); Value tmp; Dict *o = as_setdict(a[1], &tmp); Dict *d = SETSELF; int ok = 1;
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF && dict_find(o, d->e[i].key)) { ok = 0; break; }
  decref(tmp); return V_bool(ok);
}
static Value st_intersection_update(ARGS) { Value r = st_intersection(a, n, kw); Value args[2] = {a[0], r}; Value c = st_clear(a, 1, NULL); decref(c); c = st_update(args, 2, NULL); decref(c); decref(r); return V_none(); }
static Value st_difference_update(ARGS) { Value r = st_difference(a, n, kw); Value args[2] = {a[0], r}; Value c = st_clear(a, 1, NULL); decref(c); c = st_update(args, 2, NULL); decref(c); decref(r); return V_none(); }

/* ------------------------------------------- dict extras / Counter */
static Value d_popitem(ARGS) {
  UNUSED; Dict *d = DSELF;
  int last = 1; Value *k = kwget(kw, "last"); if (k) last = val_truthy(*k); else if (n == 2) last = val_truthy(a[1]);
  if (d->kind != DK_ORDERED) last = 1;
  if (last) { for (int i = d->n - 1; i >= 0; i--) if (d->e[i].key.t != T_UNDEF) { Value kv[2] = {d->e[i].key, d->e[i].val}; Value t = V_tuple(kv, 2); Value key = inc(kv[0]); dict_del(d, key); decref(key); return t; } }
  else { for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) { Value kv[2] = {d->e[i].key, d->e[i].val}; Value t = V_tuple(kv, 2); Value key = inc(kv[0]); dict_del(d, key); decref(key); return t; } }
  throw_error("KeyError", "popitem(): dictionary is empty");
}
static Value d_move_to_end(ARGS) {
  UNUSED; chk("move_to_end", n, 2, 3); Dict *d = DSELF;
  int last = n == 3 ? val_truthy(a[2]) : 1; Value *k = kwget(kw, "last"); if (k) last = val_truthy(*k);
  Value *v = dict_find(d, a[1]); if (!v) throw_keyerror(a[1]);
  Value val = inc(*v), key = inc(a[1]);
  if (last) { dict_del(d, key); dict_set(d, key, val); }
  else {
    Value tmp = V_obj(dict_new(), T_DICT); Dict *td = (Dict *)tmp.o;
    dict_set(td, key, val);
    for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF && !val_eq(d->e[i].key, key)) dict_set(td, d->e[i].key, d->e[i].val);
    Value c = d_clear(a, 1, NULL); decref(c);
    for (int i = 0; i < td->n; i++) if (td->e[i].key.t != T_UNDEF) dict_set(d, td->e[i].key, td->e[i].val);
    decref(tmp);
  }
  decref(val); decref(key); return V_none();
}
static Value d_fromkeys(Value *a, int n, Kw *kw) {
  UNUSED;  /* called as dict.fromkeys(keys[, value]) -- no self */
  chk("fromkeys", n, 1, 2);
  Dict *d = dict_new(); Value dv = V_obj(d, T_DICT); Value val = n == 2 ? a[1] : V_none();
  Iter it; iter_init(&it, a[0]); Value x;
  while (iter_next(&it, &x)) { dict_set(d, x, val); decref(x); }
  iter_done(&it); return dv;
}
static Value d_fromkeys_m(ARGS) { return d_fromkeys(a + 1, n - 1, kw); }
static Value d_most_common(ARGS) {
  UNUSED; chk("most_common", n, 1, 2); Dict *d = DSELF;
  int cnt = 0; int *order = xmalloc((d->live + 1) * sizeof(int));
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF) order[cnt++] = i;
  for (int i = 1; i < cnt; i++) {   /* stable insertion sort, descending by count */
    int x = order[i], j = i - 1;
    while (j >= 0 && val_lt(d->e[order[j]].val, d->e[x].val)) { order[j + 1] = order[j]; j--; }
    order[j + 1] = x;
  }
  int lim = cnt; if (n == 2 && a[1].t != T_NONE) { int64_t q = need_int(a[1], "n"); if (q < lim) lim = (int)q; if (lim < 0) lim = 0; }
  List *l = list_new(lim);
  for (int i = 0; i < lim; i++) { Value kv[2] = {d->e[order[i]].key, d->e[order[i]].val}; list_append_own(l, V_tuple(kv, 2)); }
  free(order); return V_obj(l, T_LIST);
}
static Value d_elements(ARGS) {
  UNUSED; Dict *d = DSELF; List *l = list_new(0);
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF && d->e[i].val.t == T_INT) for (int64_t k = 0; k < d->e[i].val.i; k++) list_append(l, d->e[i].key);
  return V_obj(l, T_LIST);
}
static Value d_total(ARGS) {
  UNUSED; Dict *d = DSELF; int64_t t = 0;
  for (int i = 0; i < d->n; i++) if (d->e[i].key.t != T_UNDEF && d->e[i].val.t == T_INT) t += d->e[i].val.i;
  return V_int(t);
}
static void counter_add(Dict *d, Value key, int64_t delta) {
  Value *v = dict_find(d, key);
  int64_t cur = v && v->t == T_INT ? v->i : 0;
  dict_set(d, key, V_int(cur + delta));
}
static Value d_counter_update(Value *a, int n, Kw *kw, int sign) {
  Dict *d = DSELF;
  if (n >= 2) {
    if (a[1].t == T_DICT) { Dict *s = (Dict *)a[1].o; for (int i = 0; i < s->n; i++) if (s->e[i].key.t != T_UNDEF && s->e[i].val.t == T_INT) counter_add(d, s->e[i].key, sign * s->e[i].val.i); }
    else { Iter it; iter_init(&it, a[1]); Value x; while (iter_next(&it, &x)) { counter_add(d, x, sign); decref(x); } iter_done(&it); }
  }
  if (kw) for (int i = 0; i < kw->n; i++) if (kw->vals[i].t == T_INT) counter_add(d, V_obj(kw->names[i], T_STR), sign * kw->vals[i].i);
  return V_none();
}
static Value d_subtract(ARGS) { return d_counter_update(a, n, kw, -1); }

/* ============================================================== tuple */
static Value t_index(ARGS) { return l_index(a, n, kw); }
static Value t_count(ARGS) { return l_count(a, n, kw); }

/* =============================================================== file */
#define FSELF ((File *)a[0].o)
static FILE *fp_of(Value *a) {
  File *f = FSELF;
  if (f->closed || !f->fp) throw_error("ValueError", "I/O operation on closed file.");
  return f->fp;
}
static Value f_read(ARGS) {
  UNUSED; chk("read", n, 1, 2); FILE *fp = fp_of(a);
  int64_t lim = (n == 2 && a[1].t != T_NONE) ? need_int(a[1], "size") : -1;
  Buf b; buf_init(&b); buf_add(&b, "", 0); int c;
  while ((lim < 0 || b.len < lim) && (c = fgetc(fp)) != EOF) buf_addc(&b, (char)c);
  return buf_to_str(&b);
}
static Value f_readline(ARGS) {
  UNUSED; FILE *fp = fp_of(a);
  Buf b; buf_init(&b); buf_add(&b, "", 0); int c;
  while ((c = fgetc(fp)) != EOF) { buf_addc(&b, (char)c); if (c == '\n') break; }
  return buf_to_str(&b);
}
static Value f_readlines(ARGS) {
  UNUSED; fp_of(a);
  List *l = list_new(0); Iter it; iter_init(&it, a[0]); Value x;
  while (iter_next(&it, &x)) list_append_own(l, x);
  iter_done(&it); return V_obj(l, T_LIST);
}
static Value f_write(ARGS) {
  UNUSED; chk("write", n, 2, 2); FILE *fp = fp_of(a);
  Str *s = need_str(a[1], "write() argument");
  fwrite(s->s, 1, s->len, fp);
  return V_int(s->cplen);
}
static Value f_close(ARGS) {
  UNUSED; File *f = FSELF;
  if (!f->closed && f->fp && f->fp != stdin && f->fp != stdout && f->fp != stderr) fclose(f->fp);
  if (f->fp != stdin && f->fp != stdout && f->fp != stderr) f->closed = 1;
  return V_none();
}
static Value f_flush(ARGS) { UNUSED; fflush(fp_of(a)); return V_none(); }

/* ============================================================ modules */
static Value make_mod(const char *name) {
  Module *m = xmalloc(sizeof(Module));
  m->h.rc = 1; m->h.type = T_MODULE; m->name = INTERN(name); oinc(m->name); m->attrs = dict_new();
  return V_obj(m, T_MODULE);
}
static void mod_fn(Value mod, const char *name, BuiltinFn fn) {
  Value b = new_builtin(name, fn); dict_set_cstr(((Module *)mod.o)->attrs, name, b); decref(b);
}
static void mod_val(Value mod, const char *name, Value v) {
  dict_set_cstr(((Module *)mod.o)->attrs, name, v); decref(v);
}

#define MATH1(fname, cfn) static Value m_##fname(ARGS) { UNUSED; chk(#fname, n, 1, 1); return V_float(cfn(need_num(a[0], #fname "()"))); }
MATH1(sin, sin) MATH1(cos, cos) MATH1(tan, tan) MATH1(asin, asin) MATH1(acos, acos) MATH1(atan, atan)
MATH1(exp, exp) MATH1(fabs, fabs) MATH1(sinh, sinh) MATH1(cosh, cosh) MATH1(tanh, tanh)
static Value m_sqrt(ARGS) {
  UNUSED; chk("sqrt", n, 1, 1); double d = need_num(a[0], "sqrt()");
  if (d < 0) throw_error("ValueError", "math domain error");
  return V_float(sqrt(d));
}
static Value m_floor(ARGS) { UNUSED; chk("floor", n, 1, 1); if (a[0].t == T_INT || a[0].t == T_BOOL) return V_int(a[0].i); double d = floor(need_num(a[0], "floor()")); if (isnan(d) || isinf(d) || fabs(d) >= 9.2e18) throw_error("OverflowError", "cannot convert float to integer"); return V_int((int64_t)d); }
static Value m_ceil(ARGS) { UNUSED; chk("ceil", n, 1, 1); if (a[0].t == T_INT || a[0].t == T_BOOL) return V_int(a[0].i); double d = ceil(need_num(a[0], "ceil()")); if (isnan(d) || isinf(d) || fabs(d) >= 9.2e18) throw_error("OverflowError", "cannot convert float to integer"); return V_int((int64_t)d); }
static Value m_trunc(ARGS) { UNUSED; chk("trunc", n, 1, 1); if (a[0].t == T_INT || a[0].t == T_BOOL) return V_int(a[0].i); return V_int((int64_t)trunc(need_num(a[0], "trunc()"))); }
static Value m_log(ARGS) {
  UNUSED; chk("log", n, 1, 2); double d = need_num(a[0], "log()");
  if (d <= 0) throw_error("ValueError", "math domain error");
  if (n == 2) { double b = need_num(a[1], "log()"); if (b <= 0 || b == 1) throw_error("ValueError", "math domain error"); return V_float(log(d) / log(b)); }
  return V_float(log(d));
}
static Value m_log2(ARGS) { UNUSED; chk("log2", n, 1, 1); double d = need_num(a[0], "log2()"); if (d <= 0) throw_error("ValueError", "math domain error"); return V_float(log2(d)); }
static Value m_log10(ARGS) { UNUSED; chk("log10", n, 1, 1); double d = need_num(a[0], "log10()"); if (d <= 0) throw_error("ValueError", "math domain error"); return V_float(log10(d)); }
static Value m_pow(ARGS) { UNUSED; chk("pow", n, 2, 2); return V_float(pow(need_num(a[0], "pow()"), need_num(a[1], "pow()"))); }
static Value m_atan2(ARGS) { UNUSED; chk("atan2", n, 2, 2); return V_float(atan2(need_num(a[0], "atan2()"), need_num(a[1], "atan2()"))); }
static Value m_hypot(ARGS) { UNUSED; chk("hypot", n, 2, 2); return V_float(hypot(need_num(a[0], "hypot()"), need_num(a[1], "hypot()"))); }
static Value m_fmod(ARGS) { UNUSED; chk("fmod", n, 2, 2); return V_float(fmod(need_num(a[0], "fmod()"), need_num(a[1], "fmod()"))); }
static Value m_isnan(ARGS) { UNUSED; chk("isnan", n, 1, 1); return V_bool(isnan(need_num(a[0], "isnan()"))); }
static Value m_isinf(ARGS) { UNUSED; chk("isinf", n, 1, 1); return V_bool(isinf(need_num(a[0], "isinf()"))); }
static Value m_radians(ARGS) { UNUSED; chk("radians", n, 1, 1); return V_float(need_num(a[0], "radians()") * M_PI / 180.0); }
static Value m_degrees(ARGS) { UNUSED; chk("degrees", n, 1, 1); return V_float(need_num(a[0], "degrees()") * 180.0 / M_PI); }
static Value m_gcd(ARGS) {
  UNUSED; chk("gcd", n, 2, 2);
  int64_t x = need_int(a[0], "gcd()"), y = need_int(a[1], "gcd()");
  if (x < 0) x = -x; if (y < 0) y = -y;
  while (y) { int64_t t = x % y; x = y; y = t; }
  return V_int(x);
}
static Value m_factorial(ARGS) {
  UNUSED; chk("factorial", n, 1, 1); int64_t k = need_int(a[0], "factorial()");
  if (k < 0) throw_error("ValueError", "factorial() not defined for negative values");
  int64_t r = 1;
  for (int64_t i = 2; i <= k; i++) if (__builtin_mul_overflow(r, i, &r)) throw_error("OverflowError", "integer overflow");
  return V_int(r);
}
static Value m_isqrt(ARGS) {
  UNUSED; chk("isqrt", n, 1, 1); int64_t k = need_int(a[0], "isqrt()");
  if (k < 0) throw_error("ValueError", "isqrt() argument must be nonnegative");
  int64_t r = (int64_t)sqrt((double)k);
  while (r * r > k) r--; while ((r + 1) * (r + 1) <= k) r++;
  return V_int(r);
}

static double now_secs(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static double mono_secs(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static Value tm_time(ARGS) { UNUSED; return V_float(now_secs()); }
static Value tm_perf(ARGS) { UNUSED; return V_float(mono_secs()); }
static Value tm_sleep(ARGS) {
  UNUSED; chk("sleep", n, 1, 1); double s = need_num(a[0], "sleep()");
  if (s < 0) throw_error("ValueError", "sleep length must be non-negative");
  fflush(stdout);
  struct timespec ts; ts.tv_sec = (time_t)s; ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
  nanosleep(&ts, NULL); return V_none();
}

static uint64_t rng_state = 88172645463325252ULL;
static uint64_t rng_next(void) {
  rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
  return rng_state * 2685821657736338717ULL;
}
static double rng_double(void) { return (double)(rng_next() >> 11) / 9007199254740992.0; }
static Value r_random(ARGS) { UNUSED; return V_float(rng_double()); }
static Value r_seed(ARGS) {
  UNUSED; chk("seed", n, 0, 1);
  uint64_t s = n && a[0].t != T_NONE ? (uint64_t)need_int(a[0], "seed()") : (uint64_t)now_secs();
  rng_state = s * 6364136223846793005ULL + 1442695040888963407ULL; if (!rng_state) rng_state = 1;
  for (int i = 0; i < 4; i++) rng_next();
  return V_none();
}
static Value r_randint(ARGS) {
  UNUSED; chk("randint", n, 2, 2);
  int64_t lo = need_int(a[0], "randint()"), hi = need_int(a[1], "randint()");
  if (hi < lo) throw_error("ValueError", "empty range for randint()");
  return V_int(lo + (int64_t)(rng_next() % (uint64_t)(hi - lo + 1)));
}
static Value r_randrange(ARGS) {
  UNUSED; chk("randrange", n, 1, 2);
  int64_t lo = 0, hi;
  if (n == 1) hi = need_int(a[0], "randrange()"); else { lo = need_int(a[0], "randrange()"); hi = need_int(a[1], "randrange()"); }
  if (hi <= lo) throw_error("ValueError", "empty range for randrange()");
  return V_int(lo + (int64_t)(rng_next() % (uint64_t)(hi - lo)));
}
static Value r_uniform(ARGS) { UNUSED; chk("uniform", n, 2, 2); double lo = need_num(a[0], "uniform()"), hi = need_num(a[1], "uniform()"); return V_float(lo + (hi - lo) * rng_double()); }
static Value r_choice(ARGS) {
  UNUSED; chk("choice", n, 1, 1);
  Value l = list_from_iter(a[0]); List *ll = (List *)l.o;
  if (ll->len == 0) { decref(l); throw_error("IndexError", "Cannot choose from an empty sequence"); }
  Value r = inc(ll->items[rng_next() % (uint64_t)ll->len]); decref(l); return r;
}
static Value r_shuffle(ARGS) {
  UNUSED; chk("shuffle", n, 1, 1);
  if (a[0].t != T_LIST) throw_error("TypeError", "shuffle() requires a list");
  List *l = (List *)a[0].o;
  for (int i = l->len - 1; i > 0; i--) { int j = (int)(rng_next() % (uint64_t)(i + 1)); Value t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; }
  return V_none();
}


/* ------------------------------------------------------------ _time */
static Value tm_time_ns(ARGS) { UNUSED; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return V_int((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec); }
static Value tm_mono_ns(ARGS) { UNUSED; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return V_int((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec); }
static Value tm_tuple(const struct tm *t) {
  Value it[9] = {V_int(t->tm_year + 1900), V_int(t->tm_mon + 1), V_int(t->tm_mday), V_int(t->tm_hour), V_int(t->tm_min),
                 V_int(t->tm_sec), V_int((t->tm_wday + 6) % 7), V_int(t->tm_yday + 1), V_int(t->tm_isdst > 0 ? 1 : t->tm_isdst == 0 ? 0 : -1)};
  return V_tuple(it, 9);
}
static Value tm_localtime(ARGS) {
  UNUSED; chk("_localtime", n, 1, 1);
  time_t t = (time_t)need_num(a[0], "secs"); struct tm tmv;
  if (!localtime_r(&t, &tmv)) throw_error("OverflowError", "timestamp out of range for platform time_t");
  return tm_tuple(&tmv);
}
static Value tm_gmtime(ARGS) {
  UNUSED; chk("_gmtime", n, 1, 1);
  time_t t = (time_t)need_num(a[0], "secs"); struct tm tmv;
  if (!gmtime_r(&t, &tmv)) throw_error("OverflowError", "timestamp out of range for platform time_t");
  return tm_tuple(&tmv);
}
static void tuple_to_tm(Value tv, struct tm *t) {
  if (tv.t != T_TUPLE || ((List *)tv.o)->len < 6) throw_error("TypeError", "time tuple must have at least 6 items");
  List *l = (List *)tv.o; memset(t, 0, sizeof *t);
  t->tm_year = (int)need_int(l->items[0], "year") - 1900; t->tm_mon = (int)need_int(l->items[1], "month") - 1; t->tm_mday = (int)need_int(l->items[2], "day");
  t->tm_hour = (int)need_int(l->items[3], "hour"); t->tm_min = (int)need_int(l->items[4], "minute"); t->tm_sec = (int)need_int(l->items[5], "second");
  t->tm_wday = l->len > 6 ? ((int)need_int(l->items[6], "wday") + 1) % 7 : 0;
  t->tm_yday = l->len > 7 ? (int)need_int(l->items[7], "yday") - 1 : 0;
  t->tm_isdst = l->len > 8 ? (int)need_int(l->items[8], "isdst") : -1;
}
static Value tm_mktime(ARGS) {
  UNUSED; chk("_mktime", n, 1, 1); struct tm t; tuple_to_tm(a[0], &t); t.tm_isdst = -1;
  time_t r = mktime(&t);
  if (r == (time_t)-1) throw_error("OverflowError", "mktime argument out of range");
  return V_float((double)r);
}
static Value tm_strftime(ARGS) {
  UNUSED; chk("_strftime", n, 2, 2);
  Str *fmt = need_str(a[0], "format"); struct tm t; tuple_to_tm(a[1], &t);
  if (fmt->len == 0) return V_str("");
  size_t cap = (size_t)fmt->len * 8 + 128;
  char *buf = xmalloc(cap); size_t r = strftime(buf, cap, fmt->s, &t);
  Value v = V_strn(buf, (int)r); free(buf); return v;
}
static Value tm_strptime(ARGS) {
  UNUSED; chk("_strptime", n, 2, 2);
  Str *s = need_str(a[0], "string"), *fmt = need_str(a[1], "format"); struct tm t; memset(&t, 0, sizeof t); t.tm_mday = 1; t.tm_isdst = -1;
  const char *end = strptime(s->s, fmt->s, &t);
  if (!end || *end) throw_error("ValueError", "time data '%s' does not match format '%s'", s->s, fmt->s);
  time_t tmp = timegm(&(struct tm){.tm_year = t.tm_year, .tm_mon = t.tm_mon, .tm_mday = t.tm_mday});
  struct tm norm; gmtime_r(&tmp, &norm); t.tm_wday = norm.tm_wday; t.tm_yday = norm.tm_yday;
  return tm_tuple(&t);
}
static Value tm_tzoffset(ARGS) {
  UNUSED; time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
  return V_int(-(int64_t)t.tm_gmtoff);
}

/* --------------------------------------------------------------- os */
static Value os_chdir(ARGS) { UNUSED; chk("chdir", n, 1, 1); Str *p = need_str(a[0], "path"); if (chdir(p->s) != 0) { int e = errno; throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); } return V_none(); }
static Value os_listdir(ARGS) {
  UNUSED; chk("listdir", n, 0, 1); const char *p = n ? need_str(a[0], "path")->s : ".";
  DIR *d = opendir(p);
  if (!d) { int e = errno; throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), p); }
  List *l = list_new(0); struct dirent *de;
  while ((de = readdir(d))) { if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue; list_append_own(l, V_str(de->d_name)); }
  closedir(d); return V_obj(l, T_LIST);
}
static Value os_mkdir(ARGS) {
  UNUSED; chk("mkdir", n, 1, 2); Str *p = need_str(a[0], "path");
  if (mkdir(p->s, 0777) != 0) { int e = errno; throw_error(e == EEXIST ? "FileExistsError" : e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); }
  return V_none();
}
static Value os_makedirs(ARGS) {
  chk("makedirs", n, 1, 3); Str *p = need_str(a[0], "path"); int exist_ok = 0; Value *k = kwget(kw, "exist_ok");
  if (k) exist_ok = val_truthy(*k); else if (n == 3) exist_ok = val_truthy(a[2]);
  char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", p->s);
  for (char *q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0777); *q = '/'; }
  if (mkdir(tmp, 0777) != 0) { int e = errno; if (!(e == EEXIST && exist_ok)) throw_error(e == EEXIST ? "FileExistsError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); }
  return V_none();
}
static Value os_rmdir(ARGS) { UNUSED; chk("rmdir", n, 1, 1); Str *p = need_str(a[0], "path"); if (rmdir(p->s) != 0) { int e = errno; throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); } return V_none(); }
static Value os_rename(ARGS) { UNUSED; chk("rename", n, 2, 2); Str *p = need_str(a[0], "src"), *q = need_str(a[1], "dst"); if (rename(p->s, q->s) != 0) { int e = errno; throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s' -> '%s'", e, strerror(e), p->s, q->s); } return V_none(); }
static Value os_system(ARGS) {
  UNUSED; chk("system", n, 1, 1); fflush(stdout); fflush(stderr);
  int r = system(need_str(a[0], "command")->s);
  if (r == -1) return V_int(-1);
  return V_int(WIFEXITED(r) ? WEXITSTATUS(r) : WIFSIGNALED(r) ? 128 + WTERMSIG(r) : r);
}
static Value os_getpid(ARGS) { UNUSED; return V_int((int64_t)getpid()); }
static Value os_cpu_count(ARGS) { UNUSED; long c = sysconf(_SC_NPROCESSORS_ONLN); return c > 0 ? V_int(c) : V_none(); }
static Value os_isatty_fd(ARGS) { UNUSED; chk("isatty", n, 1, 1); return V_bool(isatty((int)need_int(a[0], "fd"))); }

static Value path_join(ARGS) {
  UNUSED; Buf b; buf_init(&b); buf_add(&b, "", 0);
  for (int i = 0; i < n; i++) {
    Str *p = need_str(a[i], "path");
    if (p->len && p->s[0] == '/') { b.len = 0; b.p[0] = 0; }
    else if (b.len && b.p[b.len - 1] != '/') buf_addc(&b, '/');
    buf_add(&b, p->s, p->len);
  }
  return buf_to_str(&b);
}
static Value path_exists(ARGS) { UNUSED; chk("exists", n, 1, 1); struct stat st; return V_bool(stat(need_str(a[0], "path")->s, &st) == 0); }
static Value path_isfile(ARGS) { UNUSED; chk("isfile", n, 1, 1); struct stat st; return V_bool(stat(need_str(a[0], "path")->s, &st) == 0 && S_ISREG(st.st_mode)); }
static Value path_isdir(ARGS) { UNUSED; chk("isdir", n, 1, 1); struct stat st; return V_bool(stat(need_str(a[0], "path")->s, &st) == 0 && S_ISDIR(st.st_mode)); }
static Value path_basename(ARGS) { UNUSED; chk("basename", n, 1, 1); Str *p = need_str(a[0], "path"); const char *sl = strrchr(p->s, '/'); return V_str(sl ? sl + 1 : p->s); }
static Value path_dirname(ARGS) {
  UNUSED; chk("dirname", n, 1, 1); Str *p = need_str(a[0], "path"); const char *sl = strrchr(p->s, '/');
  if (!sl) return V_str("");
  const char *e = sl; while (e > p->s && e[-1] == '/') e--;
  if (e == p->s) return V_str("/");
  return V_strn(p->s, (int)(e - p->s));
}
static Value path_split(ARGS) {
  UNUSED; Value d = path_dirname(a, n, NULL), b = path_basename(a, n, NULL); Value it[2] = {d, b}; Value t = V_tuple(it, 2); decref(d); decref(b); return t;
}
static Value path_splitext(ARGS) {
  UNUSED; chk("splitext", n, 1, 1); Str *p = need_str(a[0], "path");
  const char *sl = strrchr(p->s, '/'); const char *base = sl ? sl + 1 : p->s; const char *dot = strrchr(base, '.');
  Value it[2];
  if (!dot || dot == base) { it[0] = inc(a[0]); it[1] = V_str(""); }
  else { it[0] = V_strn(p->s, (int)(dot - p->s)); it[1] = V_str(dot); }
  Value t = V_tuple(it, 2); decref(it[0]); decref(it[1]); return t;
}
static Value path_normpath(ARGS) {
  UNUSED; chk("normpath", n, 1, 1); Str *p = need_str(a[0], "path");
  int abs_ = p->len && p->s[0] == '/'; char *parts[256]; int np = 0; char *tmp = xstrdup(p->s);
  for (char *tok = strtok(tmp, "/"); tok && np < 255; tok = strtok(NULL, "/")) {
    if (!strcmp(tok, ".") || !*tok) continue;
    if (!strcmp(tok, "..")) { if (np && strcmp(parts[np - 1], "..")) np--; else if (!abs_) parts[np++] = tok; continue; }
    parts[np++] = tok;
  }
  Buf b; buf_init(&b); if (abs_) buf_addc(&b, '/');
  for (int i = 0; i < np; i++) { if (i) buf_addc(&b, '/'); buf_adds(&b, parts[i]); }
  if (!b.len) buf_addc(&b, '.');
  free(tmp); return buf_to_str(&b);
}
static Value path_abspath(ARGS) {
  UNUSED; chk("abspath", n, 1, 1); Str *p = need_str(a[0], "path");
  Value full;
  if (p->len && p->s[0] == '/') full = inc(a[0]);
  else { char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "."); Value args[2] = {V_str(cwd), a[0]}; full = path_join(args, 2, NULL); decref(args[0]); }
  Value r = path_normpath(&full, 1, NULL); decref(full); return r;
}
static Value path_realpath(ARGS) {
  UNUSED; chk("realpath", n, 1, 1); char buf[PATH_MAX];
  if (realpath(need_str(a[0], "path")->s, buf)) return V_str(buf);
  return path_abspath(a, n, kw);
}
static Value path_expanduser(ARGS) {
  UNUSED; chk("expanduser", n, 1, 1); Str *p = need_str(a[0], "path");
  if (p->len && p->s[0] == '~' && (p->len == 1 || p->s[1] == '/')) { const char *h = getenv("HOME"); if (h) { Buf b; buf_init(&b); buf_adds(&b, h); buf_add(&b, p->s + 1, p->len - 1); return buf_to_str(&b); } }
  return inc(a[0]);
}
static Value path_isabs(ARGS) { UNUSED; chk("isabs", n, 1, 1); Str *p = need_str(a[0], "path"); return V_bool(p->len && p->s[0] == '/'); }
static Value path_getsize(ARGS) { UNUSED; chk("getsize", n, 1, 1); struct stat st; Str *p = need_str(a[0], "path"); if (stat(p->s, &st) != 0) { int e = errno; throw_error("FileNotFoundError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); } return V_int((int64_t)st.st_size); }
static Value path_getmtime(ARGS) { UNUSED; chk("getmtime", n, 1, 1); struct stat st; Str *p = need_str(a[0], "path"); if (stat(p->s, &st) != 0) { int e = errno; throw_error("FileNotFoundError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); } return V_float((double)st.st_mtime); }

/* random extras */
static Value r_sample(ARGS) {
  UNUSED; chk("sample", n, 2, 2);
  Value pool = list_from_iter(a[0]); List *l = (List *)pool.o; int64_t k = need_int(a[1], "k");
  if (k < 0 || k > l->len) { decref(pool); throw_error("ValueError", "Sample larger than population or is negative"); }
  for (int64_t i = 0; i < k; i++) { int j = (int)(i + rng_next() % (uint64_t)(l->len - i)); Value t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; }
  List *out = list_new((int)k); for (int64_t i = 0; i < k; i++) list_append(out, l->items[i]);
  decref(pool); return V_obj(out, T_LIST);
}
static Value r_choices(ARGS) {
  chk("choices", n, 1, 2); Value pool = list_from_iter(a[0]); List *l = (List *)pool.o;
  int64_t k = 1; Value *kk = kwget(kw, "k"); if (kk) k = need_int(*kk, "k"); else if (n == 2) k = need_int(a[1], "k");
  Value *w = kwget(kw, "weights");
  if (l->len == 0) { decref(pool); throw_error("IndexError", "Cannot choose from an empty sequence"); }
  double *cum = NULL; double total = 0;
  if (w && w->t != T_NONE) {
    Value wl = list_from_iter(*w); List *wv = (List *)wl.o;
    if (wv->len != l->len) { decref(pool); decref(wl); throw_error("ValueError", "The number of weights does not match the population"); }
    cum = xmalloc(l->len * sizeof(double));
    for (int i = 0; i < l->len; i++) { total += need_num(wv->items[i], "weight"); cum[i] = total; }
    decref(wl);
  }
  List *out = list_new((int)k);
  for (int64_t q = 0; q < k; q++) {
    int idx;
    if (cum) { double r = rng_double() * total; idx = 0; while (idx < l->len - 1 && cum[idx] <= r) idx++; }
    else idx = (int)(rng_next() % (uint64_t)l->len);
    list_append(out, l->items[idx]);
  }
  free(cum); decref(pool); return V_obj(out, T_LIST);
}
static Value r_gauss(ARGS) {
  UNUSED; chk("gauss", n, 0, 2); double mu = n > 0 ? need_num(a[0], "mu") : 0, sigma = n > 1 ? need_num(a[1], "sigma") : 1;
  double u1 = rng_double(), u2 = rng_double(); if (u1 < 1e-300) u1 = 1e-300;
  return V_float(mu + sigma * sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2));
}
static Value r_getrandbits(ARGS) { UNUSED; chk("getrandbits", n, 1, 1); int64_t k = need_int(a[0], "k"); if (k <= 0 || k > 62) throw_error("ValueError", "number of bits must be in 1..62"); return V_int((int64_t)(rng_next() >> (64 - k))); }

static Value g_argv = {0};
void set_sys_argv(int argc, char **argv) {
  List *l = list_new(argc);
  for (int i = 0; i < argc; i++) list_append_own(l, V_str(argv[i]));
  g_argv = V_obj(l, T_LIST);
}
static Value sys_exit(ARGS) {
  UNUSED; chk("exit", n, 0, 1); fflush(stdout);
  int code = 0;
  if (n == 1) { if (a[0].t == T_INT) code = (int)a[0].i; else if (a[0].t != T_NONE) { Str *s = val_str(a[0]); fprintf(stderr, "%s\n", s->s); odec(s); code = 1; } }
  exit(code);
}
int g_max_depth = MAX_DEPTH;
static Value sys_getrec(ARGS) { UNUSED; return V_int(g_max_depth); }
static Value sys_setrec(ARGS) { UNUSED; chk("setrecursionlimit", n, 1, 1); int64_t v = need_int(a[0], "limit"); if (v < 50) v = 50; if (v > 60000) v = 60000; g_max_depth = (int)v; return V_none(); }
static Value os_getenv(ARGS) {
  UNUSED; chk("getenv", n, 1, 2);
  const char *v = getenv(need_str(a[0], "getenv()")->s);
  if (v) return V_str(v);
  return n == 2 ? inc(a[1]) : V_none();
}
static Value os_getcwd(ARGS) { UNUSED; char b[4096]; if (!getcwd(b, sizeof b)) throw_error("OSError", "getcwd failed"); return V_str(b); }
static Value os_remove(ARGS) {
  UNUSED; chk("remove", n, 1, 1); Str *p = need_str(a[0], "remove()");
  if (remove(p->s) != 0) { int e = errno; throw_error(e == ENOENT ? "FileNotFoundError" : "OSError", "[Errno %d] %s: '%s'", e, strerror(e), p->s); }
  return V_none();
}

Value builtin_module(const char *name) {
  if (!strcmp(name, "math")) {
    Value m = make_mod("math");
    mod_fn(m, "sqrt", m_sqrt); mod_fn(m, "sin", m_sin); mod_fn(m, "cos", m_cos); mod_fn(m, "tan", m_tan);
    mod_fn(m, "asin", m_asin); mod_fn(m, "acos", m_acos); mod_fn(m, "atan", m_atan); mod_fn(m, "atan2", m_atan2);
    mod_fn(m, "exp", m_exp); mod_fn(m, "log", m_log); mod_fn(m, "log2", m_log2); mod_fn(m, "log10", m_log10);
    mod_fn(m, "pow", m_pow); mod_fn(m, "fabs", m_fabs); mod_fn(m, "floor", m_floor); mod_fn(m, "ceil", m_ceil);
    mod_fn(m, "trunc", m_trunc); mod_fn(m, "hypot", m_hypot); mod_fn(m, "fmod", m_fmod);
    mod_fn(m, "isnan", m_isnan); mod_fn(m, "isinf", m_isinf); mod_fn(m, "radians", m_radians); mod_fn(m, "degrees", m_degrees);
    mod_fn(m, "gcd", m_gcd); mod_fn(m, "factorial", m_factorial); mod_fn(m, "isqrt", m_isqrt);
    mod_fn(m, "sinh", m_sinh); mod_fn(m, "cosh", m_cosh); mod_fn(m, "tanh", m_tanh);
    mod_val(m, "pi", V_float(M_PI)); mod_val(m, "e", V_float(M_E));
    mod_val(m, "inf", V_float(INFINITY)); mod_val(m, "nan", V_float(NAN)); mod_val(m, "tau", V_float(2 * M_PI));
    return m;
  }
  if (!strcmp(name, "_time")) {
    Value md = make_mod("_time");
    mod_fn(md, "time", tm_time); mod_fn(md, "time_ns", tm_time_ns); mod_fn(md, "sleep", tm_sleep);
    mod_fn(md, "perf_counter", tm_perf); mod_fn(md, "monotonic", tm_perf); mod_fn(md, "monotonic_ns", tm_mono_ns); mod_fn(md, "perf_counter_ns", tm_mono_ns);
    mod_fn(md, "_localtime", tm_localtime); mod_fn(md, "_gmtime", tm_gmtime); mod_fn(md, "_mktime", tm_mktime);
    mod_fn(md, "_strftime", tm_strftime); mod_fn(md, "_strptime", tm_strptime); mod_fn(md, "_tzoffset", tm_tzoffset);
    return md;
  }
  if (!strcmp(name, "random")) {
    Value m = make_mod("random");
    mod_fn(m, "random", r_random); mod_fn(m, "seed", r_seed); mod_fn(m, "randint", r_randint);
    mod_fn(m, "randrange", r_randrange); mod_fn(m, "uniform", r_uniform); mod_fn(m, "choice", r_choice);
    mod_fn(m, "shuffle", r_shuffle); mod_fn(m, "sample", r_sample); mod_fn(m, "choices", r_choices);
    mod_fn(m, "gauss", r_gauss); mod_fn(m, "normalvariate", r_gauss); mod_fn(m, "getrandbits", r_getrandbits);
    return m;
  }
  if (!strcmp(name, "sys")) {
    Value m = make_mod("sys");
    mod_fn(m, "exit", sys_exit);
    if (g_argv.t == T_LIST) mod_val(m, "argv", inc(g_argv)); else mod_val(m, "argv", V_list_new());
    mod_val(m, "stdout", make_file(stdout)); mod_val(m, "stderr", make_file(stderr)); mod_val(m, "stdin", make_file(stdin));
    mod_val(m, "version", V_str("cpy " CPY_VERSION)); mod_val(m, "maxsize", V_int(INT64_MAX));
    mod_val(m, "platform", V_str("linux")); mod_val(m, "path", inc(get_sys_path())); mod_val(m, "modules", inc(V_obj(g_modules, T_DICT)));
    { Value vi[5] = {V_int(3), V_int(12), V_int(0), V_str("final"), V_int(0)}; Value t = V_tuple(vi, 5); decref(vi[3]); mod_val(m, "version_info", t); }
    mod_val(m, "executable", V_str("cpy")); mod_val(m, "byteorder", V_str("little"));
    mod_fn(m, "getrecursionlimit", sys_getrec); mod_fn(m, "setrecursionlimit", sys_setrec);
    return m;
  }
  if (!strcmp(name, "_itertools")) return iter_module();
  if (!strcmp(name, "os")) {
    Value md = make_mod("os");
    mod_fn(md, "getenv", os_getenv); mod_fn(md, "getcwd", os_getcwd); mod_fn(md, "remove", os_remove); mod_fn(md, "unlink", os_remove);
    mod_fn(md, "chdir", os_chdir); mod_fn(md, "listdir", os_listdir); mod_fn(md, "mkdir", os_mkdir); mod_fn(md, "makedirs", os_makedirs);
    mod_fn(md, "rmdir", os_rmdir); mod_fn(md, "rename", os_rename); mod_fn(md, "system", os_system); mod_fn(md, "getpid", os_getpid);
    mod_fn(md, "cpu_count", os_cpu_count); mod_fn(md, "isatty", os_isatty_fd);
    mod_val(md, "name", V_str("posix")); mod_val(md, "sep", V_str("/")); mod_val(md, "linesep", V_str("\n")); mod_val(md, "pathsep", V_str(":"));
    mod_val(md, "curdir", V_str(".")); mod_val(md, "devnull", V_str("/dev/null"));
    { Value env = V_obj(dict_new(), T_DICT); extern char **environ;
      for (char **e = environ; e && *e; e++) { char *eq = strchr(*e, '='); if (!eq) continue; Value k = V_strn(*e, (int)(eq - *e)), v = V_str(eq + 1); dict_set((Dict *)env.o, k, v); decref(k); decref(v); }
      mod_val(md, "environ", env); }
    Value pm = make_mod("os.path");
    mod_fn(pm, "join", path_join); mod_fn(pm, "exists", path_exists); mod_fn(pm, "isfile", path_isfile); mod_fn(pm, "isdir", path_isdir);
    mod_fn(pm, "basename", path_basename); mod_fn(pm, "dirname", path_dirname); mod_fn(pm, "split", path_split); mod_fn(pm, "splitext", path_splitext);
    mod_fn(pm, "abspath", path_abspath); mod_fn(pm, "realpath", path_realpath); mod_fn(pm, "normpath", path_normpath);
    mod_fn(pm, "expanduser", path_expanduser); mod_fn(pm, "isabs", path_isabs); mod_fn(pm, "getsize", path_getsize); mod_fn(pm, "getmtime", path_getmtime);
    mod_val(pm, "sep", V_str("/"));
    mod_val(md, "path", pm);
    return md;
  }
  return V_undef();
}

void methods_init(void) {
  str_methods = dict_new(); list_methods = dict_new(); tuple_methods = dict_new();
  dict_methods = dict_new(); file_methods = dict_new();
  rng_state ^= (uint64_t)now_secs() * 0x9E3779B97F4A7C15ULL; if (!rng_state) rng_state = 1;
  Dict *s = str_methods;
  regm(s, "upper", s_upper); regm(s, "lower", s_lower); regm(s, "swapcase", s_swapcase);
  regm(s, "capitalize", s_capitalize); regm(s, "title", s_title);
  regm(s, "strip", s_strip); regm(s, "lstrip", s_lstrip); regm(s, "rstrip", s_rstrip);
  regm(s, "split", s_split); regm(s, "join", s_join); regm(s, "replace", s_replace);
  regm(s, "find", s_find); regm(s, "rfind", s_rfind); regm(s, "index", s_index); regm(s, "rindex", s_rindex);
  regm(s, "count", s_count); regm(s, "startswith", s_startswith); regm(s, "endswith", s_endswith);
  regm(s, "isdigit", s_isdigit); regm(s, "isalpha", s_isalpha); regm(s, "isalnum", s_isalnum);
  regm(s, "isspace", s_isspace); regm(s, "isupper", s_isupper); regm(s, "islower", s_islower);
  regm(s, "ljust", s_ljust); regm(s, "rjust", s_rjust); regm(s, "center", s_center); regm(s, "zfill", s_zfill);
  regm(s, "splitlines", s_splitlines); regm(s, "partition", s_partition); regm(s, "format", s_format);
  Dict *l = list_methods;
  regm(l, "append", l_append); regm(l, "extend", l_extend); regm(l, "insert", l_insert); regm(l, "pop", l_pop);
  regm(l, "remove", l_remove); regm(l, "index", l_index); regm(l, "count", l_count); regm(l, "sort", l_sort);
  regm(l, "reverse", l_reverse); regm(l, "copy", l_copy); regm(l, "clear", l_clear);
  regm(tuple_methods, "index", t_index); regm(tuple_methods, "count", t_count);
  Dict *d = dict_methods;
  regm(d, "keys", d_keys); regm(d, "values", d_values); regm(d, "items", d_items); regm(d, "get", d_get);
  regm(d, "pop", d_pop); regm(d, "update", d_update); regm(d, "setdefault", d_setdefault);
  regm(d, "copy", d_copy); regm(d, "clear", d_clear);
  regm(d, "popitem", d_popitem); regm(d, "move_to_end", d_move_to_end); regm(d, "fromkeys", d_fromkeys_m);
  regm(d, "most_common", d_most_common); regm(d, "elements", d_elements); regm(d, "total", d_total); regm(d, "subtract", d_subtract);
  set_methods = dict_new(); Dict *st = set_methods;
  regm(st, "add", st_add); regm(st, "remove", st_remove); regm(st, "discard", st_discard); regm(st, "pop", st_pop);
  regm(st, "clear", st_clear); regm(st, "copy", st_copy); regm(st, "update", st_update); regm(st, "union", st_union);
  regm(st, "intersection", st_intersection); regm(st, "difference", st_difference); regm(st, "symmetric_difference", st_symdiff);
  regm(st, "issubset", st_issubset); regm(st, "issuperset", st_issuperset); regm(st, "isdisjoint", st_isdisjoint);
  regm(st, "intersection_update", st_intersection_update); regm(st, "difference_update", st_difference_update);
  Dict *f = file_methods;
  regm(f, "read", f_read); regm(f, "readline", f_readline); regm(f, "readlines", f_readlines);
  regm(f, "write", f_write); regm(f, "close", f_close); regm(f, "flush", f_flush);
}

/* attribute access on builtin type objects: str.lower, dict.fromkeys, ... */
Value builtin_type_attr(const char *tname, Str *name) {
  Dict *t = NULL;
  if (!strcmp(tname, "str")) t = str_methods;
  else if (!strcmp(tname, "list")) t = list_methods;
  else if (!strcmp(tname, "tuple")) t = tuple_methods;
  else if (!strcmp(tname, "dict") || !strcmp(tname, "OrderedDict") || !strcmp(tname, "defaultdict") || !strcmp(tname, "Counter")) {
    { static Value fk; if (name->len == 8 && !strcmp(name->s, "fromkeys")) { if (fk.t != T_BUILTIN) fk = new_builtin("fromkeys", d_fromkeys); return inc(fk); } }
    t = dict_methods;
  }
  else if (!strcmp(tname, "set") || !strcmp(tname, "frozenset")) t = set_methods;
  if (!t) return V_undef();
  Value *f = dict_find_str(t, name);
  return f ? inc(*f) : V_undef();
}
