#!/bin/sh
# Production entrypoint for Mosquitto: wait for TLS material, run the broker,
# and pick up renewed certificates.
#
# Two problems this solves, both consequences of Caddy owning ACME:
#
#   1. On a first deploy the certificate does not exist yet. Mosquitto's TLS
#      listener would fail to start and the container would crash-loop until
#      the ACME challenge completed. Waiting here turns that into a quiet
#      "not ready yet" instead of a restart storm.
#
#   2. Certificates are replaced roughly every 30 days, and a broker still
#      serving the old one goes dark for every badge the moment it expires.
#
# On (2): SIGHUP is NOT enough. Mosquitto 2.0.20 rereads its configuration on
# SIGHUP but keeps serving the certificate the listener started with - this was
# measured against this exact image, both via SIGHUP and by signalling the
# process by hand, and only a process restart swapped the certificate. So the
# broker is supervised here and restarted in place when the certificate
# changes. That is roughly one sub-second blip every 30 days, on a connection
# the badge firmware already reconnects to with backoff.
#
# Doing the supervision inside this container means the certsync sidecar needs
# neither the Docker socket nor a shared PID namespace, and the restart never
# involves the Docker restart policy.

set -u

CONF=${CONF:-/mosquitto/config/mosquitto.production.conf}
CERT=${CERT:-/mosquitto/certs/fullchain.pem}
KEY=${KEY:-/mosquitto/certs/privkey.pem}

# How often to re-hash the certificate. Renewals happen monthly, so the default
# is unhurried; the liveness poll below is what stays responsive.
WATCH_INTERVAL=${CERT_WATCH_INTERVAL:-300}
# Also bounds how long shutdown waits: a trap runs after the current sleep.
POLL=2

log() { echo "entrypoint: $*"; }

cert_fingerprint() {
	sha256sum "$CERT" "$KEY" 2>/dev/null | sha256sum | cut -d' ' -f1
}

wait_for_cert() {
	waited=0
	while [ ! -s "$CERT" ] || [ ! -s "$KEY" ]; do
		if [ "$((waited % 60))" -eq 0 ]; then
			log "waiting for certsync to publish $CERT"
		fi
		sleep 5
		waited=$((waited + 5))
	done
	[ "$waited" -eq 0 ] || log "TLS material appeared after ${waited}s"
}

start_broker() {
	/usr/sbin/mosquitto -c "$CONF" &
	MOSQ_PID=$!
	CERT_FP=$(cert_fingerprint)
	log "broker started (pid $MOSQ_PID)"
}

STOPPING=no
shutdown() {
	STOPPING=yes
	log "received shutdown signal, stopping broker"
	kill -TERM "$MOSQ_PID" 2>/dev/null || true
}
trap shutdown TERM INT

wait_for_cert
start_broker

since_check=0
while :; do
	sleep "$POLL"

	# Did the broker die on its own? Propagate its status so the container's
	# restart policy takes over rather than leaving a supervisor with no
	# broker under it.
	if ! kill -0 "$MOSQ_PID" 2>/dev/null; then
		wait "$MOSQ_PID"
		status=$?
		if [ "$STOPPING" = yes ]; then
			log "broker stopped cleanly"
			exit 0
		fi
		log "broker exited unexpectedly with status $status"
		exit "$status"
	fi
	[ "$STOPPING" = no ] || continue

	since_check=$((since_check + POLL))
	[ "$since_check" -ge "$WATCH_INTERVAL" ] || continue
	since_check=0

	[ -s "$CERT" ] && [ -s "$KEY" ] || continue
	now=$(cert_fingerprint)
	[ "$now" = "$CERT_FP" ] && continue

	log "certificate renewed, restarting broker to load it"
	kill -TERM "$MOSQ_PID" 2>/dev/null || true
	wait "$MOSQ_PID" 2>/dev/null
	wait_for_cert
	start_broker
done
