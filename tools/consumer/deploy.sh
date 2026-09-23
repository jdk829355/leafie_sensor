#!/usr/bin/env bash
# consumer Lambda 코드를 의존성과 함께 zip으로 묶어 배포한다.
# 사용: tools/consumer/deploy.sh
#
# supabase-ca.crt가 이 폴더에 있으면 함께 포함된다 (Supabase 대시보드 > Database > SSL Configuration).
set -euo pipefail

FUNCTION_NAME="leafie-telemetry-consumer"
REGION="ap-northeast-2"

cd "$(dirname "$0")"

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

# pg8000과 그 의존성은 모두 순수 파이썬이라 플랫폼 지정이 필요 없다. boto3는 Lambda 런타임에 있다.
pip install --quiet --requirement requirements.txt --target "$BUILD/package"
cp lambda_function.py "$BUILD/package/"
[ -f supabase-ca.crt ] && cp supabase-ca.crt "$BUILD/package/"

(cd "$BUILD/package" && zip -q -r "$BUILD/consumer.zip" .)

aws lambda update-function-code \
  --function-name "$FUNCTION_NAME" \
  --region "$REGION" \
  --zip-file "fileb://$BUILD/consumer.zip" \
  --query '{function:FunctionName,lastModified:LastModified,size:CodeSize}' \
  --output json

aws lambda wait function-updated-v2 --function-name "$FUNCTION_NAME" --region "$REGION"
echo "deployed: $FUNCTION_NAME"
