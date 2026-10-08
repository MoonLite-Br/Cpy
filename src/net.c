/* net.c - a real, POSIX-backed `socket` module (modeled on CPython's
 * Modules/socketmodule.c, trimmed to the parts most scripts actually use)
 * plus the module glue for the pure-cpy lib/http_client.cpy on top of it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include "cpy.h"

#define ARGS Value *a, int n, Kw *kw
#define UNUSED (void)a; (void)n; (void)kw

static Value make_sock(int fd, int family, int type_) {
  Sock *s = xmalloc(sizeof(Sock));
  s->h.rc = 1; s->h.type = T_SOCK;
  s->fd = fd; s->family = family; s->type_ = type_; s->closed = 0; s->timeout = -1;
  return V_obj(s, T_SOCK);
}
void net_free(Obj *o) {
  Sock *s = (Sock *)o;
  if (!s->closed && s->fd >= 0) close(s->fd);
  free(s);
}
static Sock *SELF(Value v) { return (Sock *)v.o; }
int sock_get_fd(Value v) { return ((Sock *)v.o)->fd; }
static void check_open(Sock *s) { if (s->closed) throw_error("OSError", "[Errno 9] Bad file descriptor (socket is closed)"); }

static void raise_errno(const char *what) {
  int e = errno;
  const char *cls = "OSError";
  if (e == ETIMEDOUT) cls = "TimeoutError";
  else if (e == ECONNREFUSED) cls = "ConnectionRefusedError";
  else if (e == ECONNRESET) cls = "ConnectionResetError";
  throw_error(cls, "[Errno %d] %s: %s", e, strerror(e), what);
}

/* ------------------------------------------------------------ address helpers */
static Value addr_to_tuple(struct sockaddr *sa) {
  char host[NI_MAXHOST]; int port = 0;
  if (sa->sa_family == AF_INET) {
    struct sockaddr_in *a4 = (struct sockaddr_in *)sa;
    inet_ntop(AF_INET, &a4->sin_addr, host, sizeof host); port = ntohs(a4->sin_port);
  } else if (sa->sa_family == AF_INET6) {
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)sa;
    inet_ntop(AF_INET6, &a6->sin6_addr, host, sizeof host); port = ntohs(a6->sin6_port);
  } else { snprintf(host, sizeof host, "?"); }
  Value it[2] = {V_str(host), V_int(port)};
  Value t = V_tuple(it, 2); decref(it[0]); return t;
}

/* resolve (host, port) from a cpy tuple into a sockaddr; tries each
 * candidate address getaddrinfo returns until one is accepted by `use`. */
typedef int (*AddrUseFn)(int fd, struct sockaddr *sa, socklen_t len, double timeout, int *out_errno, int *out_timeout);

static int do_connect(int fd, struct sockaddr *sa, socklen_t len, double timeout, int *out_errno, int *out_timeout) {
  *out_timeout = 0;
  if (timeout < 0) { if (connect(fd, sa, len) == 0) return 1; *out_errno = errno; return 0; }
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = connect(fd, sa, len);
  if (r == 0) { fcntl(fd, F_SETFL, flags); return 1; }
  if (errno != EINPROGRESS) { *out_errno = errno; fcntl(fd, F_SETFL, flags); return 0; }
  fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
  struct timeval tv; tv.tv_sec = (time_t)timeout; tv.tv_usec = (long)((timeout - (double)(time_t)timeout) * 1e6);
  int sr = select(fd + 1, NULL, &wf, NULL, &tv);
  fcntl(fd, F_SETFL, flags);
  if (sr == 0) { *out_timeout = 1; return 0; }
  if (sr < 0) { *out_errno = errno; return 0; }
  int err = 0; socklen_t elen = sizeof err;
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
  if (err != 0) { *out_errno = err; return 0; }
  return 1;
}

