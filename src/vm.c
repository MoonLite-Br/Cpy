/* vm.c - register bytecode VM, an optional execution tier under the tree-walker.
 *
 * Idea: a function body is compiled ONCE (on its first call) from the resolved AST
 * into a flat array of 3-address instructions, then run by vm_run() in one C
 * function with a plain `switch` dispatch -- no recursive eval()/exec_block() per
 * AST node, no refcount traffic for temporaries, and fused compare-and-jump.
 *
 * Design rules that keep this safe and small:
 *
 *  - Registers ARE the function's Env slots: locals first (fd->nlocals of them,
 *    exactly the slots resolve.c assigned), then compiler temporaries appended
 *    after them (fd->vm_ntemps, reserved in env_new_func).  Closures, tracebacks,
 *    exceptions, `del`, nonlocal etc. therefore see the same state as before.
 *  - An operand is an int: >= 0 selects an Env slot, < 0 selects constant ~r.
 *  - Anything the compiler doesn't natively support is delegated to the
 *    tree-walker with VM_EVAL (expression -> temp) / VM_EXEC (whole statement,
 *    incl. try/with/def/class/import/...).  So EVERY function compiles; coverage
 *    only decides how much of it gets the speedup.  Never a semantic gap.
 *  - Generators/coroutines and lambdas are left on the tree-walker.
 *  - `CPY_NOVM=1` in the environment (or --no-vm) turns the whole tier off. */
#include "cpy.h"

enum { ST_NONE = 0, ST_BREAK, ST_CONT, ST_RET };   /* must match interp.c */

int g_vm_enabled = 1;

/* ------------------------------------------------------------------ opcodes */
enum {
  VM_MOVE,     /* a=dst b=src (never a possibly-unbound local: see LOADL)       */
  VM_LOADL,    /* a=dst b=local slot n=name node: copy with unbound check        */
  VM_LOADN,    /* a=dst n=name node: non-local name load (global/free/...)       */
  VM_STOREN,   /* a=src n=name node: non-local name store                        */
  VM_EVAL,     /* a=dst n=expr node: run the tree-walker                         */
  VM_BIN,      /* a=dst b,c=operands sub=OP_*                                    */
  VM_CMP,      /* a=dst b,c sub=C_*  (result bool)                               */
  VM_NOT,      /* a=dst b                                                        */
  VM_UNARY,    /* a=dst b sub=OP_NEG/POS/INV                                     */
  VM_JMP,      /* d=target                                                       */
  VM_JT, VM_JF,/* a=cond d=target                                                */
  VM_JCMP,     /* a,b operands sub=C_* c=polarity(jump if cmp==c) d=target       */
  VM_CALL,     /* a=dst b=callee c=arg base slot sub=nargs                       */
  VM_CALLM,    /* a=dst b=(unused) c=base slot: [obj,args...] sub=nargs n=attr   */
  VM_RET,      /* a=value                                                        */
  VM_INDEX,    /* a=dst b=obj c=idx                                              */
  VM_SETIDX,   /* a=obj b=idx c=value                                            */
  VM_ATTR,     /* a=dst b=obj n=attr node                                        */
  VM_AUG,      /* a=local slot b=rhs sub=OP_*                                    */
  VM_ASSIGN,   /* a=value n=target node (tuple/attr/... via tree-walker)         */
  VM_EXEC,     /* n=stmt b=break target(-1) c=continue target(-1)                */
  VM_FORPREP,  /* a=base of 3 temps b=iterable                                   */
  VM_FORNEXT,  /* a=base b=target local slot or -1 (then n=target) d=exit        */
  VM_CLR       /* a=temp to release                                              */
};

typedef struct { int op, sub, a, b, c, d, line; Node *n; } Ins;

typedef struct VMCode {
  Ins *code; int ncode;
  Value *consts; int nconsts;
  int nlocals, ntemps;
  FuncDef *fd;
} VMCode;

/* ----------------------------------------------------------------- compiler */
typedef struct { int *at; int n, cap; } Lbl;          /* patch list: (ins<<2)|field */
typedef struct Loop { Lbl brk; int cont; struct Loop *prev; } Loop;

typedef struct {
  FuncDef *fd;
  Ins *code; int ncode, cap;
  Value *k; int nk, kcap;
  int nlocals, ntmp, maxtmp;
  int unsafe;            /* a nested def/lambda/class/walrus can rebind our locals behind our back */
  Loop *loop;
  int line;
} C;

static void lbl_add(Lbl *l, int ins, int field) {
  if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->at = xrealloc(l->at, l->cap * sizeof(int)); }
  l->at[l->n++] = (ins << 2) | field;
}
static void patch_to(C *c, int enc, int target) {
  Ins *I = &c->code[enc >> 2];
  switch (enc & 3) { case 0: I->d = target; break; case 1: I->b = target; break; default: I->c = target; break; }
}
static void lbl_bind(C *c, Lbl *l) {
  for (int i = 0; i < l->n; i++) patch_to(c, l->at[i], c->ncode);
  free(l->at); l->at = NULL; l->n = l->cap = 0;
}

