#!/bin/sh
set -eu

cd "$(dirname "$0")"
mkdir -p config data log

if [ -f .env ] && [ -f config/passwd ]; then
  echo "broker credentials already exist"
  exit 0
fi

random_password() {
  openssl rand -hex 18
}

BADGE_PASSWORD=$(random_password)
WEB_PASSWORD=$(random_password)
OPERATOR_PASSWORD=$(random_password)
HEALTH_PASSWORD=$(random_password)

cat >.env <<EOF
BADGE_PASSWORD=$BADGE_PASSWORD
WEB_PASSWORD=$WEB_PASSWORD
OPERATOR_PASSWORD=$OPERATOR_PASSWORD
HEALTH_PASSWORD=$HEALTH_PASSWORD
EOF
chmod 600 .env

rm -f config/passwd
docker run --rm -v "$PWD/config:/work" eclipse-mosquitto:2.0.20 \
  mosquitto_passwd -b -c /work/passwd badge "$BADGE_PASSWORD"
docker run --rm -v "$PWD/config:/work" eclipse-mosquitto:2.0.20 \
  mosquitto_passwd -b /work/passwd web "$WEB_PASSWORD"
docker run --rm -v "$PWD/config:/work" eclipse-mosquitto:2.0.20 \
  mosquitto_passwd -b /work/passwd operator "$OPERATOR_PASSWORD"
docker run --rm -v "$PWD/config:/work" eclipse-mosquitto:2.0.20 \
  mosquitto_passwd -b /work/passwd health "$HEALTH_PASSWORD"
chmod 600 config/passwd

echo "generated broker/.env and broker/config/passwd"
echo "keep .env private; it is ignored by git"