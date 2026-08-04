# Shared helpers for the production scripts. Sourced, not executed.
#
# Every script in this directory works from broker/ regardless of where it is
# invoked from, so cron entries and half-remembered paths behave the same.

# CDPATH= is a prefix assignment for cd, not a typo: without it a stray CDPATH
# in the operator's environment can make cd print, and resolve, the wrong path.
# shellcheck disable=SC1007
BROKER_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Consumed by the scripts that source this file.
# shellcheck disable=SC2034
{
	COMPOSE_FILE="$BROKER_DIR/compose.production.yml"
	ENV_FILE="$BROKER_DIR/.env.production"
	PASSWD_FILE="$BROKER_DIR/config/passwd.production"
	ROLLBACK_STATE="$BROKER_DIR/.rollback-state"
	ROLLBACK_FILE="$BROKER_DIR/compose.rollback.yml"
	BACKUP_DIR="$BROKER_DIR/backups"
}

log() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

compose() {
	docker compose -f "$COMPOSE_FILE" --env-file "$ENV_FILE" "$@"
}

# Same stack, with the recorded image digests pinned over the top.
compose_rollback() {
	docker compose -f "$COMPOSE_FILE" -f "$ROLLBACK_FILE" --env-file "$ENV_FILE" "$@"
}

require_docker() {
	command -v docker >/dev/null 2>&1 || die "docker is not installed - see DEPLOYMENT.md"
	docker compose version >/dev/null 2>&1 ||
		die "docker compose v2 is required (the 'docker compose' subcommand, not docker-compose)"
	docker info >/dev/null 2>&1 ||
		die "cannot talk to the Docker daemon - is it running, and is your user in the docker group?"
}

require_config() {
	[ -f "$ENV_FILE" ] ||
		die "$ENV_FILE is missing - copy .env.production.example and run scripts/setup-production.sh"
	[ -f "$PASSWD_FILE" ] ||
		die "$PASSWD_FILE is missing - run scripts/setup-production.sh"
}

# Read KEY=VALUE pairs out of an env file into the environment.
#
# Deliberately parsed rather than sourced. Sourcing would execute the file,
# which turns a value containing a space - TLS_DIRECTIVE="tls internal" is the
# obvious one - into a command, and makes a config file a code path. This
# matches how docker compose reads --env-file: comments and blank lines are
# skipped, and one optional layer of surrounding quotes is stripped.
parse_env() {
	file=$1
	[ -f "$file" ] || die "$file is missing"
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
		'' | '#'*) continue ;;
		*=*) ;;
		*) continue ;;
		esac
		key=${line%%=*}
		val=${line#*=}
		# Ignore anything that is not a plain shell-safe identifier.
		case "$key" in
		'' | *[!A-Za-z0-9_]*) continue ;;
		esac
		case "$val" in
		'"'*'"') val=${val#\"} && val=${val%\"} ;;
		"'"*"'") val=${val#\'} && val=${val%\'} ;;
		esac
		export "$key=$val"
	done <"$file"
}

load_env() {
	require_config
	parse_env "$ENV_FILE"
	[ -n "${DOMAIN:-}" ] || die "DOMAIN is not set in $ENV_FILE"
	case "$DOMAIN" in
	badge.example.com | *.example.com | example.com)
		die "DOMAIN is still the placeholder ($DOMAIN) - set your real hostname in $ENV_FILE"
		;;
	esac
}

# Wait for a compose service to report healthy. Returns non-zero on timeout.
wait_healthy() {
	service=$1
	timeout=${2:-180}
	waited=0
	while [ "$waited" -lt "$timeout" ]; do
		cid=$(compose ps -q "$service" 2>/dev/null)
		if [ -n "$cid" ]; then
			state=$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$cid" 2>/dev/null || echo unknown)
			case "$state" in
			healthy | running) return 0 ;;
			esac
		fi
		sleep 5
		waited=$((waited + 5))
	done
	return 1
}
