#!/bin/sh
# End-to-end validation of the production deployment.
#
# Checks the things that actually break in the field: an expired or untrusted
# certificate, a port that got exposed by accident, a broken WSS path after a
# Caddy change, and an ACL that stopped separating the public web credential
# from operator authority.
#
# Exits non-zero if any check fails, so update.sh can use it as a gate and cron
# can use it as a monitor.
#
#   ./scripts/healthcheck.sh            full check against the public hostname
#   ./scripts/healthcheck.sh --local    skip checks that require the public
#                                       endpoint to be reachable from this host

set -u

. "$(dirname -- "$0")/lib.sh"

LOCAL_ONLY=no
[ "${1:-}" = "--local" ] && LOCAL_ONLY=yes

# Days of certificate life below which the deployment is considered at risk.
# Let's Encrypt renews at 30 days remaining, so 14 means renewal has been
# failing for a fortnight and someone needs to look.
CERT_MIN_DAYS=${CERT_MIN_DAYS:-14}

PASS=0
FAIL=0
ok() {
	PASS=$((PASS + 1))
	printf '  PASS  %s\n' "$*"
}
bad() {
	FAIL=$((FAIL + 1))
	printf '  FAIL  %s\n' "$*"
}
skip() { printf '  SKIP  %s\n' "$*"; }
section() { printf '\n%s\n' "$*"; }

require_docker
load_env

printf 'Validating dc34 broker at %s\n' "$DOMAIN"

# --- Containers -------------------------------------------------------------
section 'containers'
for svc in caddy mosquitto certsync; do
	cid=$(compose ps -q "$svc" 2>/dev/null)
	if [ -z "$cid" ]; then
		bad "$svc is not running"
		continue
	fi
	state=$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$cid")
	case "$state" in
	healthy | running) ok "$svc is $state" ;;
	*) bad "$svc is $state" ;;
	esac
done

# --- Port exposure ----------------------------------------------------------
# The whole point of the design is that Mosquitto's plain listeners are not
# reachable. Compare the published set against the intended one rather than
# probing, so the check is exact and does not depend on where it runs from.
section 'published ports'
PUBLISHED=$(docker ps --filter "label=com.docker.compose.project=dc34-broker-prod" \
	--format '{{.Ports}}' | tr ',' '\n' | grep -o '0\.0\.0\.0:[0-9]*' | cut -d: -f2 | sort -un | tr '\n' ' ')
EXPECTED='80 443 8883 '
if [ "$PUBLISHED" = "$EXPECTED" ]; then
	ok "exactly 80, 443, 8883 are published"
else
	bad "published ports are [$PUBLISHED], expected [$EXPECTED]"
fi
for port in 1883 9001; do
	case " $PUBLISHED " in
	*" $port "*) bad "$port is published to the host - it must stay private" ;;
	*) ok "$port is not published" ;;
	esac
done

# --- Broker auth, ACL and TLS listener, from inside the container ------------
section 'broker'
mq() { compose exec -T mosquitto "$@"; }

if mq mosquitto_sub -h 127.0.0.1 -p 1883 -u health -P "$HEALTH_PASSWORD" \
	-t '$SYS/broker/uptime' -C 1 -W 5 >/dev/null 2>&1; then
	ok "authenticated MQTT round trip on the loopback listener"
else
	bad "could not read \$SYS/broker/uptime as the health user"
fi

if mq mosquitto_sub -h 127.0.0.1 -p 1883 -t '$SYS/broker/uptime' -C 1 -W 3 >/dev/null 2>&1; then
	bad "anonymous access is allowed"
else
	ok "anonymous access is refused"
fi

# Internal TLS listener check: real handshake, public chain, and credential.
# Hostname verification is intentionally deferred to the public DOMAIN check
# below because this connection targets container loopback.
if mq mosquitto_sub -h 127.0.0.1 -p 8883 \
	--cafile /etc/ssl/certs/ca-certificates.crt --insecure \
	-u health -P "$HEALTH_PASSWORD" -t '$SYS/broker/uptime' -C 1 -W 5 >/dev/null 2>&1; then
	ok "MQTT over TLS on 8883 accepts authenticated clients against a publicly trusted chain"