static void resolve_and_use(Sock *s, Str *host, int64_t port, AddrUseFn use, const char *verb) {
  char portstr[16]; snprintf(portstr, sizeof portstr, "%lld", (long long)port);
  struct addrinfo hints; memset(&hints, 0, sizeof hints);
  hints.ai_family = s->family; hints.ai_socktype = s->type_;
  struct addrinfo *res = NULL;
  int gai = getaddrinfo(host->len ? host->s : NULL, portstr, &hints, &res);
  if (gai != 0) throw_error("OSError", "[Errno -2] %s: %s", gai_strerror(gai), host->s);
  int last_errno = 0, any_timeout = 0, ok = 0;
  for (struct addrinfo *p = res; p; p = p->ai_next) {
    int e = 0, to = 0;
    if (use(s->fd, p->ai_addr, p->ai_addrlen, s->timeout, &e, &to)) { ok = 1; break; }
    last_errno = e; any_timeout = any_timeout || to;
  }
  freeaddrinfo(res);
  if (!ok) {
    if (any_timeout) throw_error("TimeoutError", "timed out");
    if (last_errno) {
      const char *cls = last_errno == ECONNREFUSED ? "ConnectionRefusedError" : last_errno == ECONNRESET ? "ConnectionResetError" : "OSError";
      throw_error(cls, "[Errno %d] %s: %s (%s)", last_errno, strerror(last_errno), host->s, verb);
    }
    throw_error("OSError", "%s: no address could be resolved for %s", verb, host->s);
  }
}

static void unpack_addr(Value v, Str **host, int64_t *port) {
  if (v.t != T_TUPLE || ((List *)v.o)->len != 2) throw_error("TypeError", "address must be a (host, port) tuple");
  List *l = (List *)v.o;
  *host = need_str(l->items[0], "host");
  *port = need_int(l->items[1], "port");
}

