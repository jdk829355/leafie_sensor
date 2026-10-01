"""SQS(leafie_telemetry) consumer.

API Gateway가 SQS에 넣은 메시지를 받아 Supabase PostgreSQL `sensor_readings`에 저장한다.
메시지 형태: {"deviceId": "...", "payload": {"created_at": ..., "lux": ..., "soilRaw": ...}}

접속 문자열은 SSM Parameter Store(SecureString)에서 읽는다. 환경변수 DATABASE_URL_PARAM에
파라미터 이름을 넣는다. 값 형식은 다음과 같다 (Supavisor 풀러 주소를 쓴다).

    postgresql://sensor_ingest.<project-ref>:<password>@<pooler-host>:5432/postgres

DB 역할 sensor_ingest는 sensor_readings INSERT만 할 수 있다. 그래서 중복 방지는
`ON CONFLICT DO NOTHING`(충돌 대상 없이)으로 한다. 대상을 지정하면 SELECT 권한이 필요하다.
"""

import json
import logging
import os
import ssl
from datetime import UTC, datetime
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

import pg8000.native

logger = logging.getLogger()
logger.setLevel(logging.INFO)

# Supabase는 자체 CA로 서명한 인증서를 쓴다. 대시보드에서 받은 CA 파일을 이 이름으로 함께 배포한다.
SUPABASE_CA_FILE = Path(__file__).with_name("supabase-ca.crt")

INSERT_SQL = """
INSERT INTO sensor_readings (device_id, sqs_message_id, measured_at, received_at, lux, soil_raw)
VALUES (:device_id, CAST(:message_id AS uuid), :measured_at, :received_at, :lux, :soil_raw)
ON CONFLICT DO NOTHING
"""

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
        # Python 3.13의 엄격 검증은 서버가 보내는 Supabase 중간 CA(key usage 확장 없음)를 거부한다.
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


def _store(connection: pg8000.native.Connection, record: dict) -> None:
    message = json.loads(record["body"])
    payload = message["payload"]

    created_at = payload["created_at"]
    measured_at = (
        datetime.strptime(created_at, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=UTC)
        if created_at is not None
        else None
    )
    # SentTimestamp = SQS에 들어간 시각(API Gateway 수신 시각). 큐가 밀려도 수신 순서를 유지한다.
    received_at = datetime.fromtimestamp(int(record["attributes"]["SentTimestamp"]) / 1000, tz=UTC)

    connection.run(
        INSERT_SQL,
        device_id=message["deviceId"],
        message_id=record["messageId"],
        measured_at=measured_at,
        received_at=received_at,
        lux=payload["lux"],
        soil_raw=payload["soilRaw"],
    )
    if connection.row_count == 0:
        logger.info("duplicate messageId=%s ignored", record["messageId"])
    else:
        logger.info(
            "stored deviceId=%s received_at=%s", message["deviceId"], received_at.isoformat()
        )


def handler(event, context):
    records = event["Records"]
    try:
        connection = _connect()
    except Exception:
        logger.exception("database connection failed")
        return {"batchItemFailures": [{"itemIdentifier": r["messageId"]} for r in records]}

    # 실패한 레코드만 재시도되도록 batchItemFailures로 알려준다 (ReportBatchItemFailures).
    # 등록되지 않은 deviceId처럼 다시 시도해도 실패하는 메시지는 maxReceiveCount 후 DLQ로 간다.
    failures = []
    try:
        for record in records:
            try:
                _store(connection, record)
            except Exception:
                logger.exception("failed to store messageId=%s", record["messageId"])
                failures.append({"itemIdentifier": record["messageId"]})
    finally:
        connection.close()

    return {"batchItemFailures": failures}