elif [ -n "${TLS_DIRECTIVE:-}" ]; then
	skip "MQTT over TLS on 8883 (TLS_DIRECTIVE is set, so the chain is not publicly trusted by design)"
else
	bad "MQTT over TLS on 8883 rejected an authenticated connection"
fi

# Mirrors smoke-test.sh: the credential that ships in the static site must not
# reach fleet command topics. A publish denied by the ACL still looks like a
# success to the publisher - the broker drops it silently - so the subscriber
# has to be listening first for this to prove anything.
DENIED_OUT=$(mktemp)
mq mosquitto_sub -h 127.0.0.1 -p 1883 -u operator -P "$OPERATOR_PASSWORD" \
	-t 'dc34/all/cmd' -C 1 -W 4 >"$DENIED_OUT" 2>/dev/null &
DENIED_PID=$!
sleep 1
mq mosquitto_pub -h 127.0.0.1 -p 1883 -u web -P "$WEB_PASSWORD" \
	-t 'dc34/all/cmd' -m denied >/dev/null 2>&1 || true
if wait "$DENIED_PID" 2>/dev/null && [ -s "$DENIED_OUT" ]; then
	bad "the web credential reached dc34/all/cmd"
else
	ok "the web credential cannot publish to dc34/all/cmd"
fi
rm -f "$DENIED_OUT"

# The same exchange over a topic the web credential legitimately owns, to prove
# the check above is measuring the ACL and not a broken test.
ALLOWED_OUT=$(mktemp)
mq mosquitto_sub -h 127.0.0.1 -p 1883 -u operator -P "$OPERATOR_PASSWORD" \
	-t 'dc34/badge/healthcheck/owner' -C 1 -W 4 >"$ALLOWED_OUT" 2>/dev/null &
ALLOWED_PID=$!
sleep 1
mq mosquitto_pub -h 127.0.0.1 -p 1883 -u web -P "$WEB_PASSWORD" \
	-t 'dc34/badge/healthcheck/owner' -m allowed >/dev/null 2>&1 || true
if wait "$ALLOWED_PID" 2>/dev/null && [ -s "$ALLOWED_OUT" ]; then
	ok "the web credential still reaches its own owner topic"
else
	bad "the web credential could not reach dc34/badge/+/owner - the ACL is too strict"
fi
rm -f "$ALLOWED_OUT"

# --- Certificate ------------------------------------------------------------
section 'certificate'
CERT_PEM=$(mq cat /mosquitto/certs/fullchain.pem 2>/dev/null)
KEY_PEM=$(mq cat /mosquitto/certs/privkey.pem 2>/dev/null)
if [ -z "$CERT_PEM" ]; then
	bad "no certificate published to the broker yet (is certsync running? has ACME completed?)"
elif ! command -v openssl >/dev/null 2>&1; then
	skip "openssl not installed on this host; cannot inspect the certificate"