static int emit(C *c, int op, int sub, int a, int b, int cc, Node *n) {
  if (c->ncode == c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->code = xrealloc(c->code, c->cap * sizeof(Ins)); }
  Ins *I = &c->code[c->ncode];
  I->op = op; I->sub = sub; I->a = a; I->b = b; I->c = cc; I->d = -1; I->n = n;
  I->line = n ? n->line : c->line;
  return c->ncode++;
}
static int addk(C *c, Value v) {
  if (c->nk == c->kcap) { c->kcap = c->kcap ? c->kcap * 2 : 16; c->k = xrealloc(c->k, c->kcap * sizeof(Value)); }
  c->k[c->nk] = v; return c->nk++;
}
static int newtmp(C *c) {
  int r = c->nlocals + c->ntmp++;
  if (c->ntmp > c->maxtmp) c->maxtmp = c->ntmp;
  return r;
}
static int is_tmp(C *c, int r) { return r >= c->nlocals; }

/* does the subtree contain something that can rebind one of our locals behind our back? */
static int scan_unsafe(Node *n) {
  if (!n) return 0;
  switch (n->k) { case N_DEF: case N_LAMBDA: case N_CLASS: case N_WALRUS: case N_GENEXP: case N_LISTCOMP:
                  case N_DICTCOMP: case N_SETCOMP: return 1; default: break; }
  if (scan_unsafe(n->a) || scan_unsafe(n->b) || scan_unsafe(n->c) || scan_unsafe(n->d)) return 1;
  for (int i = 0; i < n->nv; i++) if (scan_unsafe(n->v[i])) return 1;
  for (int i = 0; i < n->nv2; i++) if (scan_unsafe(n->v2[i])) return 1;
  return 0;
}

static int cexpr(C *c, Node *n, int want);
static void cjump(C *c, Node *n, int when, Lbl *L);

static int mov_to(C *c, int dst, int src, Node *n) { emit(c, VM_MOVE, 0, dst, src, 0, n); return dst; }

static int ccall(C *c, Node *n, int want) {
  int nargs = n->nv;
  if (n->op != 0 || n->nv2 != 0 || nargs > 30) goto generic;
  for (int i = 0; i < nargs; i++) if (n->v[i]->k == N_STAR || n->v[i]->k == N_DSTAR) goto generic;
  {
    Node *cn = n->a;
    int mark = c->ntmp, callee = -1, base;
    if (cn->k == N_ATTR) {
      base = c->nlocals + c->ntmp;
      for (int i = 0; i < nargs + 1; i++) newtmp(c);
      cexpr(c, cn->a, base);                       /* receiver into base */
      for (int i = 0; i < nargs; i++) cexpr(c, n->v[i], base + 1 + i);
      c->ntmp = mark;
      int dst = want >= 0 ? want : newtmp(c);
      emit(c, VM_CALLM, nargs, dst, 0, base, n);    /* I->n->a is the N_ATTR: name + line */
      return dst;
    }
    callee = cexpr(c, cn, -1);
    base = c->nlocals + c->ntmp;
    for (int i = 0; i < nargs; i++) newtmp(c);
    for (int i = 0; i < nargs; i++) cexpr(c, n->v[i], base + i);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_CALL, nargs, dst, callee, base, n);
    return dst;
  }
generic: {
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_EVAL, 0, dst, 0, 0, n);
    return dst;
  }
}

