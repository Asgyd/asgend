# Production image for the ASGEND Control Plane.
# The container is designed to run with read_only: true and an external data volume.
FROM python:3.12-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONPATH=/opt/asgend

RUN groupadd --system --gid 10001 asgend \
    && useradd --system --uid 10001 --gid asgend --home-dir /nonexistent \
       --shell /usr/sbin/nologin asgend \
    && mkdir -p /data/artifacts /backups \
    && chown -R 10001:10001 /data /backups

WORKDIR /opt/asgend
COPY --chown=10001:10001 server/asgend_cp/ /opt/asgend/asgend_cp/

# psycopg is imported lazily; it is needed only for the PostgreSQL backend.
RUN python -m pip install --no-cache-dir --disable-pip-version-check --root-user-action=ignore 'psycopg[binary]>=3.2,<4' \
    && python -m compileall -q /opt/asgend/asgend_cp

USER 10001:10001
EXPOSE 8787
ENTRYPOINT ["python", "-m", "asgend_cp"]
CMD ["serve"]