/* --------------------------------------------------------------- methods */
static Value sk_connect(ARGS) {
  UNUSED; chk("connect", n, 2, 2);
  Sock *s = SELF(a[0]); check_open(s);
  Str *host; int64_t port; unpack_addr(a[1], &host, &port);
  resolve_and_use(s, host, port, do_connect, "connect");
  return V_none();
}
static Value sk_connect_ex(ARGS) {
  UNUSED; chk("connect_ex", n, 2, 2);
  Sock *s = SELF(a[0]); check_open(s);
  Str *host; int64_t port; unpack_addr(a[1], &host, &port);
  char portstr[16]; snprintf(portstr, sizeof portstr, "%lld", (long long)port);
  struct addrinfo hints; memset(&hints, 0, sizeof hints);
  hints.ai_family = s->family; hints.ai_socktype = s->type_;
  struct addrinfo *res = NULL;
  if (getaddrinfo(host->s, portstr, &hints, &res) != 0) return V_int(-2);
  int e = 0, to = 0, ok = 0;
  for (struct addrinfo *p = res; p; p = p->ai_next) if (do_connect(s->fd, p->ai_addr, p->ai_addrlen, s->timeout, &e, &to)) { ok = 1; break; }
  freeaddrinfo(res);
  return V_int(ok ? 0 : (to ? ETIMEDOUT : e));
}
static Value sk_bind(ARGS) {
  UNUSED; chk("bind", n, 2, 2);
  Sock *s = SELF(a[0]); check_open(s);
  Str *host; int64_t port; unpack_addr(a[1], &host, &port);
  char portstr[16]; snprintf(portstr, sizeof portstr, "%lld", (long long)port);
  struct addrinfo hints; memset(&hints, 0, sizeof hints);
  hints.ai_family = s->family; hints.ai_socktype = s->type_; hints.ai_flags = AI_PASSIVE;
  struct addrinfo *res = NULL;
  int gai = getaddrinfo(host->len ? host->s : NULL, portstr, &hints, &res);
  if (gai != 0) throw_error("OSError", "[Errno -2] %s", gai_strerror(gai));
  int ok = 0, e = 0;
  for (struct addrinfo *p = res; p; p = p->ai_next) if (bind(s->fd, p->ai_addr, p->ai_addrlen) == 0) { ok = 1; break; } else e = errno;
  freeaddrinfo(res);
  if (!ok) throw_error("OSError", "[Errno %d] %s", e, strerror(e));
  return V_none();
}
static Value sk_listen(ARGS) {
  UNUSED; chk("listen", n, 1, 2);
  Sock *s = SELF(a[0]); check_open(s);
  int backlog = n == 2 ? (int)need_int(a[1], "backlog") : 128;
  if (listen(s->fd, backlog) != 0) raise_errno("listen");
  return V_none();
}
static Value sk_accept(ARGS) {
  UNUSED; chk("accept", n, 1, 1);
  Sock *s = SELF(a[0]); check_open(s);
  struct sockaddr_storage ss; socklen_t sl = sizeof ss;
  if (s->timeout >= 0) {
    fd_set rf; FD_ZERO(&rf); FD_SET(s->fd, &rf);
    struct timeval tv; tv.tv_sec = (time_t)s->timeout; tv.tv_usec = (long)((s->timeout - (double)(time_t)s->timeout) * 1e6);
    int sr = select(s->fd + 1, &rf, NULL, NULL, &tv);
    if (sr == 0) throw_error("TimeoutError", "timed out");
    if (sr < 0) raise_errno("accept");
  }
  int cfd = accept(s->fd, (struct sockaddr *)&ss, &sl);
  if (cfd < 0) raise_errno("accept");
  Value conn = make_sock(cfd, s->family, s->type_);
  Value addr = addr_to_tuple((struct sockaddr *)&ss);
  Value it[2] = {conn, addr}; Value t = V_tuple(it, 2); decref(conn); decref(addr);
  return t;
}
static ssize_t recv_with_timeout(Sock *s, char *buf, size_t len, int flags) {
  if (s->timeout >= 0) {
    fd_set rf; FD_ZERO(&rf); FD_SET(s->fd, &rf);
    struct timeval tv; tv.tv_sec = (time_t)s->timeout; tv.tv_usec = (long)((s->timeout - (double)(time_t)s->timeout) * 1e6);
    int sr = select(s->fd + 1, &rf, NULL, NULL, &tv);
    if (sr == 0) { errno = ETIMEDOUT; return -1; }
    if (sr < 0) return -1;
  }
  return recv(s->fd, buf, len, flags);
}
static Value sk_recv(ARGS) {
  UNUSED; chk("recv", n, 2, 3);
  Sock *s = SELF(a[0]); check_open(s);
  int64_t bufsize = need_int(a[1], "bufsize");
  if (bufsize < 0 || bufsize > (64 << 20)) throw_error("ValueError", "recv() buffer size out of range");
  char *buf = xmalloc((size_t)bufsize + 1);
  ssize_t r = recv_with_timeout(s, buf, (size_t)bufsize, 0);
  if (r < 0) { free(buf); raise_errno("recv"); }
  Value v = V_strn(buf, (int)r); free(buf); return v;
}
static Value sk_recv_into_str(ARGS) { return sk_recv(a, n, kw); }
static Value sk_sendall(ARGS) {
  UNUSED; chk("sendall", n, 2, 2);
  Sock *s = SELF(a[0]); check_open(s);
  Str *data = need_str(a[1], "data");
  size_t off = 0;
  while (off < (size_t)data->len) {
    if (s->timeout >= 0) {
      fd_set wf; FD_ZERO(&wf); FD_SET(s->fd, &wf);
      struct timeval tv; tv.tv_sec = (time_t)s->timeout; tv.tv_usec = (long)((s->timeout - (double)(time_t)s->timeout) * 1e6);
      int sr = select(s->fd + 1, NULL, &wf, NULL, &tv);
      if (sr == 0) throw_error("TimeoutError", "timed out");
      if (sr < 0) raise_errno("sendall");
    }
    ssize_t w = send(s->fd, data->s + off, data->len - off, 0);
    if (w < 0) { if (errno == EINTR) continue; raise_errno("sendall"); }
    off += (size_t)w;
  }
  return V_none();
}
static Value sk_send(ARGS) {
  UNUSED; chk("send", n, 2, 2);
  Sock *s = SELF(a[0]); check_open(s);
  Str *data = need_str(a[1], "data");
  ssize_t w = send(s->fd, data->s, data->len, 0);
  if (w < 0) raise_errno("send");
  return V_int(w);
}
static Value sk_close(ARGS) {
  UNUSED; Sock *s = SELF(a[0]);
  if (!s->closed && s->fd >= 0) close(s->fd);
  s->closed = 1; return V_none();
}
static Value sk_settimeout(ARGS) {
  UNUSED; chk("settimeout", n, 2, 2);
  Sock *s = SELF(a[0]);
  s->timeout = a[1].t == T_NONE ? -1 : need_num(a[1], "timeout");
  return V_none();
}
static Value sk_gettimeout(ARGS) { UNUSED; Sock *s = SELF(a[0]); return s->timeout < 0 ? V_none() : V_float(s->timeout); }
static Value sk_setblocking(ARGS) {
  UNUSED; chk("setblocking", n, 2, 2);
  Sock *s = SELF(a[0]); s->timeout = val_truthy(a[1]) ? -1 : 0;
  return V_none();
}
static Value sk_setsockopt(ARGS) {
  UNUSED; chk("setsockopt", n, 4, 4);
  Sock *s = SELF(a[0]); check_open(s);
  int level = (int)need_int(a[1], "level"), opt = (int)need_int(a[2], "optname"), val = (int)need_int(a[3], "value");
  if (setsockopt(s->fd, level, opt, &val, sizeof val) != 0) raise_errno("setsockopt");
  return V_none();
}
static Value sk_getsockname(ARGS) {
  UNUSED; Sock *s = SELF(a[0]); check_open(s);
  struct sockaddr_storage ss; socklen_t sl = sizeof ss;
  if (getsockname(s->fd, (struct sockaddr *)&ss, &sl) != 0) raise_errno("getsockname");
  return addr_to_tuple((struct sockaddr *)&ss);
}
static Value sk_getpeername(ARGS) {
  UNUSED; Sock *s = SELF(a[0]); check_open(s);
  struct sockaddr_storage ss; socklen_t sl = sizeof ss;
  if (getpeername(s->fd, (struct sockaddr *)&ss, &sl) != 0) raise_errno("getpeername");
  return addr_to_tuple((struct sockaddr *)&ss);
}
static Value sk_fileno(ARGS) { UNUSED; return V_int(SELF(a[0])->fd); }
static Value sk_shutdown(ARGS) {
  UNUSED; chk("shutdown", n, 2, 2); Sock *s = SELF(a[0]); check_open(s);
  shutdown(s->fd, (int)need_int(a[1], "how")); return V_none();
}
static Value sk_enter(ARGS) { UNUSED; return inc(a[0]); }
static Value sk_exit(ARGS) { UNUSED; Sock *s = SELF(a[0]); if (!s->closed && s->fd >= 0) close(s->fd); s->closed = 1; return V_bool(0); }

