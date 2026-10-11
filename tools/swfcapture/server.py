"""Local server for capture.html: serves this folder plus the SWFs, and stores the frames the page posts.

Usage: python server.py <swf dir> <frame out dir> [port]
GET /swf/<name>.swf comes from the swf dir; POST /frame/<movie>/<n> writes <out>/<movie>/<n>.png.
"""
import http.server
import os
import sys

SWF, OUT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))


class H(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = path.split("?")[0]
        if path.startswith("/swf/"):
            return os.path.join(SWF, os.path.basename(path))
        return os.path.join(HERE, os.path.basename(path) or "capture.html")

    def do_POST(self):
        parts = self.path.strip("/").split("/")
        data = self.rfile.read(int(self.headers["Content-Length"]))
        if len(parts) == 3 and parts[0] == "frame" and parts[1].isalnum() and parts[2].isdigit():
            d = os.path.join(OUT, parts[1])
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "%04d.png" % int(parts[2])), "wb") as f:
                f.write(data)
            self.send_response(200)
        else:
            self.send_response(400)
        self.end_headers()

    def log_message(self, *a):
        pass


http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[3]) if len(sys.argv) > 3 else 8765), H).serve_forever()
