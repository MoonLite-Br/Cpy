/* lexer.c - turns source text into tokens (with INDENT/DEDENT) */
#include <errno.h>
#include "cpy.h"

typedef struct { Token *t; int n, cap; } TokVec;

static void tv_push(TokVec *tv, Token t) {
  if (tv->n >= tv->cap) { tv->cap = tv->cap ? tv->cap * 2 : 256; tv->t = xrealloc(tv->t, tv->cap * sizeof(Token)); }
  tv->t[tv->n++] = t;
}
static Token mk(TokKind k, int line) {
  Token t; memset(&t, 0, sizeof t); t.k = k; t.line = line; return t;
}

static void utf8_put(Buf *b, unsigned cp) {
  if (cp < 0x80) buf_addc(b, (char)cp);
  else if (cp < 0x800) { buf_addc(b, (char)(0xC0 | (cp >> 6))); buf_addc(b, (char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { buf_addc(b, (char)(0xE0 | (cp >> 12))); buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F))); buf_addc(b, (char)(0x80 | (cp & 0x3F))); }
  else { buf_addc(b, (char)(0xF0 | (cp >> 18))); buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F))); buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F))); buf_addc(b, (char)(0x80 | (cp & 0x3F))); }
}
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char *decode_escapes(const char *s, int len, int *outlen) {
  Buf b; buf_init(&b);
  for (int i = 0; i < len; i++) {
    if (s[i] != '\\' || i + 1 >= len) { buf_addc(&b, s[i]); continue; }
    char c = s[++i];
    switch (c) {
    case 'n': buf_addc(&b, '\n'); break;
    case 't': buf_addc(&b, '\t'); break;
    case 'r': buf_addc(&b, '\r'); break;
    case '0': buf_addc(&b, '\0'); break;
    case 'a': buf_addc(&b, '\a'); break;
    case 'b': buf_addc(&b, '\b'); break;
    case 'f': buf_addc(&b, '\f'); break;
    case 'v': buf_addc(&b, '\v'); break;
    case '\\': buf_addc(&b, '\\'); break;
    case '\'': buf_addc(&b, '\''); break;
    case '"': buf_addc(&b, '"'); break;
    case '\n': break;
    case 'x': {
      if (i + 2 < len + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) { buf_addc(&b, (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]))); i += 2; }
      else { buf_addc(&b, '\\'); buf_addc(&b, 'x'); }
      break;
    }
    case 'u': case 'U': {
      int nd = c == 'u' ? 4 : 8; unsigned cp = 0; int ok = i + nd < len + 0 || i + nd == len - 0;
      for (int k = 1; k <= nd && ok; k++) { if (i + k >= len || hexv(s[i + k]) < 0) ok = 0; else cp = cp * 16 + hexv(s[i + k]); }
      if (ok) { utf8_put(&b, cp); i += nd; } else { buf_addc(&b, '\\'); buf_addc(&b, c); }
      break;
    }
    default: buf_addc(&b, '\\'); buf_addc(&b, c); break;
    }
  }
  if (!b.p) buf_add(&b, "", 0);
  *outlen = b.len;
  return b.p;
}

static void lex_error(const char *file, int line, const char *msg) {
  throw_error("SyntaxError", "%s (%s, line %d)", msg, file, line);
}