else
	SUBJECT=$(printf '%s' "$CERT_PEM" | openssl x509 -noout -subject 2>/dev/null)
	ISSUER=$(printf '%s' "$CERT_PEM" | openssl x509 -noout -issuer 2>/dev/null)
	NOT_AFTER=$(printf '%s' "$CERT_PEM" | openssl x509 -noout -enddate 2>/dev/null | cut -d= -f2)

	if printf '%s' "$CERT_PEM" | openssl x509 -noout -checkend $((CERT_MIN_DAYS * 86400)) >/dev/null 2>&1; then
		ok "certificate valid for at least $CERT_MIN_DAYS more days (expires $NOT_AFTER)"
	else
		bad "certificate expires within $CERT_MIN_DAYS days ($NOT_AFTER) - renewal is not working"
	fi

	if printf '%s' "$CERT_PEM" | openssl x509 -noout -text 2>/dev/null | grep -q "DNS:$DOMAIN"; then
		ok "certificate covers $DOMAIN"
	else
		bad "certificate does not list $DOMAIN in its SANs ($SUBJECT)"
	fi

	CERT_KEY=$(printf '%s' "$CERT_PEM" | openssl x509 -pubkey -noout 2>/dev/null |
		openssl pkey -pubin -outform DER 2>/dev/null | openssl sha256 2>/dev/null)
	PRIVATE_KEY=$(printf '%s' "$KEY_PEM" | openssl pkey -pubout -outform DER 2>/dev/null |
		openssl sha256 2>/dev/null)
	if [ -n "$CERT_KEY" ] && [ "$CERT_KEY" = "$PRIVATE_KEY" ]; then
		ok "certificate matches Mosquitto's private key"
	else
		bad "certificate and private key do not match"
	fi

	KEY_MODE=$(mq stat -c '%a' /mosquitto/certs/privkey.pem 2>/dev/null)
	case "$KEY_MODE" in
	600 | 640) ok "private key permissions are $KEY_MODE" ;;
	*) bad "private key permissions are ${KEY_MODE:-unknown}, expected 600 or 640" ;;
	esac

	# Public trust is proved properly by the s_client check further down. What
	# matters here is catching the one wrong-but-plausible outcome: a rehearsal
	# left TLS_DIRECTIVE set to "tls internal", so the broker is serving
	# Caddy's own CA and no badge or browser will trust it.
	case "$ISSUER" in
	*"Caddy Local Authority"*)
		bad "serving Caddy's internal CA (${ISSUER#issuer=}) - unset TLS_DIRECTIVE for real ACME certificates"
		;;
	*)
		ok "issued by ${ISSUER#issuer=}"
		;;
	esac
fi

# --- Public endpoints -------------------------------------------------------
section 'public endpoints'
if [ "$LOCAL_ONLY" = yes ]; then
	skip "public endpoint checks (--local)"
else
	if curl -fsS --max-time 10 "https://$DOMAIN/healthz" >/dev/null 2>&1; then
		ok "https://$DOMAIN/healthz responds with a trusted certificate"
	else
		bad "https://$DOMAIN/healthz failed - DNS, firewall, or certificate problem"
	fi

	if curl -sS --max-time 10 -o /dev/null -w '%{http_code}' "http://$DOMAIN/" 2>/dev/null | grep -q '^30'; then
		ok "http://$DOMAIN/ redirects to HTTPS"
	else
		bad "http://$DOMAIN/ did not redirect (port 80 must stay open for ACME renewal)"
	fi

	# --http1.1 is required, not cosmetic. Caddy offers HTTP/2 over ALPN, and
	# the Upgrade mechanism does not exist in HTTP/2, so a default curl gets a
	# 502 from a perfectly healthy proxy. Browsers get this right on their own
	# (mqtt.js opens a WebSocket over HTTP/1.1).
	#
	# curl also exits non-zero here: after a successful 101 the connection
	# stays open until --max-time fires. The status it already wrote is what
	# matters, so the exit code is deliberately ignored.
	WS_STATUS=$(curl -s --http1.1 --max-time 6 -o /dev/null -w '%{http_code}' \
		-H 'Connection: Upgrade' -H 'Upgrade: websocket' \
		-H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
		-H 'Sec-WebSocket-Protocol: mqtt' \
		"https://$DOMAIN/mqtt" 2>/dev/null)
	[ -n "$WS_STATUS" ] || WS_STATUS=000
	if [ "$WS_STATUS" = "101" ]; then
		ok "wss://$DOMAIN/mqtt upgrades to a WebSocket"
	else
		bad "wss://$DOMAIN/mqtt returned HTTP $WS_STATUS, expected 101"
	fi

	if command -v openssl >/dev/null 2>&1; then
		if echo | openssl s_client -connect "$DOMAIN:8883" -servername "$DOMAIN" \
			-verify_hostname "$DOMAIN" -verify_return_error >/dev/null 2>&1; then
			ok "$DOMAIN:8883 certificate chain and hostname verify"
		else
			bad "$DOMAIN:8883 failed TLS verification from this host (firewall, security list, or chain)"
		fi
	else
		skip "openssl not installed; cannot verify 8883 from outside"
	fi
fi

printf '\n%s passed, %s failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
