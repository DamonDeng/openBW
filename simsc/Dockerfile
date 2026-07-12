# simsc-app: one container running everything (per M2 decisions).
#
# debian:12-slim + postgresql-16 + Python 3.12 (from deadsnakes-like
# python3-full apt package) + FastAPI stack + our source.
#
# Build:
#   docker build -f simsc/Dockerfile -t simsc-app:local simsc/
#
# Run standalone (host maps host port 8080 -> container 8080):
#   docker run --rm -p 8080:8080 \
#     -e COGNITO_POOL_ID=... -e COGNITO_CLIENT_ID=... -e COGNITO_DOMAIN=... \
#     -e SITE_ORIGIN=https://simsc.agentnumber47.com \
#     -e ADMIN_TOKEN=$(openssl rand -hex 32) \
#     simsc-app:local

FROM debian:12-slim

# System deps + postgres + python. Bookworm's default repo has pg15;
# we want pg16, so pull from the PostgreSQL Global Development Group
# apt repo (pgdg).
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg \
    && install -d /usr/share/postgresql-common/pgdg \
    && curl -fsSL https://www.postgresql.org/media/keys/ACCC4CF8.asc \
        -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
    && echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] https://apt.postgresql.org/pub/repos/apt bookworm-pgdg main" \
        > /etc/apt/sources.list.d/pgdg.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        postgresql-16 postgresql-client-16 \
        python3 python3-pip python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Python venv so pip installs don't collide with Debian's system python
RUN python3 -m venv /opt/venv
ENV PATH=/opt/venv/bin:$PATH

WORKDIR /opt/simsc

# Install python deps first (better layer caching)
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

# App source + start script
COPY app/           ./app/
COPY alembic/       ./alembic/
COPY alembic.ini    ./
COPY start.sh       /opt/simsc/start.sh
RUN chmod +x /opt/simsc/start.sh

# Postgres listens on 127.0.0.1 only (in-pod)
RUN sed -i "s/^#\?listen_addresses.*/listen_addresses = '127.0.0.1'/" \
        /etc/postgresql/16/main/postgresql.conf

# Expose only the web port; pg is pod-internal
EXPOSE 8080

# Run as root so postgres+web can share the container. This container
# is a pod-in-a-container by design; k8s pod-level user isolation
# comes from Fargate not shared kernels.
ENTRYPOINT ["/opt/simsc/start.sh"]
