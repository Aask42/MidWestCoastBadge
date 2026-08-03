#!/bin/sh
# Restore a backup produced by backup.sh, onto this instance or a fresh one.
#
#   ./scripts/restore.sh backups/dc34-broker-20260803-120000.tar.gz
#
# Stops the stack, replaces the contents of the data volumes and the credential
# files, then brings everything back up and validates it. Destructive by
# definition, so it asks first unless --yes is given.

set -eu

. "$(dirname -- "$0")/lib.sh"

ARCHIVE=${1:-}
ASSUME_YES=no
[ "${2:-}" = "--yes" ] && ASSUME_YES=yes

[ -n "$ARCHIVE" ] || die "usage: $0 <archive.tar.gz> [--yes]"
[ -f "$ARCHIVE" ] || die "no such archive: $ARCHIVE"
# shellcheck disable=SC1007  # CDPATH= is a prefix assignment for cd
ARCHIVE=$(CDPATH= cd -- "$(dirname -- "$ARCHIVE")" && pwd)/$(basename -- "$ARCHIVE")

require_docker
cd "$BROKER_DIR"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
tar xzf "$ARCHIVE" -C "$STAGE"

[ -f "$STAGE/created-at" ] && log "archive created $(cat "$STAGE/created-at")"

if [ "$ASSUME_YES" = no ]; then
	printf 'This replaces retained broker state, certificates and credentials on this host. Continue? [y/N] '
	read -r answer
	case "$answer" in
	y | Y | yes) ;;
	*) die "aborted" ;;
	esac
fi

log "stopping the stack"
compose down

for vol in mosquitto_data caddy_data; do
	[ -f "$STAGE/$vol.tar.gz" ] || {
		warn "$vol is not in this archive, leaving it alone"
		continue
	}
	full="dc34-broker-prod_$vol"
	log "restoring $full"
	docker volume create "$full" >/dev/null
	docker run --rm -v "$full:/dst" -v "$STAGE:/in:ro" alpine:3.20 \
		sh -c 'rm -rf /dst/..?* /dst/.[!.]* /dst/* 2>/dev/null; tar xzf "/in/'"$vol"'.tar.gz" --numeric-owner -C /dst'
done

if [ -f "$STAGE/env.production" ]; then
	log "restoring .env.production"
	cp "$STAGE/env.production" "$ENV_FILE"
	chmod 600 "$ENV_FILE"
fi
if [ -f "$STAGE/config/passwd.production" ]; then
	log "restoring config/passwd.production"
	cp "$STAGE/config/passwd.production" "$PASSWD_FILE"
	chmod 600 "$PASSWD_FILE"
fi

log "starting the stack"
compose up -d
wait_healthy mosquitto 240 || warn "mosquitto did not report healthy"

"$BROKER_DIR/scripts/healthcheck.sh" --local
log "restore complete - re-run scripts/healthcheck.sh once DNS points at this host"
