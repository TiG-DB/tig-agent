#!/usr/bin/env python3
"""A read-only stand-in for the Docker Engine API, for end_to_end_docker.sh.

Usage: fake_docker.py unix <socket path> <scenario dir>   (or: tcp <port file> <scenario dir>)

Serves files from the scenario directory, which the test can swap while it runs:
  GET /containers/json...        list.json
  GET /containers/<id>/json      <id>.inspect.json
  GET /containers/<id>/stats...  <id>.stats.json
Anything else, or a file that is missing, is a 404. Anything but GET is a 405, as it would be from a read-only proxy.
"""
import http.server
import os
import re
import socketserver
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def address_string(self):
        return "client"

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/containers/json":
            name = "list.json"
        else:
            match = re.fullmatch(r"/containers/([0-9a-f]+)/(json|stats)", path)
            name = "%s.%s.json" % (match.group(1), "inspect" if match.group(2) == "json" else "stats") if match else None
        file = os.path.join(self.server.scenario, name) if name else None
        if not file or not os.path.exists(file):
            self.send_error(404)
            return
        with open(file, "rb") as f:
            body = f.read().rstrip(b"\n") + b"\n"  # the Engine ends its JSON with a newline
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        self.send_error(405)

    do_PUT = do_DELETE = do_POST


class UnixServer(socketserver.UnixStreamServer):
    def get_request(self):
        request, _ = super().get_request()
        return request, ("local", 0)


kind, where, scenario = sys.argv[1:4]
if kind == "unix":
    server = UnixServer(where, Handler)
else:
    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    with open(where, "w") as f:
        f.write(str(server.server_address[1]))
server.scenario = scenario
server.serve_forever()
