/* resolve.c - static scope analysis: turns variable names into slots / globals.
 *
 * After parsing, every N_NAME node is annotated with how to reach its variable:
 *   RK_LOCAL  slot ri of the current function environment
 *   RK_FREE   slot ri of the environment rh parents up (closure)
 *   RK_GLOBAL entry in the module dict of the running function (then builtins)
 *   RK_DYN    module-level code: entry in the current (module) dict, then builtins
 *   RK_CLASS  class body: class dict first, then the fallback (rk2/ri2/rh2)
 */
#include "cpy.h"

enum { SC_MODULE, SC_FUNC, SC_CLASS, SC_COMP };
typedef struct Scope {
  int kind; FuncDef *fd;
  Str **names; int n, cap;
  struct Scope *parent;
} Scope;

static int scope_find(Scope *sc, Str *s) {
  for (int i = 0; i < sc->n; i++) if (sc->names[i] == s) return i;
  return -1;
}
static int scope_add(Scope *sc, Str *s) {
  int i = scope_find(sc, s);
  if (i >= 0) return i;
  if (sc->n >= sc->cap) { sc->cap = sc->cap ? sc->cap * 2 : 8; sc->names = xrealloc(sc->names, sc->cap * sizeof(Str *)); }
  sc->names[sc->n] = s;
  return sc->n++;
}
static int in_list(Str **l, int n, Str *s) { for (int i = 0; i < n; i++) if (l[i] == s) return 1; return 0; }

/* ------------------------------------------------- local collection */
static void collect(Node *n, Scope *sc);
static void bind(Node *t, Scope *sc) {
  if (!t) return;
  if (t->k == N_NAME) {
    FuncDef *fd = sc->fd;
    if (fd && (in_list(fd->globals, fd->nglobals, t->s) || in_list(fd->nonlocals, fd->nnonlocals, t->s))) return;
    scope_add(sc, t->s);
  } else if (t->k == N_TUPLE || t->k == N_LIST) for (int i = 0; i < t->nv; i++) bind(t->v[i], sc);
}
static void collect_list(Node **v, int n, Scope *sc) { for (int i = 0; i < n; i++) collect(v[i], sc); }
static void collect(Node *n, Scope *sc) {
  if (!n) return;
  switch (n->k) {
  case N_ASSIGN: for (int i = 0; i < n->nv; i++) bind(n->v[i], sc); collect(n->a, sc); return;
  case N_AUG: bind(n->a, sc); collect(n->b, sc); return;
  case N_FOR: bind(n->a, sc); collect(n->b, sc); collect_list(n->v, n->nv, sc); collect_list(n->v2, n->nv2, sc); return;
  case N_WITH: bind(n->b, sc); collect(n->a, sc); collect_list(n->v, n->nv, sc); return;
  case N_EXCEPT: bind(n->b, sc); collect(n->a, sc); collect_list(n->v, n->nv, sc); return;
  case N_IMPORT: bind(n->a, sc); return;
  case N_FROM: for (int i = 0; i < n->nv; i++) bind(n->v[i], sc); return;
  case N_DEF: bind(n->a, sc); return;
  case N_CLASS: bind(n->b, sc); return;
  case N_DEL: bind(n->a, sc); return;
  case N_ANNASSIGN: bind(n->a, sc); collect(n->b, sc); return;
  case N_WALRUS: bind(n->a, sc); collect(n->b, sc); return;
  case N_LAMBDA: case N_LISTCOMP: case N_DICTCOMP: case N_SETCOMP: case N_GENEXP: return;
  default:
    collect(n->a, sc); collect(n->b, sc); collect(n->c, sc); collect(n->d, sc);
    collect_list(n->v, n->nv, sc); collect_list(n->v2, n->nv2, sc);
  }
}

/* ------------------------------------------------- name resolution */
static void resolve_outer(int *rk, int *ri, int *rh, Str *s, Scope *from, int hops, int class_body) {
  for (Scope *p = from; p; p = p->parent) {
    if (p->kind == SC_CLASS) { if (class_body) hops++; continue; }
    if (p->kind == SC_MODULE) { *rk = RK_GLOBAL; return; }
    FuncDef *fd = p->fd;
    if (fd && in_list(fd->globals, fd->nglobals, s)) { *rk = RK_GLOBAL; return; }
    int i = scope_find(p, s);
    if (i >= 0) { *rk = RK_FREE; *ri = i; *rh = hops; return; }
    hops++;
  }
  *rk = RK_GLOBAL;
}