static int cexpr(C *c, Node *n, int want) {
  switch (n->k) {
  case N_CONST: {
    int k = ~addk(c, n->val);
    if (want < 0) return k;
    return mov_to(c, want, k, n);
  }
  case N_NAME:
    if (n->rk == RK_LOCAL) {
      if (want < 0 && !c->unsafe) return n->ri;
      int dst = want >= 0 ? want : newtmp(c);
      emit(c, VM_LOADL, 0, dst, n->ri, 0, n);
      return dst;
    } else {
      int dst = want >= 0 ? want : newtmp(c);
      emit(c, VM_LOADN, 0, dst, 0, 0, n);
      return dst;
    }
  case N_BINOP: {
    int mark = c->ntmp;
    int ra = cexpr(c, n->a, -1), rb = cexpr(c, n->b, -1);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_BIN, n->op, dst, ra, rb, n);
    return dst;
  }
  case N_UNARY: {
    int mark = c->ntmp;
    int ra = cexpr(c, n->a, -1);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_UNARY, n->op, dst, ra, 0, n);
    return dst;
  }
  case N_NOT: {
    int mark = c->ntmp;
    int ra = cexpr(c, n->a, -1);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_NOT, 0, dst, ra, 0, n);
    return dst;
  }
  case N_CMP: {
    if (n->nv != 1) break;
    int mark = c->ntmp;
    int ra = cexpr(c, n->a, -1), rb = cexpr(c, n->v[0], -1);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_CMP, n->ops[0], dst, ra, rb, n);
    return dst;
  }
  case N_AND: case N_OR: {
    /* value-preserving short circuit: always through a fresh temp, because the
     * right operand may still read a local that `want` names */
    int mark = c->ntmp;
    int t = newtmp(c);
    cexpr(c, n->a, t);
    Lbl L = {0};
    lbl_add(&L, emit(c, n->k == N_AND ? VM_JF : VM_JT, 0, t, 0, 0, n), 0);
    cexpr(c, n->b, t);
    lbl_bind(c, &L);
    c->ntmp = mark;
    if (want < 0) { int dst = newtmp(c); if (dst != t) mov_to(c, dst, t, n); return dst; }
    return mov_to(c, want, t, n);
  }
  case N_IFEXP: {
    int mark = c->ntmp;
    int t = newtmp(c);
    Lbl Lelse = {0}, Lend = {0};
    cjump(c, n->a, 0, &Lelse);
    cexpr(c, n->b, t);
    lbl_add(&Lend, emit(c, VM_JMP, 0, 0, 0, 0, n), 0);
    lbl_bind(c, &Lelse);
    cexpr(c, n->c, t);
    lbl_bind(c, &Lend);
    c->ntmp = mark;
    if (want < 0) { int dst = newtmp(c); if (dst != t) mov_to(c, dst, t, n); return dst; }
    return mov_to(c, want, t, n);
  }
  case N_CALL: return ccall(c, n, want);
  case N_INDEX: {
    if (n->b->k == N_SLICE) break;
    int mark = c->ntmp;
    int ro = cexpr(c, n->a, -1), ri = cexpr(c, n->b, -1);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_INDEX, 0, dst, ro, ri, n);
    return dst;
  }
  case N_ATTR: {
    int mark = c->ntmp;
    int ro = cexpr(c, n->a, -1);
    c->ntmp = mark;
    int dst = want >= 0 ? want : newtmp(c);
    emit(c, VM_ATTR, 0, dst, ro, 0, n);
    return dst;
  }
  default: break;
  }
  { int dst = want >= 0 ? want : newtmp(c); emit(c, VM_EVAL, 0, dst, 0, 0, n); return dst; }
}

/* Emit code that jumps to L when truthiness(n) == when, and falls through otherwise. */
static void cjump(C *c, Node *n, int when, Lbl *L) {
  int mark = c->ntmp;
  switch (n->k) {
  case N_NOT: cjump(c, n->a, !when, L); return;
  case N_AND:
    if (!when) { cjump(c, n->a, 0, L); cjump(c, n->b, 0, L); }
    else { Lbl skip = {0}; cjump(c, n->a, 0, &skip); cjump(c, n->b, 1, L); lbl_bind(c, &skip); }
    return;
  case N_OR:
    if (when) { cjump(c, n->a, 1, L); cjump(c, n->b, 1, L); }
    else { Lbl skip = {0}; cjump(c, n->a, 1, &skip); cjump(c, n->b, 0, L); lbl_bind(c, &skip); }
    return;
  case N_CMP:
    if (n->nv == 1) {
      int ra = cexpr(c, n->a, -1), rb = cexpr(c, n->v[0], -1);
      c->ntmp = mark;
      int i = emit(c, VM_JCMP, n->ops[0], ra, rb, when, n);
      lbl_add(L, i, 0);
      return;
    }
    break;
  default: break;
  }
  int r = cexpr(c, n, -1);
  c->ntmp = mark;
  lbl_add(L, emit(c, when ? VM_JT : VM_JF, 0, r, 0, 0, n), 0);
}

static void cblock(C *c, Node **v, int n);

static void cloop_body(C *c, Node **v, int nv, Loop *lp) {
  lp->prev = c->loop; c->loop = lp;
  cblock(c, v, nv);
  c->loop = lp->prev;
}

