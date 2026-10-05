#!/usr/bin/env python3
"""모바일 앱 역할을 흉내 내는 E2E 테스트 스크립트 (AGENTS.md 11번 claim flow).

ESP-IDF의 python 환경(bleak, cryptography, protobuf 설치됨)으로 실행한다:
  ~/.espressif/python_env/idf6.1_py3.13_env/bin/python3 tools/phone_sim.py <command> ...

설정은 tools/phone_sim.env (버전 관리 제외, phone_sim.env.example 참고) 또는 환경변수로 준다.

Commands:
  provision --service_name PROV_XXXX [--ssid S --passphrase P]  BLE로 Wi-Fi 자격증명 전달 (기본값: WIFI_SSID/WIFI_PASSWORD)
  claim     --service_name PROV_XXXX                          등록 -> claim 발급 -> BLE 전달 -> 결과 polling
  claim-token --service_name PROV_XXXX --claim_token T        BLE로 claimToken만 전달 (서버 호출 없음)

provision, claim, claim-token은 기기 라벨의 PIN(PoP)이 필요하다. --pop PIN 또는 환경변수 PROV_POP으로 준다.
기기가 부팅 때 시리얼 로그로 "label -> ID: ...  PIN: ..."을 출력한다.
  server-claim --device_id XXXX                               서버에서 claim만 발급하고 토큰 출력
  poll --claim_token T                                        claim 결과 polling
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ESP_PROV = os.path.join(
    TOOLS_DIR, "..", "managed_components", "espressif__network_provisioning", "tool", "esp_prov", "esp_prov.py"
)
PROV_CLIENT = os.path.join(TOOLS_DIR, "leafie_prov_client.py")


def load_env():
    path = os.path.join(TOOLS_DIR, "phone_sim.env")
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip())


def need(name):
    v = os.environ.get(name)
    if not v:
        sys.exit(f"missing config: {name} (tools/phone_sim.env)")
    return v


def http(method, url, body=None, headers=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={"Content-Type": "application/json", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            raw = r.read()
            return r.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, raw.decode(errors="replace")


def login():
    """Supabase Auth password grant로 user JWT를 받는다."""
    url = need("SUPABASE_URL").rstrip("/") + "/auth/v1/token?grant_type=password"
    status, body = http(
        "POST", url, {"email": need("TEST_EMAIL"), "password": need("TEST_PASSWORD")},
        {"apikey": need("SUPABASE_PUBLISHABLE_KEY")},
    )
    if status != 200:
        sys.exit(f"login failed: {status} {body}")
    return body["access_token"]


def api(method, path, jwt, body=None):
    return http(method, need("BACKEND_URL").rstrip("/") + "/api/v1" + path, body, {"Authorization": f"Bearer {jwt}"})


def device_id_from(service_name):
    if not service_name.startswith("PROV_"):
        sys.exit("service_name must look like PROV_<deviceId>")
    return service_name[len("PROV_"):]


def run_quiet(cmd, what):
    """실패해도 cmd(Wi-Fi 비밀번호, PIN 포함)를 출력하지 않는다."""
    if subprocess.run(cmd).returncode != 0:
        sys.exit(f"{what} failed")


def ble_claim(service_name, claim_token, pop):
    run_quiet(
        [sys.executable, PROV_CLIENT, "--service_name", service_name, "--pop", pop,
         "--endpoint", "claim", "--data", json.dumps({"claimToken": claim_token})],
        "BLE claim",
    )


def poll(jwt, claim_token, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        status, body = api("GET", f"/sensor-device-claims/{claim_token}", jwt)
        print(f"[poll] {status} {body}")
        if status == 200 and body.get("status") != "PENDING":
            return body["status"]
        if status != 200:
            return f"HTTP {status}"
        time.sleep(2)
    return "TIMEOUT"


def create_claim(jwt, device_id, register=True):
    if register:
        status, body = api("POST", "/sensor-devices", jwt, {"deviceId": device_id})
        print(f"[register] {status} {body}")
        if status not in (200, 201):
            sys.exit(1)
    status, body = api("POST", f"/sensor-devices/{device_id}/claims", jwt)
    print(f"[claim create] {status} {body}")
    if status != 200 and status != 201:
        sys.exit(1)
    return body["claimToken"]


def main():
    load_env()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("provision")
    s.add_argument("--service_name", required=True)
    s.add_argument("--pop", default=os.environ.get("PROV_POP"))
    s.add_argument("--ssid", default=os.environ.get("WIFI_SSID"))
    s.add_argument("--passphrase", default=os.environ.get("WIFI_PASSWORD"))

    s = sub.add_parser("claim")
    s.add_argument("--service_name", required=True)
    s.add_argument("--pop", default=os.environ.get("PROV_POP"))

    s = sub.add_parser("claim-token")
    s.add_argument("--service_name", required=True)
    s.add_argument("--pop", default=os.environ.get("PROV_POP"))
    s.add_argument("--claim_token", required=True)

    s = sub.add_parser("server-claim")
    s.add_argument("--device_id", required=True)

    s = sub.add_parser("poll")
    s.add_argument("--claim_token", required=True)

    a = p.parse_args()
    if a.cmd in ("provision", "claim", "claim-token") and not a.pop:
        sys.exit("missing PIN(PoP): --pop 또는 PROV_POP (기기 라벨의 PIN)")

    if a.cmd == "provision":
        if not a.ssid or a.passphrase is None:
            sys.exit("missing Wi-Fi config: WIFI_SSID / WIFI_PASSWORD (tools/phone_sim.env) or --ssid/--passphrase")
        run_quiet(
            [sys.executable, ESP_PROV, "--transport", "ble", "--service_name", a.service_name,
             "--sec_ver", "1", "--pop", a.pop, "--ssid", a.ssid, "--passphrase", a.passphrase],
            "BLE provisioning",
        )
    elif a.cmd == "claim":
        jwt = login()
        token = create_claim(jwt, device_id_from(a.service_name))
        ble_claim(a.service_name, token, a.pop)
        print(f"[result] {poll(jwt, token)}")
    elif a.cmd == "claim-token":
        ble_claim(a.service_name, a.claim_token, a.pop)
    elif a.cmd == "server-claim":
        print(create_claim(login(), a.device_id))
    elif a.cmd == "poll":
        print(f"[result] {poll(login(), a.claim_token)}")


if __name__ == "__main__":
    main()
