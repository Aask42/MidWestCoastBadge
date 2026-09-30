#!/bin/sh
# Push the broker directory to the remote server and optionally run deploy.sh.
#
#   ./push-broker.sh              # rsync + deploy
#   ./push-broker.sh --no-deploy  # rsync only
#   ./push-broker.sh --dry-run    # show what rsync would transfer
#
# Requires: rsync, ssh key at ~/.ssh/sshkey
set -eu

REMOTE_HOST="aask@192.168.1.100"
REMOTE_DIR="~/dc34-broker"
SSH_KEY="$HOME/.ssh/sshkey"
SSH_OPTS="-i $SSH_KEY"

DEPLOY=true
DRY_RUN=""

for arg in "$@"; do
  case "$arg" in
    --no-deploy) DEPLOY=false ;;
    --dry-run)   DRY_RUN="--dry-run" ;;
    -h|--help)
      echo "Usage: $0 [--no-deploy] [--dry-run]"
      exit 0 ;;
    *) echo "unknown flag: $arg" >&2; exit 1 ;;
  esac
done

BROKER_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"

echo "==> syncing $BROKER_DIR -> $REMOTE_HOST:$REMOTE_DIR"

rsync -avz --delete $DRY_RUN \
  -e "ssh $SSH_OPTS" \
  --exclude '.env' \
  --exclude '.env.production' \
  --exclude '.env.*' \
  --include '.env.production.example' \
  --exclude 'data/' \
  --exclude 'log/' \
  --exclude 'certs/' \
  --exclude 'backups/' \
  --exclude '.rollback-state' \
  --exclude 'compose.rollback.yml' \
  --exclude 'config/passwd' \
  --exclude 'config/passwd.production' \
  --exclude 'config/acl.production.conf' \
  --exclude '.DS_Store' \
  "$BROKER_DIR/" "$REMOTE_HOST:$REMOTE_DIR/"

if [ -n "$DRY_RUN" ]; then
  echo "==> dry run complete, nothing was transferred"
  exit 0
fi

if $DEPLOY; then
  echo "==> running deploy.sh on $REMOTE_HOST"
  ssh $SSH_OPTS "$REMOTE_HOST" "cd $REMOTE_DIR && sh scripts/deploy.sh"
else
  echo "==> sync complete (deploy skipped, run 'ssh $SSH_OPTS $REMOTE_HOST cd $REMOTE_DIR \\&\\& sh scripts/deploy.sh' to deploy)"
fi