static void cstmt(C *c, Node *n) {
  int mark = c->ntmp;
  c->line = n->line;
  switch (n->k) {
  case N_PASS: return;
  case N_EXPR: {
    int r = cexpr(c, n->a, -1);
    if (r >= 0 && is_tmp(c, r)) emit(c, VM_CLR, 0, r, 0, 0, n);
    break;
  }
  case N_ASSIGN: {
    if (n->nv == 1 && n->v[0]->k == N_NAME) {
      Node *t = n->v[0];
      if (t->rk == RK_LOCAL) cexpr(c, n->a, t->ri);
      else { int r = cexpr(c, n->a, -1); emit(c, VM_STOREN, 0, r, 0, 0, t); }
      break;
    }
    if (n->nv == 1 && n->v[0]->k == N_INDEX && n->v[0]->b->k != N_SLICE) {
      Node *t = n->v[0];
      int rv = cexpr(c, n->a, -1);
      int ro = cexpr(c, t->a, -1), ri = cexpr(c, t->b, -1);
      emit(c, VM_SETIDX, 0, ro, ri, rv, n);
      break;
    }
    int r = cexpr(c, n->a, -1);
    for (int i = 0; i < n->nv; i++) emit(c, VM_ASSIGN, 0, r, 0, 0, n->v[i]);
    break;
  }
  case N_AUG: {
    Node *t = n->a;
    if (t->k == N_NAME && t->rk == RK_LOCAL) {
      int r = cexpr(c, n->b, -1);
      emit(c, VM_AUG, n->op, t->ri, r, 0, n);
      break;
    }
    goto fallback;
  }
  case N_IF: {
    Lbl Lelse = {0}, Lend = {0};
    cjump(c, n->a, 0, &Lelse);
    c->ntmp = mark;
    cblock(c, n->v, n->nv);
    if (n->nv2) {
      lbl_add(&Lend, emit(c, VM_JMP, 0, 0, 0, 0, n), 0);
      lbl_bind(c, &Lelse);
      cblock(c, n->v2, n->nv2);
      lbl_bind(c, &Lend);
    } else lbl_bind(c, &Lelse);
    break;
  }
  case N_WHILE: {
    Loop lp = {{0}, 0, NULL};
    int top = c->ncode; lp.cont = top;
    Lbl Lexit = {0};
    cjump(c, n->a, 0, &Lexit);
    c->ntmp = mark;
    cloop_body(c, n->v, n->nv, &lp);
    emit(c, VM_JMP, 0, 0, 0, 0, n); c->code[c->ncode - 1].d = top;
    lbl_bind(c, &Lexit);                 /* normal exit -> else clause */
    if (n->nv2) cblock(c, n->v2, n->nv2);
    lbl_bind(c, &lp.brk);                /* break skips else */
    break;
  }
  case N_FOR: {
    Loop lp = {{0}, 0, NULL};
    int base = c->nlocals + c->ntmp;
    newtmp(c); newtmp(c); newtmp(c);               /* cur/iterator, stop, step: live for the whole loop */
    int rs = cexpr(c, n->b, -1);
    emit(c, VM_FORPREP, 0, base, rs, 0, n);
    int top = c->ncode; lp.cont = top;
    Node *t = n->a;
    int fn = emit(c, VM_FORNEXT, 0, base, (t->k == N_NAME && t->rk == RK_LOCAL) ? t->ri : -1, 0, t);
    Lbl Lexit = {0}; lbl_add(&Lexit, fn, 0);
    cloop_body(c, n->v, n->nv, &lp);
    emit(c, VM_JMP, 0, 0, 0, 0, n); c->code[c->ncode - 1].d = top;
    lbl_bind(c, &Lexit);
    if (n->nv2) cblock(c, n->v2, n->nv2);
    lbl_bind(c, &lp.brk);
    break;
  }
  case N_RETURN: {
    if (n->a) { int r = cexpr(c, n->a, -1); emit(c, VM_RET, 0, r, 0, 0, n); }
    else emit(c, VM_RET, 0, ~addk(c, V_none()), 0, 0, n);
    break;
  }
  case N_BREAK:
    if (c->loop) { lbl_add(&c->loop->brk, emit(c, VM_JMP, 0, 0, 0, 0, n), 0); break; }
    goto fallback;
  case N_CONTINUE:
    if (c->loop) { emit(c, VM_JMP, 0, 0, 0, 0, n); c->code[c->ncode - 1].d = c->loop->cont; break; }
    goto fallback;
  default:
  fallback: {
    int i = emit(c, VM_EXEC, 0, -1, -1, -1, n);
    if (c->loop) { lbl_add(&c->loop->brk, i, 1); c->code[i].c = c->loop->cont; }
    break;
  }
  }
  c->ntmp = mark;
}

static void cblock(C *c, Node **v, int n) { for (int i = 0; i < n; i++) cstmt(c, v[i]); }

static void vm_dump(const char *kind, const char *name, VMCode *vc, int unsafe) {
  static const char *nm[] = {"MOVE","LOADL","LOADN","STOREN","EVAL","BIN","CMP","NOT","UNARY","JMP","JT","JF","JCMP","CALL","CALLM","RET","INDEX","SETIDX","ATTR","AUG","ASSIGN","EXEC","FORPREP","FORNEXT","CLR"};
  fprintf(stderr, "== vm %s %s: %d ins, %d locals, %d temps%s\n", kind, name, vc->ncode, vc->nlocals, vc->ntemps, unsafe ? " (unsafe)" : "");
  for (int i = 0; i < vc->ncode; i++) { Ins *I = &vc->code[i]; fprintf(stderr, "  %3d %-8s sub=%-3d a=%-4d b=%-4d c=%-4d d=%-4d L%d\n", i, nm[I->op], I->sub, I->a, I->b, I->c, I->d, I->line); }
}

