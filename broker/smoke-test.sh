#!/bin/sh
set -eu

cd "$(dirname "$0")"
. ./.env

docker compose exec -T mosquitto mosquitto_sub -h 127.0.0.1 \
  -u web -P "$WEB_PASSWORD" -t dc34/claim/test -C 1 -W 5 &
SUB_PID=$!
sleep 1
docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 \
  -u badge -P "$BADGE_PASSWORD" -t dc34/claim/test -m ok
wait "$SUB_PID"

DENIED_OUT=$(mktemp)
trap 'rm -f "$DENIED_OUT"' EXIT
docker compose exec -T mosquitto mosquitto_sub -h 127.0.0.1 \
  -u operator -P "$OPERATOR_PASSWORD" -t dc34/all/cmd -C 1 -W 2 \
  >"$DENIED_OUT" 2>/dev/null &
DENIED_SUB_PID=$!
sleep 1
docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 \
  -u web -P "$WEB_PASSWORD" -t dc34/all/cmd -m denied
if wait "$DENIED_SUB_PID" || [ -s "$DENIED_OUT" ]; then
  echo "FAIL: web credential reached operator command topic" >&2
  exit 1
fi

docker compose exec -T mosquitto mosquitto_pub -h 127.0.0.1 \
  -u web -P "$WEB_PASSWORD" -t dc34/badge/test/owner -m allowed

echo "PASS: claim exchange works and web operator access is denied"