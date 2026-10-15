# lib/urllib.cpy - a minimal, dependency-free HTTP/1.1 client, in the spirit
# of Python's urllib.request but trimmed to what actually fits a scripting
# tool: GET/POST/PUT/DELETE with headers and a body, redirects, and chunked
# or Content-Length responses. https:// works when this cpy binary was
# built with OpenSSL (the default when it's available -- see docs/language.md);
# otherwise it raises ssl.SSLError with instructions to install it.
import socket
import ssl as _ssl


class URLError(Exception):
    pass


class HTTPError(URLError):
    def __init__(self, url, code, msg, headers, body):
        super().__init__(f"HTTP Error {code}: {msg}")
        self.url = url
        self.code = code
        self.msg = msg
        self.headers = headers
        self.body = body


class Response:
    def __init__(self, url, status, reason, headers, body):
        self.url = url
        self.status = status
        self.code = status
        self.reason = reason
        self.headers = headers
        self._body = body

    def read(self):
        return self._body

    def text(self):
        return self._body

    def json(self):
        import json
        return json.loads(self._body)

    def getheader(self, name, default=None):
        name = name.lower()
        for k, v in self.headers:
            if k.lower() == name:
                return v
        return default

    def __enter__(self):
        return self

    def __exit__(self, a, b, c):
        return False


def _split_url(url):
    if url.startswith("https://"):
        scheme, default_port, rest = "https", 443, url[len("https://"):]
    elif url.startswith("http://"):
        scheme, default_port, rest = "http", 80, url[len("http://"):]
    else:
        raise URLError(f"unsupported URL scheme: {url!r}")
    slash = rest.find("/")
    if slash < 0:
        hostport, path = rest, "/"
    else:
        hostport, path = rest[:slash], rest[slash:]
    if ":" in hostport:
        host, port = hostport.split(":", 1)
        port = int(port)
    else:
        host, port = hostport, default_port
    return scheme, host, port, path


def _parse_response(sock, timeout):
    buf = b"" if False else ""
    while "\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf = buf + chunk
    head, _, rest = buf.partition("\r\n\r\n")
    lines = head.split("\r\n")
    status_line = lines[0]
    parts = status_line.split(" ", 2)
    status = int(parts[1])
    reason = parts[2] if len(parts) > 2 else ""
    headers = []
    content_length = None
    chunked = False
    for line in lines[1:]:
        if not line:
            continue
        name, _, value = line.partition(":")
        name = name.strip()
        value = value.strip()
        headers.append((name, value))
        if name.lower() == "content-length":
            content_length = int(value)
        if name.lower() == "transfer-encoding" and "chunked" in value.lower():
            chunked = True

    body = rest
    if chunked:
        body = _read_chunked(sock, body)
    elif content_length is not None:
        while len(body) < content_length:
            chunk = sock.recv(4096)
            if not chunk:
                break
            body = body + chunk
        body = body[0:content_length]
    else:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            body = body + chunk
    return status, reason, headers, body


def _read_chunked(sock, buf):
    out = []
    while True:
        while "\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                return "".join(out)
            buf = buf + chunk
        size_line, _, buf = buf.partition("\r\n")
        size = int(size_line.split(";")[0], 16)
        if size == 0:
            return "".join(out)
        while len(buf) < size + 2:
            chunk = sock.recv(4096)
            if not chunk:
                return "".join(out)
            buf = buf + chunk
        out.append(buf[0:size])
        buf = buf[size + 2:]


def request(method, url, data=None, headers=None, timeout=10, max_redirects=5):
    method = method.upper()
    hdrs = dict(headers) if headers else {}
    for _ in range(max_redirects + 1):
        scheme, host, port, path = _split_url(url)
        lines = [f"{method} {path} HTTP/1.1", f"Host: {host}"]
        body = data if data is not None else ""
        if "connection" not in {k.lower() for k in hdrs}:
            lines.append("Connection: close")
        if body and "content-length" not in {k.lower() for k in hdrs}:
            lines.append(f"Content-Length: {len(body)}")
        if "user-agent" not in {k.lower() for k in hdrs}:
            lines.append("User-Agent: cpy-urllib/1.0")
        for k, v in hdrs.items():
            lines.append(f"{k}: {v}")
        req = "\r\n".join(lines) + "\r\n\r\n" + body

        raw = socket.create_connection((host, port), timeout=timeout)
        if scheme == "https":
            try:
                s = _ssl.wrap_socket(raw, server_hostname=host)
            except _ssl.SSLError as e:
                raw.close()
                raise URLError(str(e))
        else:
            s = raw
        try:
            s.sendall(req)
            status, reason, resp_headers, resp_body = _parse_response(s, timeout)
        finally:
            s.close()

        if status in (301, 302, 303, 307, 308):
            location = None
            for k, v in resp_headers:
                if k.lower() == "location":
                    location = v
            if location:
                if location.startswith("http://") or location.startswith("https://"):
                    url = location
                else:
                    url = f"{scheme}://{host}:{port}" + (location if location.startswith("/") else "/" + location)
                if status == 303:
                    method = "GET"
                    data = None
                continue
        break

    resp = Response(url, status, reason, resp_headers, resp_body)
    if status >= 400:
        raise HTTPError(url, status, reason, resp_headers, resp_body)
    return resp


def get(url, headers=None, timeout=10):
    return request("GET", url, headers=headers, timeout=timeout)


def post(url, data="", headers=None, timeout=10):
    return request("POST", url, data=data, headers=headers, timeout=timeout)


def urlopen(url, data=None, timeout=10):
    method = "POST" if data is not None else "GET"
    return request(method, url, data=data, timeout=timeout)
