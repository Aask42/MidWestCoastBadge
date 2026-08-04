#!/bin/sh
# Create or rotate one production badge credential.

set -eu

. "$(dirname -- "$0")/lib.sh"

ROTATE=no
[ "${1:-}" = "--rotate" ] && ROTATE=yes && shift
BADGE_ID=$(printf '%s' "${1:-}" | tr 'A-F' 'a-f')
case "$BADGE_ID" in
????????) ;;
*) die "usage: $0 [--rotate] <8-hex-badge-id>" ;;
esac
case "$BADGE_ID" in
*[!0-9a-f]*) die "badge id must be exactly 8 hexadecimal characters" ;;
esac

require_docker
load_env

ACL_FILE="$BROKER_DIR/config/acl.production.conf"
ACL_BASE="$BROKER_DIR/config/acl.production.base.conf"
USERNAME="badge-$BADGE_ID"

[ -f "$ACL_FILE" ] || cp "$ACL_BASE" "$ACL_FILE"
if grep -q "^user $USERNAME$" "$ACL_FILE" && [ "$ROTATE" = no ]; then
	die "$BADGE_ID is already enrolled; use --rotate to issue a new password"
fi

if command -v openssl >/dev/null 2>&1; then
	PASSWORD=$(openssl rand -hex 18)
else
	PASSWORD=$(od -An -tx1 -N18 /dev/urandom | tr -d ' \n')
fi

docker run --rm --user "$(id -u):$(id -g)" \
	-v "$BROKER_DIR/config:/work" eclipse-mosquitto:2.0.20 \
	mosquitto_passwd -b /work/passwd.production "$USERNAME" "$PASSWORD"

if ! grep -q "^user $USERNAME$" "$ACL_FILE"; then
	cat >>"$ACL_FILE" <<EOF

user $USERNAME
topic read dc34/all/cmd
topic read dc34/badge/$BADGE_ID/cmd
topic read dc34/badge/$BADGE_ID/owner
topic write dc34/badge/$BADGE_ID/state
topic write dc34/badge/$BADGE_ID/telemetry
topic write dc34/badge/$BADGE_ID/wifi
EOF
fi
chmod 600 "$PASSWD_FILE" "$ACL_FILE"

if [ -n "$(compose ps -q mosquitto 2>/dev/null)" ]; then
	compose restart mosquitto >/dev/null
fi

cat <<EOF
Enrolled badge $BADGE_ID with an individually revocable broker credential.

Provision it over USB serial:

  mqtt $DOMAIN 8883 $USERNAME $PASSWORD

This password is shown once. Re-run with --rotate if it is lost or exposed.
EOF