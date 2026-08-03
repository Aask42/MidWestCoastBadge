#!/bin/sh
# Bring the production stack up, in the order the certificate flow requires.
#
# Mosquitto's TLS listener cannot start without a certificate, and the
# certificate does not exist until Caddy has completed an ACME challenge. So
# Caddy and certsync go first, then Mosquitto once the material has landed.
# (Mosquitto's entrypoint waits anyway - this just makes a first deploy report
# progress instead of sitting silently.)
#
# Safe to re-run: it is the normal way to apply a config change.

set -eu

. "$(dirname -- "$0")/lib.sh"

CERT_WAIT=${CERT_WAIT:-300}

require_docker
load_env
cd "$BROKER_DIR"

# --- Preflight --------------------------------------------------------------
log "preflight"

if ! docker compose -f "$COMPOSE_FILE" --env-file "$ENV_FILE" config >/dev/null; then
	die "compose.production.yml did not validate"
fi

# DNS has to be right before ACME will issue anything. This is advisory: split
# horizon DNS and proxied setups are legitimate, so it warns rather than stops.
if command -v getent >/dev/null 2>&1; then
	RESOLVED=$(getent ahostsv4 "$DOMAIN" 2>/dev/null | awk 'NR==1{print $1}')
else
	RESOLVED=$(nslookup "$DOMAIN" 2>/dev/null | awk '/^Address: /{print $2; exit}')
fi
PUBLIC_IP=$(curl -fsS --max-time 5 https://api.ipify.org 2>/dev/null || echo '')
if [ -z "$RESOLVED" ]; then
	warn "$DOMAIN does not resolve yet - ACME will fail until the A record exists"
elif [ -n "$PUBLIC_IP" ] && [ "$RESOLVED" != "$PUBLIC_IP" ]; then
	warn "$DOMAIN resolves to $RESOLVED but this host's public IP is $PUBLIC_IP"
	warn "ACME will fail unless that is deliberate (proxy, split horizon, or a reserved IP not yet attached)"
else
	log "$DOMAIN resolves to $RESOLVED"
fi

# Port 80 and 443 must be free and, crucially, reachable. Reachability is an
# Oracle-specific trap: the VCN security list AND the instance firewall both
# have to allow them. See DEPLOYMENT.md.
for port in 80 443 8883; do
	if command -v ss >/dev/null 2>&1 && ss -ltn "sport = :$port" 2>/dev/null | grep -q LISTEN; then
		if ! docker ps --format '{{.Ports}}' | grep -q ":$port->"; then
			warn "something other than this stack is already listening on $port"
		fi
	fi
done

case "${ACME_CA:-}" in
*staging*) warn "ACME_CA points at the Let's Encrypt staging CA - certificates will NOT be publicly trusted" ;;
esac
[ -z "${TLS_DIRECTIVE:-}" ] || warn "TLS_DIRECTIVE is set to '${TLS_DIRECTIVE}' - automatic ACME is overridden"

# --- Pull and start ---------------------------------------------------------
log "pulling images"
compose pull --quiet

log "starting caddy and certsync"
compose up -d caddy certsync

log "waiting for the ACME certificate (up to ${CERT_WAIT}s)"
waited=0
while [ "$waited" -lt "$CERT_WAIT" ]; do
	if compose exec -T certsync test -s /mqtt-certs/fullchain.pem 2>/dev/null; then
		log "certificate published after ${waited}s"
		break
	fi
	sleep 5
	waited=$((waited + 5))
done
if [ "$waited" -ge "$CERT_WAIT" ]; then
	warn "no certificate after ${CERT_WAIT}s. Mosquitto will keep waiting and start on its own once one arrives."
	warn "Check: docker compose -f compose.production.yml --env-file .env.production logs caddy"
	warn "Most common causes: port 80 blocked by the VCN security list or the instance firewall, or DNS not pointing here."
fi

log "starting the broker"
compose up -d

log "waiting for containers to report healthy"
wait_healthy caddy 120 || warn "caddy did not report healthy"
wait_healthy mosquitto 240 || warn "mosquitto did not report healthy"

compose ps

log "running healthcheck"
"$BROKER_DIR/scripts/healthcheck.sh" "$@"
