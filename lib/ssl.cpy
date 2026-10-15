# lib/ssl.cpy - a small client-side TLS layer, in the spirit of Python's
# `ssl` module, over the native _ssl module (src/ssl_mod.c, OpenSSL).
try:
    from _ssl import _wrap_socket
    _available = True
except ImportError:
    _available = False


class SSLError(OSError):
    pass


class SSLCertVerificationError(SSLError):
    pass


CERT_NONE = 0
CERT_REQUIRED = 1


def _check_available():
    if not _available:
        raise SSLError(
            "this cpy binary was built without TLS support -- install OpenSSL's "
            "dev package (e.g. `pkg install openssl` on Termux, `apt install "
            "libssl-dev` on Debian/Ubuntu) and rebuild (./install.sh or `make`)")


def wrap_socket(sock, server_hostname=None, verify=True):
    """Wrap an already-connected plain `socket` in TLS and perform the
    handshake. Returns an object with send/sendall/recv/close, usable
    anywhere a plain socket is (e.g. by urllib for https://)."""
    _check_available()
    return _wrap_socket(sock, server_hostname, verify)


class SSLContext:
    def __init__(self, verify=True):
        self.verify = verify
        self.check_hostname = verify

    def wrap_socket(self, sock, server_hostname=None):
        return wrap_socket(sock, server_hostname=server_hostname, verify=self.verify)


def create_default_context():
    return SSLContext(verify=True)


def _create_unverified_context():
    return SSLContext(verify=False)
