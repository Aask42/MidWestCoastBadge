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

## Production

The Oracle Always Free deployment lives alongside this one and does not change
it: separate compose file, separate Mosquitto config, separate credentials.
Nothing above behaves differently because production exists.

| | Local | Production |
| --- | --- | --- |
| Compose | `compose.yml` | `compose.production.yml` |
| Mosquitto config | `config/mosquitto.conf` | `config/mosquitto.production.conf` |
| Credentials | `.env`, `config/passwd` | `.env.production`, `config/passwd.production` |
| ACL | `config/acl.conf` | the same file, shared |
| Exposed | 1883, 9001 on localhost | 80, 443, 8883 only |

See [DEPLOYMENT.md](DEPLOYMENT.md) for the full walkthrough: instance setup,
the two Oracle firewalls, ACME certificates shared between Caddy and Mosquitto,
and the backup, update and rollback commands.