int vm_toplevel_ntemps(struct VMCode *vc) { return vc->ntemps; }

void vm_compile(FuncDef *fd) {
  fd->vm_state = 2;
  if (!g_vm_enabled) return;
  if (fd->is_gen || fd->is_async || fd->expr || !fd->body) return;
  C c; memset(&c, 0, sizeof c);
  c.fd = fd; c.nlocals = fd->nlocals; c.line = fd->line;
  for (int i = 0; i < fd->nbody; i++) if (scan_unsafe(fd->body[i])) { c.unsafe = 1; break; }
  cblock(&c, fd->body, fd->nbody);
  emit(&c, VM_RET, 0, ~addk(&c, V_none()), 0, 0, NULL);
  VMCode *vc = xmalloc(sizeof *vc);
  vc->code = c.code; vc->ncode = c.ncode; vc->consts = c.k; vc->nconsts = c.nk; vc->nlocals = c.nlocals; vc->ntemps = c.maxtmp; vc->fd = fd;
  fd->vm = vc; fd->vm_ntemps = c.maxtmp; fd->vm_state = 1;
  if (getenv("CPY_VMDUMP")) vm_dump("fn", fd->name ? fd->name->s : "?", vc, c.unsafe);
}

/* Does this subtree create something (a function, a class) whose closure
 * would persist beyond the statement that created it, capturing whatever
 * Env it's compiled with? Comprehensions/genexps also create a nested Env
 * via the same env_new_func mechanism, but theirs is used and discarded
 * within the same expression (see N_LISTCOMP/N_GENEXP in interp.c: the
 * comprehension env is odec'd right after it runs) -- they don't escape,
 * so they're NOT included here. def/lambda/class matter because whatever
 * Env compiles them becomes their permanent closure. This only matters
 * for module-level compilation (vm_compile_toplevel): inside an ordinary
 * function body this concern doesn't apply, because that function's own
 * Env is exactly what SHOULD be captured -- there, scan_unsafe (a
 * different, unrelated check) governs a narrower LOADL fast-path instead. */
static int scan_escapes(Node *n) {
  if (!n) return 0;
  switch (n->k) { case N_DEF: case N_LAMBDA: case N_CLASS: return 1; default: break; }
  if (scan_escapes(n->a) || scan_escapes(n->b) || scan_escapes(n->c) || scan_escapes(n->d)) return 1;
  for (int i = 0; i < n->nv; i++) if (scan_escapes(n->v[i])) return 1;
  for (int i = 0; i < n->nv2; i++) if (scan_escapes(n->v2[i])) return 1;
  return 0;
}

/* Module-/script-level code has no local *slots* at all (resolve.c gives
 * every top-level name RK_GLOBAL, dict-based) -- but it still has
 * control flow and expressions, which is what this compiles: every name
 * access falls through cexpr/cstmt's existing RK_GLOBAL path (VM_LOADN/
 * VM_STOREN) exactly like a global reference inside a function body
 * already does, while loops/branches/arithmetic get the same speedup as
 * in a function. Returns NULL (caller keeps using the tree-walker,
 * unchanged) whenever this isn't safe or isn't enabled. */
VMCode *vm_compile_toplevel(Node **prog, int n) {
  if (!g_vm_enabled) return NULL;
  for (int i = 0; i < n; i++) if (scan_escapes(prog[i])) return NULL;
  C c; memset(&c, 0, sizeof c);
  c.fd = NULL; c.nlocals = 0; c.line = 0;
  cblock(&c, prog, n);
  emit(&c, VM_RET, 0, ~addk(&c, V_none()), 0, 0, NULL);
  VMCode *vc = xmalloc(sizeof *vc);
  vc->code = c.code; vc->ncode = c.ncode; vc->consts = c.k; vc->nconsts = c.nk; vc->nlocals = 0; vc->ntemps = c.maxtmp; vc->fd = NULL;
  if (getenv("CPY_VMDUMP")) vm_dump("toplevel", "<module>", vc, 0);
  return vc;
}

/* ------------------------------------------------------------------ runtime */
static void __attribute__((noinline, noreturn)) vm_unbound(VMCode *vc, const Ins *I, int r) {
  g_line = I->line;
  throw_error("UnboundLocalError", "cannot access local variable '%s' where it is not associated with a value",
              r >= 0 && r < vc->nlocals ? vc->fd->locals[r]->s : "?");
}