/* --------------------------------------------------------------- module */
static Dict *sock_methods;
static void regm(Dict *t, const char *name, BuiltinFn fn) { Value b = new_builtin(name, fn); dict_set_cstr(t, name, b); decref(b); }

Value builtin_method_socket(Value obj, Str *name) {
  Value *f = dict_find_str(sock_methods, name);
  return f ? make_bound(obj, *f) : V_undef();
}

static Value sk_new(ARGS) {
  chk("socket", n, 0, 2);
  int family = AF_INET, type_ = SOCK_STREAM;
  if (n >= 1) family = (int)need_int(a[0], "family");
  if (n >= 2) type_ = (int)need_int(a[1], "type");
  int fd = socket(family, type_, 0);
  if (fd < 0) raise_errno("socket");
  return make_sock(fd, family, type_);
}
static Value sk_gethostbyname(ARGS) {
  UNUSED; chk("gethostbyname", n, 1, 1);
  Str *host = need_str(a[0], "host");
  struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET;
  struct addrinfo *res;
  if (getaddrinfo(host->s, NULL, &hints, &res) != 0) throw_error("OSError", "[Errno -2] Name or service not known: %s", host->s);
  char ip[NI_MAXHOST];
  inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof ip);
  freeaddrinfo(res);
  return V_str(ip);
}
static Value sk_getfqdn(ARGS) {
  chk("getfqdn", n, 0, 1);
  char buf[256];
  if (n == 0 || ((Str *)a[0].o)->len == 0) { gethostname(buf, sizeof buf); return V_str(buf); }
  return inc(a[0]);
}