static int is_id_start(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static int is_id_char(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

Token *lex(const char *src, const char *file, int *ntok) {
  TokVec tv = {0};
  int indents[256]; int nind = 1; indents[0] = 0;
  int line = 1, depth = 0, bol = 1, i = 0;
  int last_nl = 1; /* last emitted token was NEWLINE (or nothing) */
  for (;;) {
    if (bol && depth == 0) {
      int col = 0, j = i;
      while (src[j] == ' ' || src[j] == '\t' || src[j] == '\f') { col += src[j] == '\t' ? 4 : (src[j] == ' ' ? 1 : 0); j++; }
      if (src[j] == '#') { while (src[j] && src[j] != '\n') j++; }
      if (src[j] == '\r') j++;
      if (src[j] == '\n') { i = j + 1; line++; continue; }
      if (src[j] == 0) { i = j; break; }
      i = j;
      if (col > indents[nind - 1]) {
        if (nind >= 255) lex_error(file, line, "too many indentation levels");
        indents[nind++] = col; tv_push(&tv, mk(TK_INDENT, line));
      } else {
        while (col < indents[nind - 1]) { nind--; tv_push(&tv, mk(TK_DEDENT, line)); }
        if (col != indents[nind - 1]) lex_error(file, line, "unindent does not match any outer indentation level");
      }
      bol = 0;
    }
    unsigned char c = (unsigned char)src[i];
    if (c == 0) break;
    if (c == '\n') {
      i++;
      if (depth > 0) { line++; continue; }
      if (!last_nl) { tv_push(&tv, mk(TK_NEWLINE, line)); last_nl = 1; }
      line++; bol = 1; continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f') { i++; continue; }
    if (c == '#') { while (src[i] && src[i] != '\n') i++; continue; }
    if (c == '\\' && (src[i + 1] == '\n' || (src[i + 1] == '\r' && src[i + 2] == '\n'))) {
      i += src[i + 1] == '\n' ? 2 : 3; line++; continue;
    }
    last_nl = 0;
    /* string with optional prefix */
    int pi = i, raw = 0, isf = 0;
    if (is_id_start(c)) {
      int k = i;
      while (k < i + 2 && (src[k] == 'r' || src[k] == 'R' || src[k] == 'f' || src[k] == 'F' || src[k] == 'b' || src[k] == 'B' || src[k] == 'u' || src[k] == 'U')) k++;
      if (k > i && (src[k] == '"' || src[k] == '\'')) {
        for (int m = i; m < k; m++) { if (src[m] == 'r' || src[m] == 'R') raw = 1; if (src[m] == 'f' || src[m] == 'F') isf = 1; }
        pi = k;
        goto do_string;
      }
    }
    if (c == '"' || c == '\'') {
    do_string:;
      char q = src[pi]; int triple = (src[pi + 1] == q && src[pi + 2] == q);
      int start_line = line;
      int j = pi + (triple ? 3 : 1), sstart = j;
      for (;;) {
        if (!src[j]) lex_error(file, start_line, triple ? "unterminated triple-quoted string" : "unterminated string literal");
        if (src[j] == '\\' && src[j + 1]) { if (src[j + 1] == '\n') line++; j += 2; continue; }
        if (triple) {
          if (src[j] == q && src[j + 1] == q && src[j + 2] == q) break;
          if (src[j] == '\n') line++;
        } else {
          if (src[j] == q) break;
          if (src[j] == '\n') lex_error(file, start_line, "unterminated string literal");
        }
        j++;
      }
      int slen = j - sstart;
      Token t = mk(isf ? TK_FSTR : TK_STR, start_line);
      t.raw = raw;
      if (isf || raw) { t.text = xmalloc(slen + 1); memcpy(t.text, src + sstart, slen); t.text[slen] = 0; t.len = slen; }
      else { t.text = decode_escapes(src + sstart, slen, &t.len); }
      tv_push(&tv, t);
      i = j + (triple ? 3 : 1);
      continue;
    }
    if (isdigit(c) || (c == '.' && isdigit((unsigned char)src[i + 1]))) {
      int j = i; Token t = mk(TK_INT, line);
      char nb[128]; int nn = 0;
      if (c == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X' || src[i + 1] == 'b' || src[i + 1] == 'B' || src[i + 1] == 'o' || src[i + 1] == 'O')) {
        int base = (src[i + 1] == 'x' || src[i + 1] == 'X') ? 16 : (src[i + 1] == 'b' || src[i + 1] == 'B') ? 2 : 8;
        j = i + 2;
        while (isalnum((unsigned char)src[j]) || src[j] == '_') { if (src[j] != '_' && nn < 120) nb[nn++] = src[j]; j++; }
        nb[nn] = 0;
        char *end; t.i = (int64_t)strtoull(nb, &end, base);
        if (*end || !nn) lex_error(file, line, "invalid number literal");
      } else {
        int isfl = 0;
        while (isdigit((unsigned char)src[j]) || src[j] == '_') { if (src[j] != '_' && nn < 120) nb[nn++] = src[j]; j++; }
        if (src[j] == '.' && (isdigit((unsigned char)src[j + 1]) || (!is_id_start((unsigned char)src[j + 1]) && src[j + 1] != '.'))) {
          isfl = 1; nb[nn++] = '.'; j++;
          while (isdigit((unsigned char)src[j]) || src[j] == '_') { if (src[j] != '_' && nn < 120) nb[nn++] = src[j]; j++; }
        }
        if ((src[j] == 'e' || src[j] == 'E') && (isdigit((unsigned char)src[j + 1]) || ((src[j + 1] == '+' || src[j + 1] == '-') && isdigit((unsigned char)src[j + 2])))) {
          isfl = 1; nb[nn++] = 'e'; j++;
          if (src[j] == '+' || src[j] == '-') nb[nn++] = src[j++];
          while (isdigit((unsigned char)src[j])) { if (nn < 120) nb[nn++] = src[j]; j++; }
        }
        nb[nn] = 0;
        if (isfl) { t.k = TK_FLOAT; t.d = strtod(nb, NULL); }
        else {
          errno = 0; char *end; long long v = strtoll(nb, &end, 10);
          if (errno) lex_error(file, line, "integer literal too large");
          t.i = v;
        }
      }
      if (is_id_start((unsigned char)src[j])) lex_error(file, line, "invalid number literal");
      tv_push(&tv, t); i = j; continue;
    }
    if (is_id_start(c)) {
      int j = i; while (is_id_char((unsigned char)src[j])) j++;
      Token t = mk(TK_NAME, line);
      t.len = j - i; t.text = xmalloc(t.len + 1); memcpy(t.text, src + i, t.len); t.text[t.len] = 0;
      tv_push(&tv, t); i = j; continue;
    }
    /* operators */
    static const char *ops3[] = {"**=", "//=", ">>=", "<<=", "...", NULL};
    static const char *ops2[] = {"==", "!=", "<=", ">=", "**", "//", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", "<<", ">>", ":=", NULL};
    const char *found = NULL; int flen = 0;
    for (int k = 0; ops3[k]; k++) if (!strncmp(src + i, ops3[k], 3)) { found = ops3[k]; flen = 3; break; }
    if (!found) for (int k = 0; ops2[k]; k++) if (!strncmp(src + i, ops2[k], 2)) { found = ops2[k]; flen = 2; break; }
    Token t = mk(TK_OP, line);
    if (found) { t.text = xstrdup(found); t.len = flen; }
    else {
      if (!strchr("+-*/%()[]{},:.;=<>&|^~@", c)) {
        char m[64]; snprintf(m, sizeof m, "invalid character '%c'", c); lex_error(file, line, m);
      }
      t.text = xmalloc(2); t.text[0] = (char)c; t.text[1] = 0; t.len = 1; flen = 1;
      if (c == '(' || c == '[' || c == '{') depth++;
      if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
    }
    tv_push(&tv, t); i += flen;
  }
  if (!last_nl && tv.n > 0) tv_push(&tv, mk(TK_NEWLINE, line));
  while (nind > 1) { nind--; tv_push(&tv, mk(TK_DEDENT, line)); }
  tv_push(&tv, mk(TK_EOF, line));
  *ntok = tv.n;
  return tv.t;
}
