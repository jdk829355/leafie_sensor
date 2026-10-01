"""API Gateway(REST) Lambda Authorizer: 기기별 telemetry 인증.

POST /devices/{deviceId}/telemetry 요청의 `Authorization: Bearer <deviceToken>`을 검증한다.
백엔드는 deviceToken의 SHA-256 hex를 sensor_devices.sensor_token_hash에 저장한다.
경로의 deviceId 기기가 CLAIMED이고 해시가 일치하면 Allow, 아니면 Deny한다.

Authorizer 유형은 REQUEST, Identity source는 `method.request.header.Authorization`과
`method.request.path.device_id`로 설정해 결과를 (토큰, 기기) 단위로 캐시한다.

접속 문자열은 SSM Parameter Store(SecureString)에서 읽는다. 환경변수 DATABASE_URL_PARAM에
파라미터 이름을 넣는다. 값 형식은 consumer와 같다 (Supavisor 풀러 주소).

    postgresql://sensor_authorizer.<project-ref>:<password>@<pooler-host>:5432/postgres

DB 역할 sensor_authorizer는 sensor_devices의 id, status, sensor_token_hash만 SELECT할 수 있다.
"""

import hashlib
import hmac
import logging
import os
import ssl
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import pg8000.native

logger = logging.getLogger()
logger.setLevel(logging.INFO)

SUPABASE_CA_FILE = Path(__file__).with_name("supabase-ca.crt")

SELECT_SQL = "SELECT sensor_token_hash, status FROM sensor_devices WHERE id = :device_id"

_database_url = None


def _load_database_url() -> str:
    # 콜드 스타트 때 한 번만 읽는다.
    global _database_url
    if _database_url is None:
        import boto3

        response = boto3.client("ssm").get_parameter(
            Name=os.environ["DATABASE_URL_PARAM"], WithDecryption=True
        )
        _database_url = response["Parameter"]["Value"]
    return _database_url


def _connect() -> pg8000.native.Connection:
    url = urlparse(_load_database_url())
    ssl_context = None
    if parse_qs(url.query).get("sslmode") != ["disable"]:
        ssl_context = ssl.create_default_context(
            cafile=str(SUPABASE_CA_FILE) if SUPABASE_CA_FILE.exists() else None
        )
        # Python 3.13의 엄격 검증은 key usage 확장이 없는 Supabase 루트 CA를 거부한다.
        # CA와 호스트명 검증은 그대로 유지하고 이 플래그만 끈다.
        ssl_context.verify_flags &= ~ssl.VERIFY_X509_STRICT
    return pg8000.native.Connection(
        user=unquote(url.username),
        password=unquote(url.password),
        host=url.hostname,
        port=url.port or 5432,
        database=url.path.lstrip("/"),
        ssl_context=ssl_context,
        timeout=5,
    )


def _is_valid(device_id: str, token: str) -> bool:
    connection = _connect()
    try:
        rows = connection.run(SELECT_SQL, device_id=device_id)
    finally:
        connection.close()
    if not rows:
        return False
    token_hash, status = rows[0]
    if status != "CLAIMED" or token_hash is None:
        return False
    return hmac.compare_digest(hashlib.sha256(token.encode()).hexdigest(), token_hash)


def _policy(effect: str, method_arn: str, principal: str) -> dict:
    # 캐시된 결과가 다른 경로에도 쓰이지 않도록 이 요청의 메서드 ARN만 허용한다.
    return {
        "principalId": principal,
        "policyDocument": {
            "Version": "2012-10-17",
            "Statement": [
                {"Action": "execute-api:Invoke", "Effect": effect, "Resource": method_arn}
            ],
        },
    }


def handler(event, context):
    method_arn = event["methodArn"]
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    device_id = (event.get("pathParameters") or {}).get("device_id", "")

    scheme, _, token = headers.get("authorization", "").partition(" ")
    if scheme != "Bearer" or not token or not device_id:
        return _policy("Deny", method_arn, "anonymous")

    try:
        allowed = _is_valid(device_id, token)
    except Exception:
        # DB 장애는 Deny가 아니라 오류로 내려 500이 되게 한다 (캐시되지 않는다).
        logger.exception("authorizer lookup failed deviceId=%s", device_id)
        raise

    logger.info("deviceId=%s %s", device_id, "allow" if allowed else "deny")
    return _policy("Allow" if allowed else "Deny", method_arn, device_id)
