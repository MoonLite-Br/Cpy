from _socket import (
    socket, gethostbyname, getfqdn,
    AF_INET, AF_INET6, AF_UNSPEC, SOCK_STREAM, SOCK_DGRAM,
    SOL_SOCKET, SO_REUSEADDR, SO_KEEPALIVE, IPPROTO_TCP, TCP_NODELAY,
    SHUT_RD, SHUT_WR, SHUT_RDWR,
)

has_ipv6 = True


def create_connection(address, timeout=None, source_address=None):
    host, port = address
    s = socket(AF_INET, SOCK_STREAM)
    if timeout is not None:
        s.settimeout(timeout)
    s.connect((host, port))
    return s
