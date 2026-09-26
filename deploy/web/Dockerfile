# The browser client (web/) as an image: Caddy serving the static files on $PORT (8080), behind the edge Caddy of
# the web VM (infra/vultr) or on Cloud Run (infra/gcp), each of which terminates TLS. Built from the repository root
# by .github/workflows/web-image.yml (GHCR, for the VM) and web-cloudrun.yml (Artifact Registry):
#   docker build -f deploy/web/Dockerfile -t cube-world-web .
#   docker run --rm -p 8080:8080 cube-world-web      # then http://localhost:8080
# What is left out of web/ is in Dockerfile.dockerignore (and, for the rsync deploy, in exclude.txt).
FROM caddy:2.11-alpine
COPY deploy/web/Caddyfile /etc/caddy/Caddyfile
COPY web/ /srv/
EXPOSE 8080
