import socket
import json
import urllib

# constants and basic object shape (no live network needed for these)
print(socket.AF_INET > 0, socket.SOCK_STREAM > 0)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
print(type(s).__name__)
s.settimeout(2)
print(s.gettimeout())
s.close()
try:
    s.send("x")
except OSError as e:
    print("closed socket error:", type(e).__name__)

try:
    socket.socket(socket.AF_INET, socket.SOCK_STREAM).connect(("127.0.0.1", 1))
except ConnectionRefusedError as e:
    print("refused:", type(e).__name__)

try:
    urllib.get("https://example.com")
except urllib.URLError as e:
    print("https blocked as expected")

print(json.dumps({"b": 2, "a": 1}, sort_keys=True))
print(json.dumps([1, "two", 3.5, None, True, False]))
print(json.loads('{"x": [1, 2, {"y": null}], "s": "a\\nb"}'))
print(json.dumps("quote\"and\\backslash"))
print(json.loads(json.dumps({"nested": {"a": [1, 2, 3]}})))
print(json.dumps({"k": 1}, indent=2))
