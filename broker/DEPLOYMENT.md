# Oracle Always Free deployment

Production deployment of the DC34 badge broker onto an Oracle Cloud Always Free
instance: Mosquitto behind Caddy, with real certificates, on hardware that costs
nothing to keep running for the life of the badge.

## What this gives you

```
                    internet
                       |
        :80  :443      |      :8883
          |            |         |
      +---+------------+---------+---+
      |            Caddy             |   ACME, HTTPS, WSS
      |   https://DOMAIN/     -> web/ (static owner controller)
      |   wss://DOMAIN/mqtt   -> mosquitto:9001
      +------------+-----------------+
                   |  internal docker network
      +------------+-----------------+
      |          Mosquitto           |
      |   :9001  websockets (private)|
      |   :1883  plain (loopback)    |
      |   :8883  MQTT over TLS  <----+---- ESP32 badges, published
      +------------------------------+
                   ^
                   | certificate copy
      +------------+-----------------+
      |          certsync            |   no network, no docker socket
      +------------------------------+
```

Three ports are open to the internet and nothing else. Mosquitto is never
published on 1883 or 9001; browsers reach it only through Caddy, and the plain
listener is bound to the container's own loopback for healthchecks.

Caddy is the only ACME client. The certificate it obtains is reused for native
MQTT over TLS, so badges and browsers share one publicly trusted identity and
one renewal path.

## Before you start

- An Oracle Cloud account with the Always Free tier available.
- A domain name you control, where you can add an `A` record.
- Roughly 20 minutes, most of it waiting for the instance to provision.

## 1. Create the instance

In the Oracle console: **Compute → Instances → Create instance**.

| Setting | Value | Why |
| --- | --- | --- |
| Shape | `VM.Standard.A1.Flex`, 1 OCPU, 6 GB | Ampere A1 is arm64 and the most generous Always Free shape (up to 4 OCPU / 24 GB total across instances). Every image in this stack has a linux/arm64 build. |
| Image | Ubuntu 24.04 or Oracle Linux 9 | Both are covered below. |
| Boot volume | 50 GB is plenty | Always Free includes 200 GB total. |
| SSH keys | Upload your public key | Password login is disabled. |

If A1 capacity is unavailable ("Out of host capacity" is common in busy
regions), `VM.Standard.E2.1.Micro` is amd64 and also Always Free. The stack runs
on it unchanged, just with less headroom.

Then reserve the address: **Networking → IP management → Reserved public IPs**,
and attach it to the instance's VNIC. An ephemeral IP is released when the
instance stops, which would break DNS and every badge's saved broker address.

## 2. Point DNS at it

Add an `A` record for your hostname to the reserved IP, and confirm it before
deploying — ACME will not issue a certificate for a name that does not resolve
to this host.

```sh
dig +short badge.example.com     # must print the reserved IP
```

## 3. Open the ports — both firewalls

**This is the step that catches everyone.** An Oracle instance has two
independent firewalls, and traffic must pass both. Opening only the security
list leaves the port silently unreachable.

### 3a. VCN security list (the cloud side)

**Networking → Virtual cloud networks → your VCN → Security lists → Default**,
then add these **ingress** rules:

| Source | IP protocol | Destination port | Purpose |
| --- | --- | --- | --- |
| `0.0.0.0/0` | TCP | 80 | ACME HTTP-01 challenge, redirect to HTTPS |
| `0.0.0.0/0` | TCP | 443 | HTTPS for the controller, WSS for browsers |
| `0.0.0.0/0` | TCP | 8883 | MQTT over TLS for badges |

Leave the existing rule for port 22, and add nothing else. Do **not** open 1883
or 9001 — they are not published by the stack, and opening them would only
expose whatever else might bind there later.

### 3b. Instance firewall (the OS side)

Oracle's stock images ship with local rules that drop inbound traffic other than
SSH. Pick the one matching your image.

**Ubuntu** (rules live in iptables and must be inserted *before* the trailing
REJECT rule, which is why `-I INPUT 6` is used rather than `-A`):

```sh
sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 80 -j ACCEPT
sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 443 -j ACCEPT
sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 8883 -j ACCEPT
sudo netfilter-persistent save
sudo iptables -L INPUT --line-numbers -n | head -12   # confirm ACCEPT precedes REJECT
```

**Oracle Linux 9** (firewalld):

