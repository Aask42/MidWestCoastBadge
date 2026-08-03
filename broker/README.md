# Local MQTT Broker

This is the development version of the Oracle deployment. It exposes MQTT on
`1883` and browser WebSockets on `9001`. Production adds Caddy in front for
valid TLS/WSS certificates; the Mosquitto ACL and data layout stay the same.

```sh
./broker/setup-local.sh
docker compose -f broker/compose.yml --env-file broker/.env up -d
./broker/smoke-test.sh
```

Generated passwords live in `broker/.env`, which is ignored by Git. The web
password is transport-only and will be embedded in the static client. It has
no access to fleet command or OTA topics. The displayed badge code and owner
session encryption authorize per-badge controls.

Stop without deleting retained state:

```sh
docker compose -f broker/compose.yml --env-file broker/.env down
```