Value net_module(void) {
  sock_methods = dict_new();
  regm(sock_methods, "connect", sk_connect); regm(sock_methods, "connect_ex", sk_connect_ex);
  regm(sock_methods, "bind", sk_bind); regm(sock_methods, "listen", sk_listen); regm(sock_methods, "accept", sk_accept);
  regm(sock_methods, "recv", sk_recv); regm(sock_methods, "recv_into", sk_recv_into_str);
  regm(sock_methods, "send", sk_send); regm(sock_methods, "sendall", sk_sendall);
  regm(sock_methods, "close", sk_close); regm(sock_methods, "settimeout", sk_settimeout); regm(sock_methods, "gettimeout", sk_gettimeout);
  regm(sock_methods, "setblocking", sk_setblocking); regm(sock_methods, "setsockopt", sk_setsockopt);
  regm(sock_methods, "getsockname", sk_getsockname); regm(sock_methods, "getpeername", sk_getpeername);
  regm(sock_methods, "fileno", sk_fileno); regm(sock_methods, "shutdown", sk_shutdown);
  regm(sock_methods, "__enter__", sk_enter); regm(sock_methods, "__exit__", sk_exit);

  Module *m = xmalloc(sizeof(Module));
  m->h.rc = 1; m->h.type = T_MODULE; m->name = INTERN("_socket"); oinc(m->name); m->attrs = dict_new();
  Value mv = V_obj(m, T_MODULE);
  Value ctor = new_builtin("socket", sk_new); dict_set_cstr(m->attrs, "socket", ctor); decref(ctor);
  Value ghn = new_builtin("gethostbyname", sk_gethostbyname); dict_set_cstr(m->attrs, "gethostbyname", ghn); decref(ghn);
  Value gfq = new_builtin("getfqdn", sk_getfqdn); dict_set_cstr(m->attrs, "getfqdn", gfq); decref(gfq);
#define CONST(name, val) do { Value v = V_int(val); dict_set_cstr(m->attrs, name, v); decref(v); } while (0)
  CONST("AF_INET", AF_INET); CONST("AF_INET6", AF_INET6); CONST("AF_UNSPEC", AF_UNSPEC);
  CONST("SOCK_STREAM", SOCK_STREAM); CONST("SOCK_DGRAM", SOCK_DGRAM);
  CONST("SOL_SOCKET", SOL_SOCKET); CONST("SO_REUSEADDR", SO_REUSEADDR); CONST("SO_KEEPALIVE", SO_KEEPALIVE);
  CONST("IPPROTO_TCP", IPPROTO_TCP); CONST("TCP_NODELAY", TCP_NODELAY);
  CONST("SHUT_RD", SHUT_RD); CONST("SHUT_WR", SHUT_WR); CONST("SHUT_RDWR", SHUT_RDWR);
#undef CONST
  return mv;
}
