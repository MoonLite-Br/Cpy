/* parser.c - recursive-descent parser producing an AST */
#include "cpy.h"

typedef struct { Node **v; int n, cap; } NodeVec;
typedef struct { Str **v; int n, cap; } StrVec;

static Token *T;          /* current token array */
static int P;             /* current position */
static const char *G_file;
static FuncDef *cur_fd;   /* function being parsed (for global/nonlocal) */

static void nv_push(NodeVec *nv, Node *n) {
  if (nv->n >= nv->cap) { nv->cap = nv->cap ? nv->cap * 2 : 8; nv->v = xrealloc(nv->v, nv->cap * sizeof(Node *)); }
  nv->v[nv->n++] = n;
}
static void sv_push(StrVec *sv, Str *s) {
  if (sv->n >= sv->cap) { sv->cap = sv->cap ? sv->cap * 2 : 4; sv->v = xrealloc(sv->v, sv->cap * sizeof(Str *)); }
  sv->v[sv->n++] = s;
}

static Node *new_node(int k, int line) {
  Node *n = xcalloc(1, sizeof(Node)); n->k = k; n->line = line; n->val = V_none(); return n;
}
static Node *mk_name(Str *s, int line) { Node *n = new_node(N_NAME, line); n->s = s; return n; }

static void perr(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void perr(const char *fmt, ...) {
  char m[256]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
  throw_error("SyntaxError", "%s (%s, line %d)", m, G_file, T[P].line);
}

static Token *tok(void) { return &T[P]; }
static int is_op(const char *s) { return T[P].k == TK_OP && strcmp(T[P].text, s) == 0; }
static int is_kw(const char *s) { return T[P].k == TK_NAME && strcmp(T[P].text, s) == 0; }
static int accept_op(const char *s) { if (is_op(s)) { P++; return 1; } return 0; }
static int accept_kw(const char *s) { if (is_kw(s)) { P++; return 1; } return 0; }
static void expect_op(const char *s) { if (!accept_op(s)) perr("invalid syntax: expected '%s'", s); }
static void expect_kw(const char *s) { if (!accept_kw(s)) perr("invalid syntax: expected '%s'", s); }

static const char *KEYWORDS[] = {"and", "as", "assert", "break", "class", "continue", "def", "del", "elif",
  "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
  "not", "or", "pass", "raise", "return", "try", "while", "with", "yield", "None", "True", "False", NULL};
static int is_keyword(const char *s) {
  for (int i = 0; KEYWORDS[i]; i++) if (!strcmp(s, KEYWORDS[i])) return 1;
  return 0;
}
static Str *parse_dotted(void);
static Str *expect_name(void) {
  if (T[P].k != TK_NAME || is_keyword(T[P].text)) perr("invalid syntax: expected a name");
  Str *s = intern(T[P].text, T[P].len); P++; return s;
}

static Str *parse_dotted(void) {
  Buf b; buf_init(&b);
  Str *first = expect_name(); buf_add(&b, first->s, first->len);
  while (is_op(".")) { P++; Str *nx = expect_name(); buf_addc(&b, '.'); buf_add(&b, nx->s, nx->len); }
  Str *r = intern(b.p, b.len); free(b.p); return r;
}

static Node *parse_test(void);
static Node *parse_or(void);
static Node *parse_bitor(void);
static Node *parse_testlist(void);
#define parse_testlist_fwd parse_testlist
static void parse_block(NodeVec *out);
static void parse_stmt(NodeVec *out);

/* ------------------------------------------------------ f-strings */
static Node *parse_expr_string(const char *s, int len, int line) {
  char *tmp = xmalloc(len + 3); tmp[0] = '('; memcpy(tmp + 1, s, len); tmp[len + 1] = ')'; tmp[len + 2] = 0;
  int nt; Token *saveT = T; int saveP = P;
  Token *toks = lex(tmp, G_file, &nt);
  for (int i = 0; i < nt; i++) toks[i].line = line;
  T = toks; P = 0;
  Node *e = parse_testlist();
  if (T[P].k != TK_NEWLINE && T[P].k != TK_EOF) perr("invalid syntax in f-string expression");
  T = saveT; P = saveP; free(tmp);
  return e;
}

static Node *const_str(const char *s, int len, int line) {
  Node *n = new_node(N_CONST, line); n->val = V_strn(s, len); return n;
}

static Node *parse_fstring(Token *t) {
  Node *n = new_node(N_FSTR, t->line);
  NodeVec parts = {0};
  const char *s = t->text; int len = t->len; Buf lit; buf_init(&lit);
  for (int i = 0; i < len; i++) {
    char c = s[i];
    if (c == '{' && i + 1 < len && s[i + 1] == '{') { buf_addc(&lit, '{'); i++; continue; }
    if (c == '}' && i + 1 < len && s[i + 1] == '}') { buf_addc(&lit, '}'); i++; continue; }
    if (c == '}') perr("f-string: single '}' is not allowed");
    if (c != '{') { buf_addc(&lit, c); continue; }
    /* flush literal */
    if (lit.len) {
      if (t->raw) nv_push(&parts, const_str(lit.p, lit.len, t->line));
      else { int ol; char *d = decode_escapes(lit.p, lit.len, &ol); nv_push(&parts, const_str(d, ol, t->line)); free(d); }
    }
    free(lit.p); buf_init(&lit);
    int j = i + 1, depth = 0; char quote = 0; int colon = -1, bang = -1;
    for (; j < len; j++) {
      char d = s[j];
      if (quote) { if (d == quote) quote = 0; continue; }
      if (d == '\'' || d == '"') { quote = d; continue; }
      if (d == '(' || d == '[' || d == '{') depth++;
      else if (d == ')' || d == ']') depth--;
      else if (d == '}') { if (depth == 0) break; depth--; }
      else if (depth == 0 && d == ':' && colon < 0) colon = j;
      else if (depth == 0 && d == '!' && colon < 0 && j + 1 < len && s[j + 1] != '=') bang = j;
    }
    if (j >= len) perr("f-string: expecting '}'");
    int eend = j;
    if (colon >= 0) eend = colon;
    int conv = 0;
    if (bang >= 0 && (colon < 0 || bang < colon)) { conv = s[bang + 1]; eend = bang; }
    int e2 = eend; while (e2 > i + 1 && isspace((unsigned char)s[e2 - 1])) e2--;
    int dbg = (e2 > i + 2 && s[e2 - 1] == '=' && !strchr("=!<>", s[e2 - 2]));
    Node *fm = new_node(N_FMT, t->line);
    if (dbg) {
      nv_push(&parts, const_str(s + i + 1, eend - (i + 1), t->line));
      if (!conv && colon < 0) conv = 'r';
    }
    fm->a = parse_expr_string(s + i + 1, (dbg ? e2 - 1 : eend) - (i + 1), t->line);
    fm->op = conv;
    if (colon >= 0) fm->s = intern(s + colon + 1, j - colon - 1);
    nv_push(&parts, fm);
    i = j;
  }
  if (lit.len) {
    if (t->raw) nv_push(&parts, const_str(lit.p, lit.len, t->line));
    else { int ol; char *d = decode_escapes(lit.p, lit.len, &ol); nv_push(&parts, const_str(d, ol, t->line)); free(d); }
  }
  free(lit.p);
  n->v = parts.v; n->nv = parts.n;
  return n;
}

/* ------------------------------------------------------ expressions */
static void parse_params(FuncDef *fd, int lambda) {
  StrVec ps = {0}; NodeVec ds = {0};
  int seen_star = 0;
  fd->star = -1; fd->kwstar = -1;
  while (!(lambda ? is_op(":") : is_op(")"))) {
    if (accept_op("/")) { /* positional-only marker: accepted and ignored */ }
    else if (accept_op("**")) {
      fd->kwstar = ps.n; sv_push(&ps, expect_name()); nv_push(&ds, NULL);
      if (!lambda && accept_op(":")) parse_test();
    } else if (accept_op("*")) {
      seen_star = 1;
      if (tok()->k == TK_NAME && !is_keyword(tok()->text)) {
        fd->star = ps.n; sv_push(&ps, expect_name()); nv_push(&ds, NULL);
        if (!lambda && accept_op(":")) parse_test();
      }
    } else {
      sv_push(&ps, expect_name());
      if (!lambda && accept_op(":")) parse_test();
      if (accept_op("=")) nv_push(&ds, parse_test()); else nv_push(&ds, NULL);
      if (seen_star) fd->nkwonly++; else fd->nnormal++;
    }
    if (ps.n > MAX_PARAMS) perr("too many parameters");
    if (!accept_op(",")) break;
  }
  fd->params = ps.v; fd->defaults = ds.v; fd->nparams = ps.n;
}

static Node *parse_lambda(void) {
  int line = tok()->line; P++;
  FuncDef *fd = xcalloc(1, sizeof(FuncDef));
  fd->name = INTERN("<lambda>"); fd->file = G_file; fd->line = line;
  parse_params(fd, 1);
  expect_op(":");
  fd->expr = parse_test();
  Node *n = new_node(N_LAMBDA, line); n->fd = fd; return n;
}

static int pdepth;
static Node *parse_test_inner(void);
static Node *parse_test(void) {
  if (++pdepth > 300) { pdepth = 0; perr("too many nested expressions"); }
  Node *r = parse_test_inner();
  if (r->k == N_NAME && is_op(":=")) {
    int line = tok()->line; P++;
    Node *w = new_node(N_WALRUS, line); w->a = r; w->b = parse_test(); r = w;
  }
  pdepth--;
  return r;
}
static Node *parse_test_inner(void) {
  if (is_kw("lambda")) return parse_lambda();
  Node *e = parse_or();
  if (is_kw("if")) {
    int line = tok()->line; P++;
    Node *c = parse_or(); expect_kw("else"); Node *o = parse_test();
    Node *n = new_node(N_IFEXP, line); n->a = c; n->b = e; n->c = o; return n;
  }
  return e;
}

static Node *parse_not(void);
static Node *parse_and(void) {
  Node *l = parse_not();
  while (is_kw("and")) {
    int line = tok()->line; P++;
    Node *n = new_node(N_AND, line); n->a = l; n->b = parse_not(); l = n;
  }
  return l;
}
static Node *parse_or(void) {
  Node *l = parse_and();
  while (is_kw("or")) {
    int line = tok()->line; P++;
    Node *n = new_node(N_OR, line); n->a = l; n->b = parse_and(); l = n;
  }
  return l;
}
static Node *parse_cmp(void);
static Node *parse_not(void) {
  if (is_kw("not")) { int line = tok()->line; P++; Node *n = new_node(N_NOT, line); n->a = parse_not(); return n; }
  return parse_cmp();
}

static int cmp_op_at(int *len) {
  *len = 1;
  if (T[P].k == TK_OP) {
    const char *t = T[P].text;
    if (!strcmp(t, "==")) return C_EQ; if (!strcmp(t, "!=")) return C_NE;
    if (!strcmp(t, "<")) return C_LT; if (!strcmp(t, "<=")) return C_LE;
    if (!strcmp(t, ">")) return C_GT; if (!strcmp(t, ">=")) return C_GE;
    return 0;
  }
  if (is_kw("in")) return C_IN;
  if (is_kw("not") && T[P + 1].k == TK_NAME && !strcmp(T[P + 1].text, "in")) { *len = 2; return C_NOTIN; }
  if (is_kw("is")) {
    if (T[P + 1].k == TK_NAME && !strcmp(T[P + 1].text, "not")) { *len = 2; return C_ISNOT; }
    return C_IS;
  }
  return 0;
}
static Node *parse_cmp(void) {
  Node *l = parse_bitor();
  int len, op = cmp_op_at(&len);
  if (!op) return l;
  Node *n = new_node(N_CMP, tok()->line); n->a = l;
  NodeVec rs = {0}; int ops[32]; int no = 0;
  while (op) {
    if (no >= 32) perr("too many chained comparisons");
    P += len; ops[no++] = op; nv_push(&rs, parse_bitor());
    op = cmp_op_at(&len);
  }
  n->v = rs.v; n->nv = rs.n; n->ops = xmalloc(no * sizeof(int)); memcpy(n->ops, ops, no * sizeof(int));
  return n;
}

static Node *binop_node(int op, Node *a, Node *b, int line) {
  Node *n = new_node(N_BINOP, line); n->op = op; n->a = a; n->b = b; return n;
}
static Node *parse_bitxor(void);
static Node *parse_bitand(void);
static Node *parse_shift(void);
static Node *parse_arith(void);
static Node *parse_term(void);
static Node *parse_factor(void);
static Node *parse_power(void);

static Node *parse_bitor(void) {
  Node *l = parse_bitxor();
  while (is_op("|")) { int line = tok()->line; P++; l = binop_node(OP_BOR, l, parse_bitxor(), line); }
  return l;
}
static Node *parse_bitxor(void) {
  Node *l = parse_bitand();
  while (is_op("^")) { int line = tok()->line; P++; l = binop_node(OP_BXOR, l, parse_bitand(), line); }
  return l;
}
static Node *parse_bitand(void) {
  Node *l = parse_shift();
  while (is_op("&")) { int line = tok()->line; P++; l = binop_node(OP_BAND, l, parse_shift(), line); }
  return l;
}
static Node *parse_shift(void) {
  Node *l = parse_arith();
  for (;;) {
    int line = tok()->line;
    if (is_op("<<")) { P++; l = binop_node(OP_SHL, l, parse_arith(), line); }
    else if (is_op(">>")) { P++; l = binop_node(OP_SHR, l, parse_arith(), line); }
    else return l;
  }
}
static Node *parse_arith(void) {
  Node *l = parse_term();
  for (;;) {
    int line = tok()->line;
    if (is_op("+")) { P++; l = binop_node(OP_ADD, l, parse_term(), line); }
    else if (is_op("-")) { P++; l = binop_node(OP_SUB, l, parse_term(), line); }
    else return l;
  }
}
static Node *parse_term(void) {
  Node *l = parse_factor();
  for (;;) {
    int line = tok()->line, op = 0;
    if (is_op("*")) op = OP_MUL; else if (is_op("/")) op = OP_DIV;
    else if (is_op("//")) op = OP_FDIV; else if (is_op("%")) op = OP_MOD;
    else return l;
    P++; l = binop_node(op, l, parse_factor(), line);
  }
}
static Node *parse_factor(void) {
  int line = tok()->line, op = 0;
  if (is_op("-")) op = OP_NEG; else if (is_op("+")) op = OP_POS; else if (is_op("~")) op = OP_INV;
  if (op) { P++; Node *n = new_node(N_UNARY, line); n->op = op; n->a = parse_factor(); return n; }
  return parse_power();
}

static Node *parse_elem(void) {
  if (is_op("*")) { int line = tok()->line; P++; Node *s = new_node(N_STAR, line); s->a = parse_bitor(); return s; }
  return parse_test();
}
static Node *parse_yield(void) {
  int line = tok()->line; P++;
  if (!cur_fd) perr("'yield' outside function");
  cur_fd->is_gen = 1;
  if (accept_kw("from")) { Node *n = new_node(N_YIELDFROM, line); n->a = parse_test(); return n; }
  Node *n = new_node(N_YIELD, line);
  if (T[P].k != TK_NEWLINE && T[P].k != TK_EOF && !is_op(")") && !is_op(";") && !is_op("=") && !is_op("]") && !is_op("}")) n->a = parse_testlist_fwd();
  return n;
}
static Node *parse_atom(void);
static Node *parse_call_args(Node *fn) {
  Node *n = new_node(N_CALL, tok()->line); n->a = fn;
  NodeVec pos = {0}, kwv = {0}; StrVec kwn = {0};
  P++; /* ( */
  while (!is_op(")")) {
    if (is_op("**")) {
      int line = tok()->line; P++; Node *s = new_node(N_DSTAR, line); s->a = parse_test(); nv_push(&pos, s); n->op = 1;
    } else if (is_op("*")) {
      int line = tok()->line; P++; Node *s = new_node(N_STAR, line); s->a = parse_test(); nv_push(&pos, s); n->op = 1;
    } else if (tok()->k == TK_NAME && !is_keyword(tok()->text) && T[P + 1].k == TK_OP && !strcmp(T[P + 1].text, "=")) {
      sv_push(&kwn, expect_name()); P++; nv_push(&kwv, parse_test());
    } else {
      Node *e = parse_test();
      if (is_kw("for")) { /* generator expression argument -> eager list */
        Node *lc = new_node(N_GENEXP, e->line); lc->a = e;
        NodeVec cl = {0};
        while (is_kw("for") || is_kw("if")) {
          if (accept_kw("for")) {
            Node *c = new_node(N_COMPFOR, tok()->line);
            c->a = parse_bitor();
            if (is_op(",")) { NodeVec tv = {0}; nv_push(&tv, c->a); while (accept_op(",")) { if (is_kw("in")) break; nv_push(&tv, parse_bitor()); } Node *tn = new_node(N_TUPLE, c->line); tn->v = tv.v; tn->nv = tv.n; c->a = tn; }
            expect_kw("in"); c->b = parse_or(); nv_push(&cl, c);
          } else { P++; Node *c = new_node(N_COMPIF, tok()->line); c->a = parse_or(); nv_push(&cl, c); }
        }
        lc->v = cl.v; lc->nv = cl.n; e = lc;
      }
      nv_push(&pos, e);
    }
    if (!accept_op(",")) break;
  }
  expect_op(")");
  n->v = pos.v; n->nv = pos.n; n->v2 = kwv.v; n->nv2 = kwv.n; n->names = kwn.v;
  return n;
}

static Node *parse_subscript(Node *obj) {
  int line = tok()->line; P++; /* [ */
  Node *lo = NULL, *hi = NULL, *st = NULL; int is_slice = 0;
  if (!is_op(":")) lo = parse_test();
  if (accept_op(":")) {
    is_slice = 1;
    if (!is_op(":") && !is_op("]")) hi = parse_test();
    if (accept_op(":")) { if (!is_op("]")) st = parse_test(); }
  }
  expect_op("]");
  Node *n = new_node(N_INDEX, line); n->a = obj;
  if (is_slice) { Node *s = new_node(N_SLICE, line); s->a = lo; s->b = hi; s->c = st; n->b = s; }
  else n->b = lo;
  return n;
}

static Node *parse_power(void) {
  Node *e = parse_atom();
  for (;;) {
    if (is_op("(")) e = parse_call_args(e);
    else if (is_op("[")) e = parse_subscript(e);
    else if (is_op(".")) {
      int line = tok()->line; P++;
      if (tok()->k != TK_NAME) perr("invalid syntax: expected attribute name");
      Node *n = new_node(N_ATTR, line); n->a = e; n->s = intern(tok()->text, tok()->len); P++; e = n;
    } else break;
  }
  if (is_op("**")) {
    int line = tok()->line; P++;
    return binop_node(OP_POW, e, parse_factor(), line);
  }
  return e;
}

static Node *parse_comp_clauses(Node *lc) {
  NodeVec cl = {0};
  while (is_kw("for") || is_kw("if")) {
    if (accept_kw("for")) {
      Node *c = new_node(N_COMPFOR, tok()->line);
      c->a = parse_bitor();
      if (is_op(",")) {
        NodeVec tv = {0}; nv_push(&tv, c->a);
        while (accept_op(",")) { if (is_kw("in")) break; nv_push(&tv, parse_bitor()); }
        Node *tn = new_node(N_TUPLE, c->line); tn->v = tv.v; tn->nv = tv.n; c->a = tn;
      }
      expect_kw("in"); c->b = parse_or(); nv_push(&cl, c);
    } else {
      P++; Node *c = new_node(N_COMPIF, tok()->line); c->a = parse_or(); nv_push(&cl, c);
    }
  }
  lc->v = cl.v; lc->nv = cl.n;
  return lc;
}

static Node *parse_atom(void) {
  Token *t = tok(); int line = t->line;
  switch (t->k) {
  case TK_INT: { P++; Node *n = new_node(N_CONST, line); n->val = V_int(t->i); return n; }
  case TK_FLOAT: { P++; Node *n = new_node(N_CONST, line); n->val = V_float(t->d); return n; }
  case TK_STR: {
    Buf b; buf_init(&b);
    while (tok()->k == TK_STR) { buf_add(&b, tok()->text, tok()->len); P++; }
    Node *n = new_node(N_CONST, line); n->val = buf_to_str(&b); return n;
  }
  case TK_FSTR: { P++; return parse_fstring(t); }
  case TK_NAME: {
    if (!strcmp(t->text, "None")) { P++; Node *n = new_node(N_CONST, line); n->val = V_none(); return n; }
    if (!strcmp(t->text, "True")) { P++; Node *n = new_node(N_CONST, line); n->val = V_bool(1); return n; }
    if (!strcmp(t->text, "False")) { P++; Node *n = new_node(N_CONST, line); n->val = V_bool(0); return n; }
    if (is_keyword(t->text)) perr("invalid syntax near '%s'", t->text);
    Node *n = new_node(N_NAME, line); n->s = intern(t->text, t->len); P++; return n;
  }
  case TK_OP: {
    if (is_op("...")) { P++; return mk_name(INTERN("Ellipsis"), line); }
    if (is_op("(")) {
      P++;
      if (accept_op(")")) { Node *n = new_node(N_TUPLE, line); return n; }
      if (is_kw("yield")) { Node *y = parse_yield(); expect_op(")"); return y; }
      Node *e = parse_elem();
      if (is_kw("for")) { Node *lc = new_node(N_GENEXP, line); lc->a = e; parse_comp_clauses(lc); expect_op(")"); return lc; }
      if (is_op(",") || e->k == N_STAR) {
        NodeVec v = {0}; nv_push(&v, e);
        while (accept_op(",")) { if (is_op(")")) break; nv_push(&v, parse_elem()); }
        expect_op(")");
        Node *n = new_node(N_TUPLE, line); n->v = v.v; n->nv = v.n; return n;
      }
      expect_op(")"); return e;
    }
    if (is_op("[")) {
      P++;
      NodeVec v = {0};
      if (accept_op("]")) { Node *n = new_node(N_LIST, line); return n; }
      Node *e = parse_elem();
      if (is_kw("for")) { Node *lc = new_node(N_LISTCOMP, line); lc->a = e; parse_comp_clauses(lc); expect_op("]"); return lc; }
      nv_push(&v, e);
      while (accept_op(",")) { if (is_op("]")) break; nv_push(&v, parse_elem()); }
      expect_op("]");
      Node *n = new_node(N_LIST, line); n->v = v.v; n->nv = v.n; return n;
    }
    if (is_op("{")) {
      P++;
      if (accept_op("}")) { Node *n = new_node(N_DICT, line); return n; }
      NodeVec ks = {0}, vs = {0};
      Node *first = NULL;
      if (is_op("**")) {
        P++; nv_push(&ks, NULL); nv_push(&vs, parse_bitor());
      } else {
        first = parse_elem();
        if (is_op(":") && first->k != N_STAR) {
          P++; Node *val = parse_test();
          if (is_kw("for")) {
            Node *dc = new_node(N_DICTCOMP, line); dc->a = first; dc->b = val; parse_comp_clauses(dc); expect_op("}"); return dc;
          }
          nv_push(&ks, first); nv_push(&vs, val);
        } else { /* set display or set comprehension */
          if (is_kw("for")) { Node *sc = new_node(N_SETCOMP, line); sc->a = first; parse_comp_clauses(sc); expect_op("}"); return sc; }
          NodeVec sv = {0}; nv_push(&sv, first);
          while (accept_op(",")) { if (is_op("}")) break; nv_push(&sv, parse_elem()); }
          expect_op("}");
          Node *n = new_node(N_SET, line); n->v = sv.v; n->nv = sv.n; return n;
        }
      }
      while (accept_op(",")) {
        if (is_op("}")) break;
        if (accept_op("**")) { nv_push(&ks, NULL); nv_push(&vs, parse_bitor()); }
        else { Node *k2 = parse_test(); expect_op(":"); nv_push(&ks, k2); nv_push(&vs, parse_test()); }
      }
      expect_op("}");
      Node *n = new_node(N_DICT, line); n->v = ks.v; n->nv = ks.n; n->v2 = vs.v; n->nv2 = vs.n; return n;
    }
    perr("invalid syntax near '%s'", t->text);
  }
  case TK_NEWLINE: perr("invalid syntax: unexpected end of line");
  case TK_INDENT: perr("unexpected indent");
  case TK_EOF: perr("unexpected end of input");
  default: perr("invalid syntax");
  }
}

static Node *parse_testlist(void) {
  int line = tok()->line;
  Node *e = parse_elem();
  if (!is_op(",") && e->k != N_STAR) return e;
  NodeVec v = {0}; nv_push(&v, e);
  while (accept_op(",")) {
    if (T[P].k == TK_NEWLINE || T[P].k == TK_EOF || is_op("=") || is_op(")") || is_op(";") || is_op(":")) break;
    nv_push(&v, parse_elem());
  }
  Node *n = new_node(N_TUPLE, line); n->v = v.v; n->nv = v.n; return n;
}

/* target list for "for" loops: a, b  (stops before 'in') */
static Node *parse_target_elem(void) {
  if (is_op("*")) { int line = tok()->line; P++; Node *s = new_node(N_STAR, line); s->a = parse_bitor(); return s; }
  return parse_bitor();
}
static Node *parse_target_list(void) {
  int line = tok()->line;
  Node *e = parse_target_elem();
  if (!is_op(",") && e->k != N_STAR) return e;
  NodeVec v = {0}; nv_push(&v, e);
  while (accept_op(",")) { if (is_kw("in")) break; nv_push(&v, parse_target_elem()); }
  Node *n = new_node(N_TUPLE, line); n->v = v.v; n->nv = v.n; return n;
}

/* ------------------------------------------------------- statements */
static int assignable(Node *n) {
  if (n->k == N_NAME || n->k == N_ATTR || n->k == N_INDEX) return 1;
  if (n->k == N_STAR) return assignable(n->a);
  if (n->k == N_TUPLE || n->k == N_LIST) { for (int i = 0; i < n->nv; i++) if (!assignable(n->v[i])) return 0; return 1; }
  return 0;
}

static int aug_op(void) {
  if (T[P].k != TK_OP) return 0;
  const char *t = T[P].text;
  if (!strcmp(t, "+=")) return OP_ADD; if (!strcmp(t, "-=")) return OP_SUB;
  if (!strcmp(t, "*=")) return OP_MUL; if (!strcmp(t, "/=")) return OP_DIV;
  if (!strcmp(t, "//=")) return OP_FDIV; if (!strcmp(t, "%=")) return OP_MOD;
  if (!strcmp(t, "**=")) return OP_POW; if (!strcmp(t, "&=")) return OP_BAND;
  if (!strcmp(t, "|=")) return OP_BOR; if (!strcmp(t, "^=")) return OP_BXOR;
  if (!strcmp(t, "<<=")) return OP_SHL; if (!strcmp(t, ">>=")) return OP_SHR;
  return 0;
}

static Node *parse_expr_stmt(void) {
  int line = tok()->line;
  Node *first = is_kw("yield") ? parse_yield() : parse_testlist();
  int ao = aug_op();
  if (ao) {
    if (first->k != N_NAME && first->k != N_ATTR && !(first->k == N_INDEX && first->b->k != N_SLICE))
      perr("illegal expression for augmented assignment");
    P++;
    Node *n = new_node(N_AUG, line); n->op = ao; n->a = first; n->b = parse_testlist(); return n;
  }
  if (is_op(":") && (first->k == N_NAME || first->k == N_ATTR)) { /* variable annotation: x: int = 5 */
    P++; parse_test();
    Node *an = new_node(N_ANNASSIGN, line); an->a = first;
    if (accept_op("=")) an->b = is_kw("yield") ? parse_yield() : parse_testlist();
    return an;
  }
  if (is_op("=")) {
    NodeVec targets = {0}; nv_push(&targets, first); Node *val = NULL;
    while (accept_op("=")) {
      Node *nx = is_kw("yield") ? parse_yield() : parse_testlist();
      if (is_op("=")) nv_push(&targets, nx); else val = nx;
    }
    for (int i = 0; i < targets.n; i++) if (!assignable(targets.v[i])) perr("cannot assign to expression");
    Node *n = new_node(N_ASSIGN, line); n->v = targets.v; n->nv = targets.n; n->a = val; return n;
  }
  Node *n = new_node(N_EXPR, line); n->a = first; return n;
}

static void parse_simple_line(NodeVec *out) {
  for (;;) {
    int line = tok()->line;
    Node *n = NULL;
    if (accept_kw("pass")) n = new_node(N_PASS, line);
    else if (accept_kw("break")) n = new_node(N_BREAK, line);
    else if (accept_kw("continue")) n = new_node(N_CONTINUE, line);
    else if (accept_kw("return")) {
      n = new_node(N_RETURN, line);
      if (T[P].k != TK_NEWLINE && T[P].k != TK_EOF && !is_op(";")) n->a = parse_testlist();
    } else if (accept_kw("raise")) {
      n = new_node(N_RAISE, line);
      if (T[P].k != TK_NEWLINE && T[P].k != TK_EOF && !is_op(";")) {
        n->a = parse_test();
        if (accept_kw("from")) parse_test();
      }
    } else if (accept_kw("import")) {
      for (;;) {
        Node *m = new_node(N_IMPORT, line); m->s = parse_dotted();
        if (accept_kw("as")) m->s2 = expect_name();
        Str *first = m->s;
        { const char *dt = strchr(m->s->s, '.'); if (dt) first = intern(m->s->s, (int)(dt - m->s->s)); }
        m->a = mk_name(m->s2 ? m->s2 : first, line);
        nv_push(out, m);
        if (!accept_op(",")) break;
      }
      n = NULL;
    } else if (accept_kw("from")) {
      n = new_node(N_FROM, line);
      { int level = 0;
        for (;;) { if (is_op(".")) { level++; P++; } else if (is_op("...")) { level += 3; P++; } else break; }
        n->op = level;
        if (is_kw("import")) { if (!level) perr("invalid syntax"); n->s = INTERN(""); } else n->s = parse_dotted(); }
      expect_kw("import");
      StrVec names = {0}; NodeVec targets = {0};
      int paren = accept_op("(");
      if (accept_op("*")) { sv_push(&names, INTERN("*")); nv_push(&targets, mk_name(INTERN("*"), line)); }
      else for (;;) {
        Str *nm = expect_name(); Str *al = nm;
        sv_push(&names, nm);
        if (accept_kw("as")) al = expect_name();
        nv_push(&targets, mk_name(al, line));
        if (!accept_op(",")) break;
        if (paren && is_op(")")) break;
      }
      if (paren) expect_op(")");
      n->names = names.v; n->v = targets.v; n->nv = names.n;
    } else if (is_kw("global") || is_kw("nonlocal")) {
      int isg = is_kw("global"); P++;
      StrVec names = {0};
      for (;;) { sv_push(&names, expect_name()); if (!accept_op(",")) break; }
      if (cur_fd) {
        for (int i = 0; i < names.n; i++) {
          if (isg) { cur_fd->globals = xrealloc(cur_fd->globals, (cur_fd->nglobals + 1) * sizeof(Str *)); cur_fd->globals[cur_fd->nglobals++] = names.v[i]; }
          else { cur_fd->nonlocals = xrealloc(cur_fd->nonlocals, (cur_fd->nnonlocals + 1) * sizeof(Str *)); cur_fd->nonlocals[cur_fd->nnonlocals++] = names.v[i]; }
        }
      }
      n = new_node(N_PASS, line);
    } else if (accept_kw("del")) {
      for (;;) {
        Node *d = new_node(N_DEL, line); d->a = parse_bitor(); nv_push(out, d);
        if (!accept_op(",")) break;
      }
      n = NULL;
    } else if (accept_kw("assert")) {
      n = new_node(N_ASSERT, line); n->a = parse_test();
      if (accept_op(",")) n->b = parse_test();
    } else n = parse_expr_stmt();
    if (n) nv_push(out, n);
    if (accept_op(";")) { if (T[P].k == TK_NEWLINE || T[P].k == TK_EOF) break; continue; }
    break;
  }
  if (T[P].k == TK_NEWLINE) P++;
  else if (T[P].k != TK_EOF) perr("invalid syntax");
}

static void parse_block(NodeVec *out) {
  expect_op(":");
  if (T[P].k == TK_NEWLINE) {
    P++;
    if (T[P].k != TK_INDENT) perr("expected an indented block");
    P++;
    while (T[P].k != TK_DEDENT && T[P].k != TK_EOF) parse_stmt(out);
    if (T[P].k == TK_DEDENT) P++;
  } else parse_simple_line(out);
}

static Node *make_block(NodeVec *v, int line) {
  Node *b = new_node(N_BLOCK, line); b->v = v->v; b->nv = v->n; return b;
}

static Node *parse_if(void) {
  int line = tok()->line; P++;
  Node *n = new_node(N_IF, line); n->a = parse_test();
  NodeVec body = {0}; parse_block(&body); n->v = body.v; n->nv = body.n;
  if (is_kw("elif")) {
    NodeVec eb = {0}; nv_push(&eb, parse_if()); /* parse_if consumes 'elif' as 'if' */
    n->v2 = eb.v; n->nv2 = eb.n;
  } else if (accept_kw("else")) {
    NodeVec eb = {0}; parse_block(&eb); n->v2 = eb.v; n->nv2 = eb.n;
  }
  return n;
}

static void parse_def(NodeVec *out, int line) {
  FuncDef *fd = xcalloc(1, sizeof(FuncDef));
  fd->file = G_file; fd->line = line;
  fd->name = expect_name();
  expect_op("(");
  parse_params(fd, 0);
  expect_op(")");
  if (accept_op("->")) parse_test();
  FuncDef *saved = cur_fd; cur_fd = fd;
  NodeVec body = {0}; parse_block(&body);
  cur_fd = saved;
  fd->body = body.v; fd->nbody = body.n;
  if (!fd->is_gen && body.n == 1 && body.v[0]->k == N_RETURN && body.v[0]->a) fd->expr = body.v[0]->a; /* fast path: def f(x): return expr */
  Node *n = new_node(N_DEF, line); n->fd = fd; n->a = mk_name(fd->name, line); nv_push(out, n);
}

static void parse_stmt(NodeVec *out) {
  Token *t = tok(); int line = t->line;
  if (t->k == TK_INDENT) perr("unexpected indent");
  if (t->k == TK_OP && !strcmp(t->text, "@")) {
    NodeVec decs = {0};
    while (accept_op("@")) {
      nv_push(&decs, parse_test());
      if (T[P].k == TK_NEWLINE) P++; else perr("invalid syntax after decorator");
    }
    if (!is_kw("def") && !is_kw("class")) perr("expected 'def' or 'class' after decorator");
    int before = out->n;
    parse_stmt(out);
    Node *target = out->v[before];
    target->v2 = decs.v; target->nv2 = decs.n;
    return;
  }
  if (t->k == TK_NAME) {
    const char *s = t->text;
    if (!strcmp(s, "if")) { nv_push(out, parse_if()); return; }
    if (!strcmp(s, "while")) {
      P++; Node *n = new_node(N_WHILE, line); n->a = parse_test();
      NodeVec b = {0}; parse_block(&b); n->v = b.v; n->nv = b.n;
      if (accept_kw("else")) { NodeVec eb = {0}; parse_block(&eb); n->v2 = eb.v; n->nv2 = eb.n; }
      nv_push(out, n); return;
    }
    if (!strcmp(s, "for")) {
      P++; Node *n = new_node(N_FOR, line); n->a = parse_target_list();
      if (!assignable(n->a)) perr("cannot assign to expression");
      expect_kw("in"); n->b = parse_testlist();
      NodeVec b = {0}; parse_block(&b); n->v = b.v; n->nv = b.n;
      if (accept_kw("else")) { NodeVec eb = {0}; parse_block(&eb); n->v2 = eb.v; n->nv2 = eb.n; }
      nv_push(out, n); return;
    }
    if (!strcmp(s, "def")) { P++; parse_def(out, line); return; }
    if (!strcmp(s, "class")) {
      P++; Node *n = new_node(N_CLASS, line); n->s = expect_name(); n->b = mk_name(n->s, line);
      if (accept_op("(")) {
        NodeVec bs = {0};
        while (!is_op(")")) {
          if (tok()->k == TK_NAME && T[P + 1].k == TK_OP && !strcmp(T[P + 1].text, "=")) { P += 2; parse_test(); /* metaclass=... etc: ignored */ }
          else nv_push(&bs, parse_test());
          if (!accept_op(",")) break;
        }
        expect_op(")");
        Node *bt = new_node(N_TUPLE, line); bt->v = bs.v; bt->nv = bs.n; n->a = bt;
      }
      FuncDef *saved = cur_fd; cur_fd = NULL;
      NodeVec b = {0}; parse_block(&b); cur_fd = saved;
      n->v = b.v; n->nv = b.n; nv_push(out, n); return;
    }
    if (!strcmp(s, "try")) {
      P++; Node *n = new_node(N_TRY, line);
      NodeVec body = {0}; parse_block(&body); n->v = body.v; n->nv = body.n;
      NodeVec hs = {0};
      while (is_kw("except")) {
        Node *h = new_node(N_EXCEPT, tok()->line); P++;
        if (!is_op(":")) { h->a = parse_test(); if (accept_kw("as")) { h->s = expect_name(); h->b = mk_name(h->s, h->line); } }
        NodeVec hb = {0}; parse_block(&hb); h->v = hb.v; h->nv = hb.n; nv_push(&hs, h);
      }
      n->v2 = hs.v; n->nv2 = hs.n;
      if (is_kw("else") && hs.n) { P++; NodeVec eb = {0}; parse_block(&eb); n->c = make_block(&eb, line); }
      if (accept_kw("finally")) { NodeVec fb = {0}; parse_block(&fb); n->d = make_block(&fb, line); }
      if (!hs.n && !n->d) perr("expected 'except' or 'finally' block");
      nv_push(out, n); return;
    }
    if (!strcmp(s, "with")) {
      P++;
      Node *top = NULL, *cur = NULL;
      for (;;) {
        Node *n = new_node(N_WITH, line); n->a = parse_test();
        if (accept_kw("as")) { Str *nm = expect_name(); n->s = nm; n->b = mk_name(nm, line); }
        if (cur) { NodeVec one = {0}; nv_push(&one, n); cur->v = one.v; cur->nv = 1; } else top = n;
        cur = n;
        if (!accept_op(",")) break;
      }
      NodeVec b = {0}; parse_block(&b); cur->v = b.v; cur->nv = b.n; nv_push(out, top); return;
    }
    if (!strcmp(s, "elif") || !strcmp(s, "else") || !strcmp(s, "except") || !strcmp(s, "finally"))
      perr("invalid syntax: '%s' without matching block", s);
  }
  parse_simple_line(out);
}

Node **parse_program(const char *src, const char *file, int *n) {
  int nt;
  Token *saveT = T; int saveP = P; const char *saveF = G_file; FuncDef *saveFd = cur_fd;
  T = lex(src, file, &nt); P = 0; G_file = file; cur_fd = NULL; pdepth = 0;
  NodeVec out = {0};
  while (T[P].k != TK_EOF) {
    if (T[P].k == TK_NEWLINE) { P++; continue; }
    parse_stmt(&out);
  }
  T = saveT; P = saveP; G_file = saveF; cur_fd = saveFd;
  *n = out.n;
  resolve_program(out.v, out.n);
  return out.v;
}
