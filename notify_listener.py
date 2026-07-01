#!/usr/bin/env python3
"""
Listener for device-initiated token push notifications.
Runs as a systemd user service on port 5555.
When the Claude Meter device comes online, it POSTs to /notify
and this service immediately triggers push_claude_token.py
"""
import http.server
import socketserver
import subprocess
import sys
import threading
import time

PORT = 5555


class NotifyHandler(http.server.BaseHTTPRequestHandler):
    def _reply_ok(self):
        body = b"OK\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.log_message("Device came online, triggering token push...")
        trigger_push()

    def do_GET(self):
        """Handle GET /notify from device"""
        if self.path == "/notify":
            self._reply_ok()
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        """Handle POST /notify from device"""
        if self.path == "/notify":
            self._reply_ok()
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        """Override to use stderr with timestamp"""
        sys.stderr.write("[%s] %s\n" % (self.log_date_time_string(), format % args))
        sys.stderr.flush()


def trigger_push():
    """Run push_claude_token.py in a background thread"""
    def run_push():
        try:
            result = subprocess.run(
                ["/usr/bin/python3", "/home/jeremy/scripts/push_claude_token.py"],
                capture_output=True,
                text=True,
                timeout=120,
            )
            if result.returncode == 0:
                sys.stderr.write("[notify] Token push succeeded\n")
            else:
                sys.stderr.write(
                    f"[notify] Token push failed: exit {result.returncode}\n"
                )
                if result.stderr:
                    sys.stderr.write(f"[notify] stderr: {result.stderr}\n")
        except Exception as e:
            sys.stderr.write(f"[notify] Error running push: {e}\n")
        sys.stderr.flush()

    thread = threading.Thread(target=run_push, daemon=True)
    thread.start()


class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True


if __name__ == "__main__":
    handler = NotifyHandler
    with ReusableTCPServer(("", PORT), handler) as httpd:
        sys.stderr.write(f"[notify] Listening on port {PORT}\n")
        sys.stderr.flush()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            sys.stderr.write("[notify] Shutting down\n")
            sys.exit(0)
