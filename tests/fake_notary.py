#!/usr/bin/env python3
"""A stand-in for TiG Notary and for the issuer's published keys, for end_to_end_notary.sh.

Usage: fake_notary.py <port file> <directory>

It follows COMMITS.md, "Notary protocol", using only ssh-keygen. The directory holds what the test changes while it runs:
  key, key.pub   the notary's key, which signs tags
  published      the file served at /.well-known/tig-public-key
  mode           what POST /v1/sign does (read for every request; "ok" if the file is missing):
                   ok            sign
                   down          503
                   rate          429 with Retry-After
                   wrong-issuer  sign a tag that names another issuer
                   unknown-key   sign with a key that isn't published
  offset         seconds the notary's clock is ahead of this machine's, 0 if the file is missing
  requests.log   one line for each request: its Object, and the User-Agent it came with
"""
import calendar
import email.utils
import http.server
import os
import re
import subprocess
import sys
import tempfile
import time

ISSUER = "tigdb.org"
WINDOW = 300


def read(directory, name, default):
    try:
        with open(os.path.join(directory, name)) as f:
            return f.read().strip() or default
    except FileNotFoundError:
        return default


def ssh_keygen(args, data=None):
    return subprocess.run(["ssh-keygen", "-q"] + args, input=data, capture_output=True)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *args):
        pass

    def now(self):
        return int(time.time()) + int(read(self.server.directory, "offset", "0"))

    # the Date header comes from the same clock that judges Time, as COMMITS.md asks
    def date_time_string(self, timestamp=None):
        return email.utils.formatdate(self.now(), usegmt=True)

    def reply(self, status, text, headers=()):
        body = (text.rstrip("\n") + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/.well-known/tig-public-key":
            self.reply(200, read(self.server.directory, "published", ""))
        else:
            self.reply(404, "not found")

    def do_POST(self):
        directory = self.server.directory
        if self.path != "/v1/sign":
            return self.reply(404, "not found")
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        mode = read(directory, "mode", "ok")

        if mode == "down":
            return self.reply(503, "the notary's clock is not synchronised")
        if mode == "rate":
            return self.reply(429, "rate limit exceeded", [("Retry-After", "60")])
        if self.headers.get("Content-Type") != "text/plain":
            return self.reply(400, "Content-Type must be text/plain")

        marker = b"-----BEGIN SSH SIGNATURE-----"
        at = body.find(marker)
        fields = {}
        if at > 0:
            for line in body[:at].decode().rstrip("\n").split("\n"):
                name, _, value = line.partition(": ")
                fields.setdefault(name, value)
        if (fields.get("TiG-Sign-Request") != "1" or not re.fullmatch(r"[0-9a-f]{40}", fields.get("Object", ""))
                or not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", fields.get("Time", ""))
                or not fields.get("Key", "").startswith("ssh-ed25519 ") or " " in fields["Key"][12:]):
            return self.reply(400, "bad request")

        with tempfile.TemporaryDirectory() as work:
            # the request must be signed by Key, in the tig-notary namespace
            with open(os.path.join(work, "allowed"), "w") as f:
                f.write('device namespaces="tig-notary" %s\n' % fields["Key"])
            with open(os.path.join(work, "request.sig"), "wb") as f:
                f.write(body[at:])
            verified = ssh_keygen(["-Y", "verify", "-f", os.path.join(work, "allowed"), "-I", "device", "-n", "tig-notary",
                                   "-s", os.path.join(work, "request.sig")], body[:at])
            if verified.returncode != 0:
                return self.reply(401, "signature does not verify")

            now = self.now()
            asked = calendar.timegm(time.strptime(fields["Time"], "%Y-%m-%dT%H:%M:%SZ"))
            if abs(asked - now) > WINDOW:
                return self.reply(422, "Time is out; correct it from the Date header")

            with open(os.path.join(work, "device.pub"), "w") as f:
                f.write(fields["Key"] + "\n")
            device = ssh_keygen(["-l", "-f", os.path.join(work, "device.pub")]).stdout.decode().split()[1]

            issuer = "other.example" if mode == "wrong-issuer" else ISSUER
            stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(now))
            payload = ("object %s\ntype commit\ntag tig/sig/%s\ntagger TiG Notary <notary@example.test> %d +0000\n\n"
                       "TiG-Signature: 1\nIssuer: %s\nDevice: %s\n" % (fields["Object"], stamp, now, issuer, device))
            with open(os.path.join(work, "tag"), "w") as f:
                f.write(payload)
            key = os.path.join(directory, "other_key" if mode == "unknown-key" else "key")
            signed = ssh_keygen(["-Y", "sign", "-n", "git", "-f", key, os.path.join(work, "tag")])
            if signed.returncode != 0:
                return self.reply(500, "cannot sign: " + signed.stderr.decode())
            with open(os.path.join(work, "tag.sig")) as f:
                tag = payload + f.read()

        with open(os.path.join(directory, "requests.log"), "a") as log:
            log.write("%s %s\n" % (fields["Object"], self.headers.get("User-Agent", "")))
        self.reply(200, tag.rstrip("\n"))


port_file, directory = sys.argv[1:3]
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
server.directory = directory
with open(port_file, "w") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
