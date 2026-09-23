#!/usr/bin/env python3
"""Local HTTP mock server for testing ESP32 HTTPS request flow and device claim without the real backend.

Claim scenario is selected by the claimToken's prefix, so no server restart is needed
to switch between success / retryable-failure / permanent-failure test cases:

  SUCCESS-*  -> /complete succeeds immediately
  RETRY-*    -> /complete returns 503 twice, then succeeds (tests retry/backoff)
  INVALID-*  -> /complete returns 400 (permanent failure)
  EXPIRED-*  -> /complete returns 410 (permanent failure)
  MISMATCH-* -> /complete returns 409 (permanent failure)

Usage:
  1. POST /devices/<deviceId>/claims {"scenario": "retry"} -> {"claimToken": "RETRY-abcd1234"}
  2. Feed that claimToken to the ESP (BLE claim endpoint)
  3. ESP calls POST /device-claims/<claimToken>/complete with {"deviceId": "..."}
     (400 if deviceId is missing, 409 if it differs from the deviceId the claim was created for)
"""

import json
import re
import secrets
from http.server import BaseHTTPRequestHandler, HTTPServer

HOST = "0.0.0.0"
PORT = 8080

SCENARIOS = ("SUCCESS", "RETRY", "INVALID", "EXPIRED", "MISMATCH")

# claimToken -> number of /complete attempts so far (only used by RETRY-* tokens)
_attempt_counts = {}
# claimToken -> claim을 만든 deviceId. /complete의 deviceId가 이 값과 같은지 검증한다.
_claim_devices = {}


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, status, body):
        payload = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/ping":
            self._send_json(200, {"status": "ok"})
            return
        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        match = re.match(r"^/devices/([^/]+)/claims$", self.path)
        if match:
            self._handle_create_claim(match.group(1))
            return

        match = re.match(r"^/device-claims/([^/]+)/complete$", self.path)
        if match:
            self._handle_complete_claim(match.group(1))
            return

        match = re.match(r"^/devices/([^/]+)/telemetry$", self.path)
        if match:
            self._handle_telemetry(match.group(1))
            return

        self.send_response(404)
        self.end_headers()

    def _handle_create_claim(self, device_id):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length)) if length else {}
        scenario = str(body.get("scenario", "success")).upper()

        if scenario not in SCENARIOS:
            self._send_json(400, {"error": f"unknown scenario, expected one of {SCENARIOS}"})
            return

        claim_token = f"{scenario}-{secrets.token_hex(4)}"
        _claim_devices[claim_token] = device_id
        self._send_json(200, {"claimToken": claim_token})

    def _handle_telemetry(self, device_id):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer ") or len(auth) <= len("Bearer "):
            self._send_json(401, {"error": "missing deviceToken"})
            return

        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length)) if length else {}
        print(f"[telemetry] device={device_id} token={auth[7:]} body={body}", flush=True)
        self._send_json(200, {"status": "ok"})

    def _handle_complete_claim(self, claim_token):
        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length)) if length else {}
        except json.JSONDecodeError:
            body = {}
        device_id = body.get("deviceId") if isinstance(body, dict) else None
        if not device_id:
            self._send_json(400, {"error": "deviceId is required"})
            return
        expected = _claim_devices.get(claim_token)
        if expected is not None and expected != device_id:
            self._send_json(409, {"error": "deviceId does not match the claim"})
            return

        scenario = claim_token.split("-", 1)[0]

        if scenario == "SUCCESS":
            self._send_json(200, {"deviceToken": f"devtok-{secrets.token_hex(8)}"})
        elif scenario == "RETRY":
            attempt = _attempt_counts.get(claim_token, 0) + 1
            _attempt_counts[claim_token] = attempt
            if attempt < 3:
                self._send_json(503, {"error": "temporarily unavailable"})
            else:
                self._send_json(200, {"deviceToken": f"devtok-{secrets.token_hex(8)}"})
        elif scenario == "INVALID":
            self._send_json(400, {"error": "invalid claim token"})
        elif scenario == "EXPIRED":
            self._send_json(410, {"error": "claim token expired"})
        elif scenario == "MISMATCH":
            self._send_json(409, {"error": "device already claimed by another user"})
        else:
            self._send_json(404, {"error": "unknown claim token"})


if __name__ == "__main__":
    server = HTTPServer((HOST, PORT), Handler)
    print(f"Mock server listening on http://{HOST}:{PORT}")
    server.serve_forever()
