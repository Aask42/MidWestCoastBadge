#!/bin/sh
# Snapshot everything needed to rebuild this deployment on a new instance.
#
# Captures:
#   mosquitto_data   retained state and the persistence database
#   caddy_data       ACME account key and issued certificates. Worth keeping:
#                    restoring it avoids re-issuing and the rate limits that
#                    come with repeated re-issuance.
#   .env.production, config/passwd.production
#                    the credentials. This makes the archive secret material -
#                    it is written mode 600, and it belongs somewhere off this
#                    instance (Object Storage is free-tier eligible).
#
# Volumes are read through a throwaway container, so this works the same
# whoever owns the files inside them.
#
#   ./scripts/backup.sh              write backups/dc34-broker-<stamp>.tar.gz
#   ./scripts/backup.sh --quiet      only report the path (for cron)

set -eu

. "$(dirname -- "$0")/lib.sh"

KEEP=${BACKUP_KEEP:-14}
QUIET=no
[ "${1:-}" = "--quiet" ] && QUIET=yes
say() { [ "$QUIET" = yes ] || log "$@"; }

require_docker
require_config
cd "$BROKER_DIR"

STAMP=$(date -u '+%Y%m%d-%H%M%S')
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$BACKUP_DIR"

ARCHIVE="$BACKUP_DIR/dc34-broker-$STAMP.tar.gz"

for vol in mosquitto_data caddy_data; do
	full="dc34-broker-prod_$vol"
	if ! docker volume inspect "$full" >/dev/null 2>&1; then
		warn "volume $full does not exist yet, skipping"
		continue
	fi
	say "archiving $full"
	# --numeric-owner keeps the mosquitto uid intact across a restore onto a
	# host where that name does not exist.
	docker run --rm -v "$full:/src:ro" -v "$STAGE:/out" alpine:3.20 \
		tar czf "/out/$vol.tar.gz" --numeric-owner -C /src . 2>/dev/null
done

say "archiving credentials and configuration"
mkdir -p "$STAGE/config"
cp "$ENV_FILE" "$STAGE/env.production"
cp "$PASSWD_FILE" "$STAGE/config/passwd.production"
cp "$BROKER_DIR/Caddyfile" "$STAGE/Caddyfile"
cp "$BROKER_DIR/config/mosquitto.production.conf" "$STAGE/config/"
cp "$BROKER_DIR/config/acl.conf" "$STAGE/config/"
compose config >"$STAGE/resolved-compose.yml" 2>/dev/null || true
date -u '+%Y-%m-%dT%H:%M:%SZ' >"$STAGE/created-at"

tar czf "$ARCHIVE" -C "$STAGE" .
chmod 600 "$ARCHIVE"

# Retention. Keeps the archive count bounded on a small Always Free boot volume.
if [ "$KEEP" -gt 0 ]; then
	# shellcheck disable=SC2012
	ls -1t "$BACKUP_DIR"/dc34-broker-*.tar.gz 2>/dev/null | tail -n +$((KEEP + 1)) | while read -r old; do
		say "pruning $(basename "$old")"
		rm -f "$old"
	done
fi

if [ "$QUIET" = yes ]; then
	printf '%s\n' "$ARCHIVE"
else
	log "wrote $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1), mode 600)"
	log "this archive contains credentials - copy it off the instance and keep it private"
fi
