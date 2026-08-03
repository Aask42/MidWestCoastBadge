#!/bin/sh
# Update the stack, with a backup taken first and an automatic rollback if the
# new containers do not pass validation.
#
# Images are pinned by tag, so this only moves if a tag has been re-published
# (caddy:2.10-alpine gets patch updates) or if you have edited the tags in
# compose.production.yml. Either way the digests running *before* the update
# are recorded first, which is what makes the rollback exact rather than
# "whatever :latest happens to be today".
#
#   ./scripts/update.sh                update, validate, roll back on failure
#   ./scripts/update.sh --no-rollback  leave the new version up for debugging
#   ./scripts/update.sh --no-backup    skip the pre-update snapshot

set -eu

. "$(dirname -- "$0")/lib.sh"

DO_ROLLBACK=yes
DO_BACKUP=yes
for arg in "$@"; do
	case "$arg" in
	--no-rollback) DO_ROLLBACK=no ;;
	--no-backup) DO_BACKUP=no ;;
	*) die "unknown option: $arg" ;;
	esac
done

require_docker
load_env
cd "$BROKER_DIR"

# --- Record what is running now ---------------------------------------------
log "recording current image digests"
: >"$ROLLBACK_STATE"
for svc in caddy mosquitto certsync; do
	cid=$(compose ps -q "$svc" 2>/dev/null || true)
	[ -n "$cid" ] || {
		warn "$svc is not running; it will not be part of a rollback"
		continue
	}
	imgid=$(docker inspect -f '{{.Image}}' "$cid")
	# Prefer a repo digest: it survives an image prune and can be re-pulled.
	ref=$(docker inspect -f '{{if .RepoDigests}}{{index .RepoDigests 0}}{{end}}' "$imgid" 2>/dev/null || true)
	[ -n "$ref" ] || ref=$imgid
	printf '%s=%s\n' "$svc" "$ref" >>"$ROLLBACK_STATE"
	log "  $svc $ref"
done

if [ ! -s "$ROLLBACK_STATE" ]; then
	rm -f "$ROLLBACK_STATE"
	die "nothing is running - use scripts/deploy.sh for a first deploy"
fi

# --- Snapshot ---------------------------------------------------------------
if [ "$DO_BACKUP" = yes ]; then
	log "taking a pre-update backup"
	"$BROKER_DIR/scripts/backup.sh"
else
	warn "skipping the pre-update backup"
fi

# --- Update -----------------------------------------------------------------
log "pulling images"
compose pull

log "recreating containers"
compose up -d

log "waiting for containers to report healthy"
HEALTHY=yes
wait_healthy caddy 120 || {
	warn "caddy did not report healthy"
	HEALTHY=no
}
wait_healthy mosquitto 240 || {
	warn "mosquitto did not report healthy"
	HEALTHY=no
}

# --- Validate, and undo if it went wrong ------------------------------------
VALID=yes
if [ "$HEALTHY" = yes ]; then
	"$BROKER_DIR/scripts/healthcheck.sh" || VALID=no
else
	VALID=no
fi

if [ "$VALID" = yes ]; then
	log "update complete and validated"
	compose ps
	exit 0
fi

if [ "$DO_ROLLBACK" = no ]; then
	warn "validation failed, but --no-rollback was given. The new version is still up."
	warn "Roll back manually with: ./scripts/rollback.sh"
	exit 1
fi

warn "validation failed - rolling back to the previously recorded digests"
"$BROKER_DIR/scripts/rollback.sh" --from-update
die "update failed and was rolled back; see the healthcheck output above"
