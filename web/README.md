# Badge owner control

Static GitHub Pages-compatible controller for a single DC34 badge. Connects over MQTT WebSocket to send authenticated commands. The owner HMAC key is derived locally (PBKDF2, 100 000 iterations) from the five-character code shown on the badge's MQTT screen — the code and key never leave the browser.

## Features

- **Set name** — up to 23 characters, displayed in nametag mode
- **Switch show** — nametag, slideshow (all), wifi scanner, cube, pyramid, tunnel, rings, pop-out text
- **Send popup** — temporary full-screen message (1–600 seconds)
- **Live state** — subscribes to the badge's state topic and shows current name, mode, and firmware version

## Security model

Commands use the `o1` sealed envelope format:

1. Key derivation: `PBKDF2(code, salt="dc34-owner-v1:<badgeId>", iterations=100000, hash=SHA-256)` → HMAC-SHA256 key
2. Replay protection: monotonic sequence number (Unix timestamp, stored in localStorage)
3. Envelope: `o1\n<seq>\n<base64 HMAC>\n<JSON body>`

The web transport credential (username `web`) is public by design; broker ACLs restrict it to owner topics (`dc34/badge/<id>/owner`). The physical badge code authenticates each command.

## Local development

Start the broker and serve this directory:

```sh
docker compose -f broker/compose.yml --env-file broker/.env up -d
python3 -m http.server 8080 --directory web
```

Open `http://localhost:8080`, use `ws://localhost:9001`, username `web`, and the `WEB_PASSWORD` value from `broker/.env`.

## Production

Serve this directory over HTTPS (e.g. GitHub Pages). Point the broker URL at the deployment's `wss://` endpoint. No build step — the app is plain ES modules plus the mqtt.js CDN bundle.

## Files

| File | Purpose |
|------|---------|
| `index.html` | UI shell with connect form, controls, and badge instructions |
| `app.js` | MQTT connection, state subscription, command dispatch |
| `crypto.js` | PBKDF2 key derivation and HMAC envelope sealing |
| `styles.css` | Responsive dark theme (Space Grotesk + DM Mono) |
