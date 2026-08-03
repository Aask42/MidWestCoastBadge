#!/bin/sh
# certsync - publish Caddy's ACME certificate to the Mosquitto container.
#
# Caddy is the only ACME client in the stack. Mosquitto needs the same
# certificate on disk to serve native MQTT over TLS on 8883, so this sidecar
# watches Caddy's certificate store and copies the leaf certificate and key
# into a small shared volume, owned by the mosquitto user.
#
# It only ever *copies*. Reloading is the Mosquitto container's job (see
# mosquitto-entrypoint.sh), which keeps this container out of Mosquitto's PID
# namespace and away from the Docker socket - it needs neither.
#
# Runs as root: it must chown the copies to the mosquitto uid.

set -eu

DOMAIN=${DOMAIN:?DOMAIN must be set}
SRC_ROOT=${SRC_ROOT:-/caddy-data/caddy/certificates}
DST=${DST:-/mqtt-certs}
OWNER_UID=${MOSQUITTO_UID:-1883}
OWNER_GID=${MOSQUITTO_GID:-1883}
INTERVAL=${SYNC_INTERVAL:-21600}
WAIT_INTERVAL=${WAIT_INTERVAL:-15}

log() { echo "certsync: $*"; }

# Caddy nests certificates under an issuer-specific directory, e.g.
# certificates/acme-v02.api.letsencrypt.org-directory/<domain>/<domain>.crt.
# Globbing the issuer keeps this working across a CA change or a staging run.
find_cert() {
	find "$SRC_ROOT" -type f -path "*/$DOMAIN/$DOMAIN.crt" 2>/dev/null | head -n 1
}

publish() {
	crt=$1
	key=${crt%.crt}.key

	if [ ! -s "$crt" ] || [ ! -s "$key" ]; then
		log "certificate material for $DOMAIN is incomplete, waiting"
		return 1
	fi

	new=$(cat "$crt" "$key" | sha256sum | cut -d' ' -f1)
	old=""
	[ -f "$DST/.checksum" ] && old=$(cat "$DST/.checksum")
	if [ "$new" = "$old" ]; then
		return 0
	fi

	# Stage then rename, so Mosquitto never reads a half-written file. The key
	# is moved into place before the certificate on purpose: the certificate
	# is what Mosquitto watches for changes, so by the time a reload can be
	# triggered the matching key is already there.
	cp "$crt" "$DST/.fullchain.pem.tmp"
	cp "$key" "$DST/.privkey.pem.tmp"
	chown "$OWNER_UID:$OWNER_GID" "$DST/.fullchain.pem.tmp" "$DST/.privkey.pem.tmp"
	chmod 644 "$DST/.fullchain.pem.tmp"
	chmod 640 "$DST/.privkey.pem.tmp"
	mv "$DST/.privkey.pem.tmp" "$DST/privkey.pem"
	mv "$DST/.fullchain.pem.tmp" "$DST/fullchain.pem"
	printf '%s\n' "$new" >"$DST/.checksum"

	log "published certificate for $DOMAIN ($(printf '%s' "$new" | cut -c1-12))"
	return 0
}

log "watching $SRC_ROOT for $DOMAIN, publishing to $DST"

while :; do
	crt=$(find_cert || true)
	if [ -n "$crt" ] && publish "$crt"; then
		sleep "$INTERVAL"
	else
		# Nothing issued yet. This is the normal state for the first minute of
		# a fresh deploy while Caddy completes the ACME challenge, so poll
		# quickly rather than making Mosquitto wait out a full interval.
		[ -n "$crt" ] || log "no certificate for $DOMAIN yet under $SRC_ROOT"
		sleep "$WAIT_INTERVAL"
	fi
done
