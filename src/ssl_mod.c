/* ssl_mod.c - a minimal TLS client, modeled on CPython's `ssl` module,
 * wrapping OpenSSL's client handshake around an already-connected `socket`.
 * Compiled only when the build detects OpenSSL headers/libs (see Makefile);
 * otherwise net.c's stub `ssl_module()` is used instead so the rest of the
 * interpreter builds and works fine without it. */
#ifdef CPY_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include "cpy.h"

typedef struct SSLSock {
  Obj h;
  SSL_CTX *ctx;
  SSL *ssl;
  Value sock;     /* the underlying plain socket Value -- keeps its fd alive */
  int closed;
} SSLSock;

#define ARGS Value *a, int n, Kw *kw
#define UNUSED (void)a; (void)n; (void)kw

static pthread_once_t g_ssl_init_once = PTHREAD_ONCE_INIT;
static void ssl_lib_init(void) {
  SSL_library_init();
  SSL_load_error_strings();
}

static void raise_ssl_error(const char *what) {
  unsigned long e = ERR_get_error();
  char buf[256];
  if (e) { ERR_error_string_n(e, buf, sizeof buf); throw_error("SSLError", "%s: %s", what, buf); }
  throw_error("SSLError", "%s: unknown TLS error", what);
}

static SSLSock *SELF(Value v) { return (SSLSock *)v.o; }
static void check_open(SSLSock *s) { if (s->closed) throw_error("OSError", "[Errno 9] Bad file descriptor (TLS socket is closed)"); }

void sslsock_free(Obj *o) {
  SSLSock *s = (SSLSock *)o;
  if (!s->closed && s->ssl) SSL_shutdown(s->ssl);
  if (s->ssl) SSL_free(s->ssl);          /* also frees the BIO we attached */
  if (s->ctx) SSL_CTX_free(s->ctx);
  decref(s->sock);
  free(s);
}

/* fd accessor into the plain `socket` object, shared with net.c's Sock */
extern int sock_get_fd(Value v);

static Value ssl_wrap_socket(ARGS) {
  chk("_wrap_socket", n, 1, 3);
  pthread_once(&g_ssl_init_once, ssl_lib_init);
  Value sock = a[0];
  if (sock.t != T_SOCK) throw_error("TypeError", "_wrap_socket() argument must be a socket");
  const char *hostname = NULL;
  if (n >= 2 && a[1].t == T_STR) hostname = ((Str *)a[1].o)->s;
  int verify = 1;
  if (n >= 3) verify = val_truthy(a[2]);

  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) raise_ssl_error("SSL_CTX_new");
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_default_verify_paths(ctx);
  SSL_CTX_set_verify(ctx, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);

  SSL *ssl = SSL_new(ctx);
  if (!ssl) { SSL_CTX_free(ctx); raise_ssl_error("SSL_new"); }

  int fd = sock_get_fd(sock);
  if (SSL_set_fd(ssl, fd) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); raise_ssl_error("SSL_set_fd"); }

  if (hostname) SSL_set_tlsext_host_name(ssl, hostname);   /* SNI: tell the server which vhost we want */
  /* Hostname verification is done explicitly below via X509_check_host after the
   * handshake, rather than via SSL_set1_host beforehand -- keeping it to one
   * code path that's easy to reason about and test. */

  int r = SSL_connect(ssl);
  if (r != 1) {
    int err = SSL_get_error(ssl, r);
    SSL_free(ssl); SSL_CTX_free(ctx);
    if (err == SSL_ERROR_SYSCALL) throw_error("SSLError", "TLS handshake failed: connection closed by peer");
    raise_ssl_error("SSL_connect (handshake failed -- check the hostname and that the server speaks TLS)");
  }
  if (verify) {
    long vr = SSL_get_verify_result(ssl);
    if (vr != X509_V_OK) {
      const char *msg = X509_verify_cert_error_string(vr);
      SSL_free(ssl); SSL_CTX_free(ctx);
      throw_error("SSLCertVerificationError", "certificate verify failed: %s", msg);
    }
    if (hostname) {                                  /* ... and suspenders: check it ourselves too */
      X509 *cert = SSL_get_peer_certificate(ssl);
      if (!cert) {
        SSL_free(ssl); SSL_CTX_free(ctx);
        throw_error("SSLCertVerificationError", "server presented no certificate");
      }
      int hok = X509_check_host(cert, hostname, strlen(hostname), 0, NULL);
      X509_free(cert);
      if (hok != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx);
        throw_error("SSLCertVerificationError", "certificate does not match hostname %s", hostname);
      }
    }
  }

  SSLSock *s = xmalloc(sizeof(SSLSock));
  s->h.rc = 1; s->h.type = T_SSLSOCK;
  s->ctx = ctx; s->ssl = ssl; s->sock = inc(sock); s->closed = 0;
  return V_obj(s, T_SSLSOCK);
}