```sh
sudo firewall-cmd --permanent --add-port=80/tcp
sudo firewall-cmd --permanent --add-port=443/tcp
sudo firewall-cmd --permanent --add-port=8883/tcp
sudo firewall-cmd --reload
sudo firewall-cmd --list-ports
```

Verify from somewhere else entirely — not from the instance:

```sh
nc -vz badge.example.com 80
nc -vz badge.example.com 443
nc -vz badge.example.com 8883
```

## 4. Install Docker

**Ubuntu:**

```sh
sudo apt-get update
sudo apt-get install -y ca-certificates curl git
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
  https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo $VERSION_CODENAME) stable" |
  sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
sudo usermod -aG docker "$USER" && newgrp docker
```

**Oracle Linux 9:**

```sh
sudo dnf install -y dnf-utils git
sudo dnf config-manager --add-repo https://download.docker.com/linux/centos/docker-ce.repo
sudo dnf install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER" && newgrp docker
```

Confirm you have Compose v2 — the scripts use `docker compose`, not
`docker-compose`:

```sh
docker compose version
```

## 5. Deploy

```sh
git clone <this repo> dc34 && cd dc34/broker

cp .env.production.example .env.production
$EDITOR .env.production          # set DOMAIN and ACME_EMAIL

./scripts/setup-production.sh    # generates credentials, prints them once
./scripts/deploy.sh              # pulls, starts, waits for ACME, validates
```

`deploy.sh` brings Caddy up first, waits for the certificate to appear, then
starts the broker — Mosquitto cannot open a TLS listener before a certificate
exists. It finishes by running the healthcheck, which should end with
`N passed, 0 failed`.

Re-running `deploy.sh` is the normal way to apply a configuration change.

### Rehearsing first

To burn in the flow without risking Let's Encrypt's rate limit (5 duplicate
certificates per week), set `ACME_CA` to the staging directory in
`.env.production`, deploy, confirm everything works, then comment it out and
re-deploy. Staging certificates are not publicly trusted, so the healthcheck
will flag the chain — that is expected until you switch back.

## 6. Point the clients at it

**Badges** — enroll each physical badge by the eight-character ID shown on its
screen:

```sh
./scripts/enroll-badge.sh 68cd2517
```

The command prints a unique username/password and the exact serial provisioning
line. On the badge's MQTT screen the equivalent fields are:

| Field | Value |
| --- | --- |
| Broker | `badge.example.com` |
| Port | `8883` |
| User | `badge-68cd2517` (that badge's ID) |
| Password | the one-time password printed by `enroll-badge.sh` |

Port 8883 is not a preference — the firmware chooses TLS purely by port number
(`arduino/badge/mqtt.cpp`), so 8883 is what turns encryption on.
Use the DNS hostname, never the server IP: firmware verifies both the Let's
Encrypt chain and the certificate hostname. Rotate one compromised badge with
`./scripts/enroll-badge.sh --rotate 68cd2517`; other badges are unaffected.

**Browser controller** — either open `https://badge.example.com/`, which Caddy
serves from `web/`, or host `web/` anywhere (GitHub Pages) and set the broker
field to `wss://badge.example.com/mqtt`. Cross-origin works: WebSockets are not
subject to CORS preflight. The `web` credential is public by design and the ACL
confines it to owner topics.

## Operations

All commands run from `broker/`.

```sh
./scripts/healthcheck.sh          # validate everything, exit non-zero on failure
./scripts/healthcheck.sh --local  # skip checks needing the public endpoint

./scripts/backup.sh               # snapshot volumes + credentials to backups/
./scripts/restore.sh backups/dc34-broker-<stamp>.tar.gz

./scripts/update.sh               # backup, pull, recreate, validate, auto-roll-back
./scripts/update.sh --no-rollback # leave a failed update up for debugging
./scripts/rollback.sh             # return to the digests recorded by update.sh

docker compose -f compose.production.yml --env-file .env.production logs -f mosquitto
docker compose -f compose.production.yml --env-file .env.production ps
```

`update.sh` records the exact image digests running beforehand, so a rollback
returns to precisely what was there, not to whatever a tag points at today. It
rolls back automatically if the post-update healthcheck fails.

Rolling back pins the stack via a generated `compose.rollback.yml`. While it
exists, pass it to any compose command you run by hand; delete it and run
`deploy.sh` when you are ready to move forward again.

### Backups

`backup.sh` captures retained broker state, Caddy's ACME account and
certificates, and the credentials. **The archive contains secrets** — it is
written mode 600, and it belongs off the instance. Oracle Object Storage has an
Always Free allowance:

```sh
oci os object put -bn dc34-backups --file backups/dc34-broker-<stamp>.tar.gz
```

A nightly cron entry, keeping the last 14:

```sh
crontab -e
0 4 * * * cd /home/ubuntu/dc34/broker && ./scripts/backup.sh --quiet
```

Keeping `caddy_data` matters more than it looks: restoring it brings back the
ACME account key and current certificate, so rebuilding on a new instance does
not re-issue and does not count against rate limits.

### Monitoring

`healthcheck.sh` exits non-zero on any failure, which is enough for cron to mail
you:

```sh
0 * * * * cd /home/ubuntu/dc34/broker && ./scripts/healthcheck.sh >/dev/null
```

It checks certificate expiry with 14 days of margin, so a renewal that quietly
stopped working surfaces two weeks before badges start failing.

### Rotating credentials

```sh
./scripts/setup-production.sh --rotate
docker compose -f compose.production.yml --env-file .env.production up -d --force-recreate mosquitto
```

Every badge, the web client and any operator tooling need the new password
afterwards. Plan it for a time when you can reflash or re-enter them.

## Certificate renewal

Caddy renews automatically at 30 days remaining. `certsync` copies the new
certificate into the shared volume within 6 hours, and the Mosquitto container
restarts its broker process to bind it — a sub-second interruption about once a
month, on a connection the firmware already reconnects to with backoff.

The restart is necessary, not defensive: Mosquitto 2.0.20 rereads its
configuration on SIGHUP but keeps serving the certificate its listener started
with. This was measured against this image, by signalling the process directly;
only a process restart swapped the certificate. If that changes upstream, the
supervision loop in `scripts/mosquitto-entrypoint.sh` is the only thing to
simplify.

Browsers are unaffected — Caddy swaps certificates without dropping anything.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `deploy.sh` waits, then warns about no certificate | Port 80 unreachable, so the ACME challenge cannot complete | Check **both** firewalls (step 3), then `logs caddy` |
| Caddy logs `could not get certificate` | DNS not pointing at this host | `dig +short DOMAIN` against the reserved IP |
| Badge shows "broker down" | Wrong port | It must be 8883; 1883 is not published |
| Badge connects, then drops at CONNACK | Wrong password, or ACL denial | `logs mosquitto` names the user |
| Browser console: WebSocket 502 | Mosquitto down; Caddy is healthy but has nothing to proxy | `ps` and `logs mosquitto` |
| `healthcheck.sh` fails only on `wss://` | Reproducing by hand with curl needs `--http1.1` | Upgrade does not exist in HTTP/2; browsers handle this correctly |
| Rate limit from Let's Encrypt | Too many re-issues while iterating | Use `ACME_CA` staging until the flow works |
| `permission denied` talking to Docker | User not in the `docker` group yet | `newgrp docker`, or log out and back in |

## Security notes

- **Mosquitto is never published on 1883 or 9001.** The healthcheck asserts
  this, so an accidental port publish fails validation rather than going
  unnoticed.
- **Secrets stay out of git.** `broker/.gitignore` covers `.env.production`,
  `config/passwd.production`, backups and rollback state, and is local to
  `broker/` so it travels with the deployment.
- **The `web` credential is public by design** — it ships in the static site.
  The ACL confines it to claim and owner topics; it cannot reach fleet command
  or OTA. Badge authority comes from the displayed code and the derived session
  key, not from this password.
- **`certsync` holds no privileges.** No Docker socket, no host mounts,
  `network_mode: none`. It only copies between two volumes.
- **Badges verify the broker certificate.** Firmware pins ISRG Root X1 with
  `setCACert()` and validates the configured hostname. The healthcheck verifies
  the public chain, SAN, expiry, certificate/key match, and private-key mode.
- **Production badge credentials are unique.** Each broker account can read
  only broadcast and its own command topics and write only its own state,
  telemetry, and Wi-Fi observations. Individual credentials can be revoked or
  rotated without touching the fleet.

## Always Free limits worth knowing

- 4 Ampere A1 OCPUs and 24 GB RAM total, across all your A1 instances.
- 200 GB total block storage.
- 10 TB/month egress. Badge telemetry will not come close.
- Idle instances can be reclaimed on the free tier. A broker with badges
  connected is not idle, but if the fleet goes quiet for weeks, check that the
  instance is still up before assuming a deployment problem.
