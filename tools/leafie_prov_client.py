#!/usr/bin/env python3
"""Send a raw string to an arbitrary protocomm custom endpoint (e.g. "device-info", "claim").

esp_prov.py's --custom_data flag is hardcoded to the endpoint name "custom-data", so it can't
reach the "device-info"/"claim" endpoints this firmware registers. This script reuses esp_prov's
session-establishment code but lets you pick the endpoint name and payload freely.

Usage:
  python3 leafie_prov_client.py --service_name PROV_D40592E7D168 --pop leafie_pop \
      --endpoint claim --data '{"claimToken": "SUCCESS-abcd1234"}'
"""

import argparse
import asyncio
import os
import sys

_NETWORK_PROV_TOOL_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "managed_components", "espressif__network_provisioning", "tool", "esp_prov",
)
sys.path.insert(0, _NETWORK_PROV_TOOL_DIR)

import prov  # noqa: E402
import security  # noqa: E402
import transport  # noqa: E402


async def establish_session(tp, sec):
    response = None
    while True:
        request = sec.security_session(response)
        if request is None:
            return True
        response = await tp.send_data("prov-session", request)
        if response is None:
            return False


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--service_name", required=True, help="BLE device name (e.g. PROV_<deviceId>)")
    parser.add_argument("--pop", default="", help="Proof of possession for security version 1")
    parser.add_argument("--sec_ver", type=int, default=1, choices=[0, 1], help="Protocomm security version")
    parser.add_argument("--endpoint", required=True, help='Custom endpoint name, e.g. "device-info" or "claim"')
    parser.add_argument("--data", default="", help="Raw string payload to send (plain text, e.g. a JSON string)")
    args = parser.parse_args()

    tp = transport.Transport_BLE(service_uuid="021a9004-0382-4aea-bff4-6b3f1c5adfb4", nu_lookup={})
    await tp.connect(devname=args.service_name)

    sec = security.Security1(args.pop, False) if args.sec_ver == 1 else security.Security0(False)

    if not await establish_session(tp, sec):
        print("Failed to establish session")
        return

    message = prov.custom_data_request(sec, args.data)
    response = await tp.send_data(args.endpoint, message)
    prov.custom_data_response(sec, response)

    await tp.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