static Value ssls_sendall(ARGS) {
  UNUSED; chk("sendall", n, 2, 2);
  SSLSock *s = SELF(a[0]); check_open(s);
  Str *data = need_str(a[1], "data");
  size_t off = 0;
  while (off < (size_t)data->len) {
    int w = SSL_write(s->ssl, data->s + off, (int)(data->len - off));
    if (w <= 0) {
      int err = SSL_get_error(s->ssl, w);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
      raise_ssl_error("SSL_write");
    }
    off += (size_t)w;
  }
  return V_none();
}
static Value ssls_send(ARGS) {
  UNUSED; chk("send", n, 2, 2);
  SSLSock *s = SELF(a[0]); check_open(s);
  Str *data = need_str(a[1], "data");
  int w = SSL_write(s->ssl, data->s, data->len);
  if (w <= 0) raise_ssl_error("SSL_write");
  return V_int(w);
}
static Value ssls_recv(ARGS) {
  UNUSED; chk("recv", n, 2, 2);
  SSLSock *s = SELF(a[0]); check_open(s);
  int64_t bufsize = need_int(a[1], "bufsize");
  if (bufsize < 0 || bufsize > (64 << 20)) throw_error("ValueError", "recv() buffer size out of range");
  char *buf = xmalloc((size_t)bufsize + 1);
  int r = SSL_read(s->ssl, buf, (int)bufsize);
  if (r <= 0) {
    int err = SSL_get_error(s->ssl, r);
    free(buf);
    if (err == SSL_ERROR_ZERO_RETURN) return V_str("");   /* clean TLS close, like a 0-byte recv() */
    raise_ssl_error("SSL_read");
  }
  Value v = V_strn(buf, r); free(buf); return v;
}
static Value ssls_close(ARGS) {
  UNUSED; SSLSock *s = SELF(a[0]);
  if (!s->closed && s->ssl) SSL_shutdown(s->ssl);
  s->closed = 1; return V_none();
}
static Value ssls_enter(ARGS) { UNUSED; return inc(a[0]); }
static Value ssls_exit(ARGS) { UNUSED; SSLSock *s = SELF(a[0]); if (!s->closed && s->ssl) SSL_shutdown(s->ssl); s->closed = 1; return V_bool(0); }

static Dict *sslsock_methods;
static void regm(Dict *t, const char *name, BuiltinFn fn) { Value b = new_builtin(name, fn); dict_set_cstr(t, name, b); decref(b); }

Value builtin_method_sslsock(Value obj, Str *name) {
  Value *f = dict_find_str(sslsock_methods, name);
  return f ? make_bound(obj, *f) : V_undef();
}

Value ssl_module(void) {
  sslsock_methods = dict_new();
  regm(sslsock_methods, "sendall", ssls_sendall); regm(sslsock_methods, "send", ssls_send);
  regm(sslsock_methods, "recv", ssls_recv); regm(sslsock_methods, "close", ssls_close);
  regm(sslsock_methods, "__enter__", ssls_enter); regm(sslsock_methods, "__exit__", ssls_exit);

  Module *m = xmalloc(sizeof(Module));
  m->h.rc = 1; m->h.type = T_MODULE; m->name = INTERN("_ssl"); oinc(m->name); m->attrs = dict_new();
  Value mv = V_obj(m, T_MODULE);
  Value wrap = new_builtin("_wrap_socket", ssl_wrap_socket);
  dict_set_cstr(m->attrs, "_wrap_socket", wrap); decref(wrap);
  Value avail = V_bool(1); dict_set_cstr(m->attrs, "available", avail); decref(avail);
  return mv;
}

#else /* !CPY_HAVE_OPENSSL */
#include "cpy.h"
void sslsock_free(Obj *o) { (void)o; }
Value builtin_method_sslsock(Value obj, Str *name) { (void)obj; (void)name; return V_undef(); }
Value ssl_module(void) { return V_undef(); }  /* -> plain "No module named '_ssl'" on import */
#endif