#define RD(r)  ((r) >= 0 ? S[r] : K[~(r)])
#define RDU(r) ({ Value _v = RD(r); if (_v.t == T_UNDEF) vm_unbound(vc, I, (r)); _v; })
#define SETR(r, v) do { Value _n = (v); Value *_p = &S[(r)]; Value _o = *_p; *_p = _n; decref(_o); } while (0)

int vm_run(VMCode *vc, Env *env) {
  Value *S = env->slots; const Value *K = vc->consts; const Ins *code = vc->code; const Ins *ip = code;
  for (;;) {
    const Ins *I = ip++;
    switch (I->op) {
    case VM_MOVE: { Value v = RD(I->b); incref(v); SETR(I->a, v); break; }
    case VM_LOADL: {
      Value v = S[I->b];
      if (v.t == T_UNDEF) { g_line = I->line; Value r = vm_load_name(I->n, env); SETR(I->a, r); break; }
      incref(v); SETR(I->a, v); break;
    }
    case VM_LOADN: {
      Node *nn = I->n;
      if (nn->rk == RK_GLOBAL) {           /* inline the cached global-dict lookup (same hint as load_from_dict) */
        Dict *d = env->globals->vars; int h = nn->gidx;
        if (h < d->n && d->e[h].key.t == T_STR && d->e[h].key.o == (Obj *)nn->s && d->e[h].val.t != T_UNDEF) {
          Value v = d->e[h].val; incref(v); SETR(I->a, v); break;
        }
      }
      g_line = I->line; Value r = vm_load_name(nn, env); SETR(I->a, r); break;
    }
    case VM_STOREN: { Value v = RDU(I->a); g_line = I->line; vm_store_name(I->n, v, env); break; }
    case VM_EVAL: { Value r = eval(I->n, env); SETR(I->a, r); break; }
    case VM_BIN: {
      Value a = RD(I->b), b = RD(I->c);
      if (a.t == T_INT && b.t == T_INT) {
        int64_t r;
        switch (I->sub) {
        case OP_ADD: if (!__builtin_add_overflow(a.i, b.i, &r)) { SETR(I->a, V_int(r)); goto next; } break;
        case OP_SUB: if (!__builtin_sub_overflow(a.i, b.i, &r)) { SETR(I->a, V_int(r)); goto next; } break;
        case OP_MUL: if (!__builtin_mul_overflow(a.i, b.i, &r)) { SETR(I->a, V_int(r)); goto next; } break;
        case OP_MOD: if (b.i > 0 && a.i >= 0) { SETR(I->a, V_int(a.i % b.i)); goto next; } break;
        case OP_FDIV: if (b.i > 0 && a.i >= 0) { SETR(I->a, V_int(a.i / b.i)); goto next; } break;
        default: break;
        }
      } else if ((a.t == T_FLOAT && (b.t == T_FLOAT || b.t == T_INT)) || (a.t == T_INT && b.t == T_FLOAT)) {
        double x = a.t == T_FLOAT ? a.d : (double)a.i, y = b.t == T_FLOAT ? b.d : (double)b.i;
        switch (I->sub) {
        case OP_ADD: SETR(I->a, V_float(x + y)); goto next;
        case OP_SUB: SETR(I->a, V_float(x - y)); goto next;
        case OP_MUL: SETR(I->a, V_float(x * y)); goto next;
        case OP_DIV: if (y != 0) { SETR(I->a, V_float(x / y)); goto next; } break;
        default: break;
        }
      }
      if (a.t == T_UNDEF) vm_unbound(vc, I, I->b);
      if (b.t == T_UNDEF) vm_unbound(vc, I, I->c);
      g_line = I->line;
      Value r = binop(I->sub, a, b);
      SETR(I->a, r);
      break;
    }
    case VM_CMP: {
      Value a = RD(I->b), b = RD(I->c);
      if (a.t == T_INT && b.t == T_INT) {
        int r;
        switch (I->sub) {
        case C_EQ: r = a.i == b.i; break; case C_NE: r = a.i != b.i; break;
        case C_LT: r = a.i < b.i; break;  case C_LE: r = a.i <= b.i; break;
        case C_GT: r = a.i > b.i; break;  case C_GE: r = a.i >= b.i; break;
        default: goto cmp_slow;
        }
        SETR(I->a, V_bool(r)); break;
      }
    cmp_slow:
      if (a.t == T_UNDEF) vm_unbound(vc, I, I->b);
      if (b.t == T_UNDEF) vm_unbound(vc, I, I->c);
      g_line = I->line;
      { int r = vm_cmp(I->sub, a, b); SETR(I->a, V_bool(r)); }
      break;
    }
    case VM_NOT: { Value a = RDU(I->b); int t = val_truthy(a); SETR(I->a, V_bool(!t)); break; }
    case VM_UNARY: {
      Value a = RD(I->b);
      if (I->sub == OP_NEG && a.t == T_INT && a.i != INT64_MIN) { SETR(I->a, V_int(-a.i)); break; }
      if (I->sub == OP_NEG && a.t == T_FLOAT) { SETR(I->a, V_float(-a.d)); break; }
      if (a.t == T_UNDEF) vm_unbound(vc, I, I->b);
      g_line = I->line;
      Value r = vm_unary(I->sub, a); SETR(I->a, r); break;
    }
    case VM_JMP: ip = code + I->d; break;
    case VM_JT: case VM_JF: {
      Value a = RDU(I->a); int t;
      if (a.t == T_BOOL || a.t == T_INT) t = a.i != 0; else t = val_truthy(a);
      if (t == (I->op == VM_JT)) ip = code + I->d;
      break;
    }
    case VM_JCMP: {
      Value a = RD(I->a), b = RD(I->b); int r;
      if (a.t == T_INT && b.t == T_INT) {
        switch (I->sub) {
        case C_EQ: r = a.i == b.i; break; case C_NE: r = a.i != b.i; break;
        case C_LT: r = a.i < b.i; break;  case C_LE: r = a.i <= b.i; break;
        case C_GT: r = a.i > b.i; break;  case C_GE: r = a.i >= b.i; break;
        default: goto jcmp_slow;
        }
      } else if (a.t == T_FLOAT && b.t == T_FLOAT) {
        switch (I->sub) {
        case C_EQ: r = a.d == b.d; break; case C_NE: r = a.d != b.d; break;
        case C_LT: r = a.d < b.d; break;  case C_LE: r = a.d <= b.d; break;
        case C_GT: r = a.d > b.d; break;  case C_GE: r = a.d >= b.d; break;
        default: goto jcmp_slow;
        }
      } else {
      jcmp_slow:
        if (a.t == T_UNDEF) vm_unbound(vc, I, I->a);
        if (b.t == T_UNDEF) vm_unbound(vc, I, I->b);
        g_line = I->line;
        r = vm_cmp(I->sub, a, b) ? 1 : 0;
      }
      if (r == I->c) ip = code + I->d;
      break;
    }
    case VM_CALL: {
      Value f = RDU(I->b); Value *argv = &S[I->c]; int na = I->sub;
      g_line = I->line;
      Value r = f.t == T_FUNC ? vm_call_func((Func *)f.o, argv, na) : call_value(f, argv, na, NULL);
      for (int i = 0; i < na; i++) if (IS_OBJ(argv[i])) { Value o = argv[i]; argv[i] = V_undef(); decref(o); }
      SETR(I->a, r);
      break;
    }
    case VM_CALLM: {
      Node *cn = I->n->a; Value *argv = &S[I->c]; int na = I->sub;
      Value obj = argv[0]; Value f, r;
      g_line = cn->line;
      Value m = vm_method_fn(obj, cn->s);
      if (m.t != T_UNDEF) {
        incref(m); g_line = I->line;
        r = m.t == T_FUNC ? vm_call_func((Func *)m.o, argv, na + 1) : call_value(m, argv, na + 1, NULL);
        decref(m);
      } else {
        f = get_attr(obj, cn->s); g_line = I->line;
        r = f.t == T_FUNC ? vm_call_func((Func *)f.o, argv + 1, na) : call_value(f, argv + 1, na, NULL);
        decref(f);
      }
      for (int i = 0; i <= na; i++) if (IS_OBJ(argv[i])) { Value o = argv[i]; argv[i] = V_undef(); decref(o); }
      SETR(I->a, r);
      break;
    }
    case VM_RET: { Value v = RDU(I->a); incref(v); g_ret_set(v); return ST_RET; }
    case VM_INDEX: {
      Value o = RD(I->b), i = RD(I->c);
      if (o.t == T_LIST && i.t == T_INT) {
        List *l = (List *)o.o; int64_t k = i.i;
        if (k < 0) k += l->len;
        if (k >= 0 && k < l->len) { Value v = l->items[k]; incref(v); SETR(I->a, v); break; }
      }
      if (o.t == T_UNDEF) vm_unbound(vc, I, I->b);
      if (i.t == T_UNDEF) vm_unbound(vc, I, I->c);
      g_line = I->line;
      Value r = index_get(o, i); SETR(I->a, r); break;
    }
    case VM_SETIDX: {
      Value o = RD(I->a), i = RD(I->b), v = RD(I->c);
      if (o.t == T_LIST && i.t == T_INT) {
        List *l = (List *)o.o; int64_t k = i.i;
        if (k < 0) k += l->len;
        if (k >= 0 && k < l->len) { Value old = l->items[k]; incref(v); l->items[k] = v; decref(old); break; }
      }
      if (o.t == T_UNDEF) vm_unbound(vc, I, I->a);
      if (i.t == T_UNDEF) vm_unbound(vc, I, I->b);
      if (v.t == T_UNDEF) vm_unbound(vc, I, I->c);
      g_line = I->line;
      incref(o); incref(i); incref(v);
      vm_index_set(o, i, v);
      decref(o); decref(i); decref(v);
      break;
    }
    case VM_ATTR: {
      Value o = RDU(I->b); g_line = I->line;
      incref(o); Value r = get_attr(o, I->n->s); decref(o);
      SETR(I->a, r); break;
    }
    case VM_AUG: {
      int dst = I->a; Value cur = S[dst], rhs = RD(I->b);
      if (cur.t == T_INT && rhs.t == T_INT) {
        int64_t r;
        switch (I->sub) {
        case OP_ADD: if (!__builtin_add_overflow(cur.i, rhs.i, &r)) { S[dst].i = r; goto next; } break;
        case OP_SUB: if (!__builtin_sub_overflow(cur.i, rhs.i, &r)) { S[dst].i = r; goto next; } break;
        case OP_MUL: if (!__builtin_mul_overflow(cur.i, rhs.i, &r)) { S[dst].i = r; goto next; } break;
        default: break;
        }
      } else if (cur.t == T_FLOAT && (rhs.t == T_FLOAT || rhs.t == T_INT)) {
        double y = rhs.t == T_FLOAT ? rhs.d : (double)rhs.i;
        switch (I->sub) {
        case OP_ADD: S[dst].d = cur.d + y; goto next;
        case OP_SUB: S[dst].d = cur.d - y; goto next;
        case OP_MUL: S[dst].d = cur.d * y; goto next;
        default: break;
        }
      }
      if (cur.t == T_UNDEF) vm_unbound(vc, I, dst);
      if (rhs.t == T_UNDEF) vm_unbound(vc, I, I->b);
      g_line = I->line;
      if (I->sub == OP_ADD && cur.t == T_STR && rhs.t == T_STR && cur.o->rc == 1 && rhs.o != cur.o) {
        /* sole owner of the left string: grow it in place so s += x loops stay O(n) */
        Str *s0 = (Str *)cur.o, *r0 = (Str *)rhs.o;
        Str *ns = xrealloc(s0, sizeof(Str) + s0->len + r0->len + 1);
        memcpy(ns->s + ns->len, r0->s, r0->len); ns->len += r0->len; ns->s[ns->len] = 0;
        ns->ascii = ns->ascii && r0->ascii; ns->cplen += r0->cplen; ns->hash = 0;
        S[dst].o = (Obj *)ns; ns->h.rc = 1;
        break;
      }
      incref(cur); incref(rhs);
      Value res = vm_aug(I->sub, cur, rhs);
      Value old = S[dst]; S[dst] = res; decref(old);
      decref(cur); decref(rhs);
      break;
    }
    case VM_ASSIGN: {
      Value v = RDU(I->a); g_line = I->line;
      incref(v); vm_assign(I->n, v, env); decref(v);
      break;
    }
    case VM_EXEC: {
      int st = vm_exec(I->n, env);
      if (st == ST_RET) return ST_RET;
      if (st == ST_BREAK && I->b >= 0) ip = code + I->b;
      else if (st == ST_CONT && I->c >= 0) ip = code + I->c;
      break;
    }
    case VM_FORPREP: {
      int base = I->a; Value v = RDU(I->b);
      if (v.t == T_RANGE) {
        Range *r = (Range *)v.o;
        SETR(base, V_int(r->start)); SETR(base + 1, V_int(r->stop)); SETR(base + 2, V_int(r->step));
      } else {
        g_line = I->line;
        incref(v); Value it = make_iterator(v); decref(v);
        SETR(base, it);
      }
      break;
    }
    case VM_FORNEXT: {
      int base = I->a; Value cur = S[base], x;
      if (cur.t == T_INT) {
        int64_t stp = S[base + 2].i, stop = S[base + 1].i, cv = cur.i;
        if (!(stp > 0 ? cv < stop : cv > stop)) { ip = code + I->d; break; }
        S[base].i = cv + stp;
        x = V_int(cv);
      } else {
        g_line = I->line;
        if (!iter_step(cur, &x)) { ip = code + I->d; break; }
      }
      if (I->b >= 0) { Value old = S[I->b]; S[I->b] = x; decref(old); }
      else { g_line = I->line; vm_assign(I->n, x, env); decref(x); }
      break;
    }
    case VM_CLR: { Value o = S[I->a]; S[I->a] = V_undef(); decref(o); break; }
    default: throw_error("SystemError", "bad VM opcode %d", I->op);
    }
  next: ;
  }
}