static void resolve_name(Node *n, Scope *sc) {
  Str *s = n->s;
  switch (sc->kind) {
  case SC_MODULE: n->rk = RK_DYN; return;
  case SC_CLASS:
    n->rk = RK_CLASS;
    resolve_outer(&n->rk2, &n->ri2, &n->rh2, s, sc->parent, 1, 1);
    return;
  default: {
    FuncDef *fd = sc->fd;
    if (fd && in_list(fd->globals, fd->nglobals, s)) { n->rk = RK_GLOBAL; return; }
    if (!(fd && in_list(fd->nonlocals, fd->nnonlocals, s))) {
      int i = scope_find(sc, s);
      if (i >= 0) { n->rk = RK_LOCAL; n->ri = i; return; }
    }
    resolve_outer(&n->rk, &n->ri, &n->rh, s, sc->parent, 1, 0);
  }
  }
}

static void rn(Node *n, Scope *sc);
static void rlist(Node **v, int n, Scope *sc) { for (int i = 0; i < n; i++) rn(v[i], sc); }

static void resolve_func(FuncDef *fd, Scope *outer) {
  for (int i = 0; i < fd->nparams; i++) if (fd->defaults[i]) rn(fd->defaults[i], outer);
  Scope sc = {SC_FUNC, fd, NULL, 0, 0, outer};
  for (int i = 0; i < fd->nparams; i++) scope_add(&sc, fd->params[i]);
  if (fd->expr) { collect(fd->expr, &sc); rn(fd->expr, &sc); }
  else { collect_list(fd->body, fd->nbody, &sc); rlist(fd->body, fd->nbody, &sc); }
  fd->locals = sc.names; fd->nlocals = sc.n;
}

static void resolve_comp(Node *n, Scope *outer) {
  FuncDef *cf = xcalloc(1, sizeof(FuncDef));
  cf->name = INTERN("<comprehension>"); cf->star = -1;
  n->fd = cf;
  Scope sc = {SC_COMP, cf, NULL, 0, 0, outer};
  for (int i = 0; i < n->nv; i++) if (n->v[i]->k == N_COMPFOR) bind(n->v[i]->a, &sc);
  for (int i = 0; i < n->nv; i++) {
    Node *c = n->v[i];
    if (c->k == N_COMPFOR) { rn(c->b, i == 0 ? outer : &sc); rn(c->a, &sc); }
    else rn(c->a, &sc);
  }
  rn(n->a, &sc);
  if (n->b) rn(n->b, &sc);
  cf->locals = sc.names; cf->nlocals = sc.n;
}

static void rn(Node *n, Scope *sc) {
  if (!n) return;
  switch (n->k) {
  case N_NAME: resolve_name(n, sc); return;
  case N_CONST: return;
  case N_LAMBDA: resolve_func(n->fd, sc); return;
  case N_DEF: rn(n->a, sc); rlist(n->v2, n->nv2, sc); resolve_func(n->fd, sc); return;
  case N_CLASS: {
    rn(n->a, sc); rn(n->b, sc); rlist(n->v2, n->nv2, sc);
    Scope cs = {SC_CLASS, NULL, NULL, 0, 0, sc};
    rlist(n->v, n->nv, &cs);
    return;
  }
  case N_LISTCOMP: case N_DICTCOMP: case N_SETCOMP: case N_GENEXP: resolve_comp(n, sc); return;
  case N_LIST: case N_TUPLE: case N_SET:
    for (int i = 0; i < n->nv; i++) if (n->v[i] && n->v[i]->k == N_STAR) n->op = 1;
    rlist(n->v, n->nv, sc); return;
  default:
    rn(n->a, sc); rn(n->b, sc); rn(n->c, sc); rn(n->d, sc);
    rlist(n->v, n->nv, sc); rlist(n->v2, n->nv2, sc);
  }
}

void resolve_program(Node **prog, int n) {
  Scope m = {SC_MODULE, NULL, NULL, 0, 0, NULL};
  rlist(prog, n, &m);
}
