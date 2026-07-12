#!/bin/bash
# One-container startup: bring up Postgres, wait for it, run alembic
# migrations, then hand off to uvicorn.
#
# Every line from either process is prefixed [pg] / [web] so
# `kubectl logs` reads cleanly even though we've merged two workloads
# into one PID space.
set -e

log() { printf '[%s] %s\n' "$1" "$2"; }

# --- postgres ---
# The image installs postgresql-16 to /var/lib/postgresql/16/main.
# On a fresh PVC the data dir will be empty; initdb via pg_ctlcluster
# is done at image build time (see Dockerfile), so mounting an empty
# PVC would wipe it. To handle both cases:
#   - if PGDATA is empty, run pg_createcluster to init
#   - otherwise just start
PGDATA=/var/lib/postgresql/16/main
if [ ! -s "$PGDATA/PG_VERSION" ]; then
    # Empty PVC (or first-ever boot before image cluster is populated).
    log pg "empty $PGDATA — re-initializing cluster"
    # Clean the data dir + the cluster registration + the conf dir.
    # The image-build initdb populated all three; a PVC mount only shadows
    # the data dir, so we have to actively clear the other two before
    # pg_createcluster will run without complaining.
    rm -rf "$PGDATA"/* "$PGDATA"/.[!.]* 2>/dev/null || true
    rm -rf /etc/postgresql/16/main /var/lib/postgresql/16/*.pid 2>/dev/null || true
    pg_createcluster --datadir="$PGDATA" 16 main
    # Re-apply our listen_addresses tweak (undone by cluster recreate).
    sed -i "s/^#\?listen_addresses.*/listen_addresses = '127.0.0.1'/" \
        /etc/postgresql/16/main/postgresql.conf
fi

log pg "starting cluster 16/main"
pg_ctlcluster 16 main start
# Wait for readiness before role/db bootstrap.
for i in $(seq 1 30); do
    if su postgres -c 'pg_isready -q'; then break; fi
    sleep 1
done

# Idempotent role + db bootstrap. Must run every startup because the
# image-time cluster init doesn't create these — that's owned by us.
log pg "ensuring role 'simsc' + database 'simsc'"
su postgres -c "psql -tAc \"SELECT 1 FROM pg_roles WHERE rolname='simsc'\"" \
    | grep -q 1 \
    || su postgres -c "psql -c \"CREATE ROLE simsc LOGIN PASSWORD 'simsc';\""
su postgres -c "psql -tAc \"SELECT 1 FROM pg_database WHERE datname='simsc'\"" \
    | grep -q 1 \
    || su postgres -c "psql -c \"CREATE DATABASE simsc OWNER simsc;\""
log pg "ready (role+db ensured)"

# --- migrations ---
cd /opt/simsc
log web "running alembic upgrade head"
export PYTHONUNBUFFERED=1
alembic upgrade head 2>&1 | sed 's/^/[web] /'

# --- uvicorn ---
log web "starting uvicorn on 0.0.0.0:8080"
exec uvicorn app.main:app --host 0.0.0.0 --port 8080 --no-server-header 2>&1 \
    | sed -u 's/^/[web] /'
