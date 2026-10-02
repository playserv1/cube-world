#!/usr/bin/env bash
# cube-deploy: the one command the CI deploy key may run on this VM (its authorized_keys line forces it), and the
# operator's tool for the same jobs (`sudo -u deploy cube-deploy status`). Installed by Terraform, infra/vultr.
#
# An environment is served one of two ways, and each deploy says which:
#   image  its own container, compose service web-<env>, from an image the VM pulls (.github/workflows/web-image.yml)
#   files  a release uploaded with rsync into /srv/cube-world/releases/<env>/ (.github/workflows/web-files.yml)
set -euo pipefail
umask 022

APP=/opt/cube-world
SRV=/srv/cube-world
KEEP_RELEASES=5 # per environment, the one being served included

# shellcheck source=/dev/null
. "$APP/deploy.conf" # WEB_IMAGE: the only image repository this VM pulls from

usage() {
  cat >&2 <<'EOF'
usage:
  rsync --server ...           sent by rsync: rsync web/ deploy@<vm>:<env>/<release>/ uploads a release
  files  <env> <release>       serve an uploaded release
  image  <env> <image> [user]  pull <image> and serve it; with [user], the registry token is read from stdin
  status                       what each environment serves
EOF
  exit 2
}
die() { echo "cube-deploy: $*" >&2; exit 1; }
say() { echo "cube-deploy: $*" >&2; }
compose() { docker compose -f "$APP/compose.yaml" "$@"; }

host_of() { awk -v env="$1" '$1 == env { print $2 }' "$APP/sites.conf"; }

check_env() {
  [[ $1 =~ ^[a-z][a-z0-9]*$ && -n $(host_of "$1") ]] ||
    die "unknown environment '$1'; this VM serves: $(awk '{ printf "%s ", $1 }' "$APP/sites.conf")"
}

# compose's .env holds the image of each environment: WEB_<ENV>_IMAGE.
get_var() { sed -n "s/^$1=//p" "$APP/.env" 2>/dev/null || true; }
set_var() {
  # Rewritten in place: deploy owns .env but not the folder. Every writer holds the lock.
  local rest
  rest=$(grep -v "^$1=" "$APP/.env" || true)
  { [ -z "$rest" ] || echo "$rest"; [ -z "$2" ] || echo "$1=$2"; } >"$APP/.env"
}

reload_caddy() {
  local try
  # Right after the container starts (first boot) its admin endpoint may not listen yet.
  for try in 1 2 3; do
    compose exec -T caddy caddy reload --config /etc/caddy/Caddyfile --adapter caddyfile && return 0
    [ "$try" = 3 ] || sleep 2
  done
  return 1
}

# Points <env>'s site at serve-<mode> (see the Caddyfile) and reloads Caddy. If Caddy refuses it, the old site is put back.
serve() {
  local env=$1 mode=$2 file="$APP/caddy/sites/$1.caddy" backup
  backup=$(mktemp)
  cp "$file" "$backup" 2>/dev/null || true
  {
    echo "# Written by cube-deploy at $(date -u +%FT%TZ): $env is served from $mode."
    echo "$(host_of "$env") {"
    echo "	import common"
    # Search engines index the prod site only.
    [ "$env" = prod ] || echo '	header X-Robots-Tag "noindex, nofollow"'
    echo "	import serve-$mode $env"
    echo "}"
  } >"$file.new"
  mv "$file.new" "$file"
  if ! reload_caddy; then
    if [ -s "$backup" ]; then mv "$backup" "$file"; else rm -f "$file" "$backup"; fi
    die "Caddy refused the new site of $env; the previous one is back"
  fi
  rm -f "$backup"
}

cmd_files() {
  local env=$1 release=$2
  check_env "$env"
  [[ $release =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] || die "bad release name '$release'"
  [ -f "$SRV/releases/$env/$release/index.html" ] || die "release $env/$release has no index.html: upload it first"

  # Swap the link in one rename, so no request sees half of one release and half of another.
  ln -sfn "../releases/$env/$release" "$SRV/sites/.$env.new"
  mv -T "$SRV/sites/.$env.new" "$SRV/sites/$env"
  serve "$env" files
  # The environment's image container, if it had one, serves nothing any more.
  compose --profile "$env" rm -sf "web-$env" >/dev/null 2>&1 || true

  find "$SRV/releases/$env" -mindepth 1 -maxdepth 1 -type d -printf '%T@ %f\n' | sort -rn | cut -d' ' -f2- |
    { grep -vxF "$release" || true; } | tail -n +"$KEEP_RELEASES" | while IFS= read -r old; do
      rm -rf -- "${SRV:?}/releases/$env/$old"
    done
  say "$env serves release $release"
}

cmd_image() {
  local env=$1 image=$2 user=${3:-} var previous re
  check_env "$env"
  re="^${WEB_IMAGE//./\\.}(:[A-Za-z0-9_][A-Za-z0-9_.-]{0,127})?(@sha256:[0-9a-f]{64})?\$"
  [[ $image =~ $re && $image != "$WEB_IMAGE" ]] ||
    die "the image must be $WEB_IMAGE:<tag> or $WEB_IMAGE@sha256:<digest>, not '$image'"
  var="WEB_${env^^}_IMAGE"

  # The registry credential lives only as long as this command: CI sends its job token, which expires with the job.
  DOCKER_CONFIG=$(mktemp -d)
  export DOCKER_CONFIG
  trap 'rm -rf "$DOCKER_CONFIG"' EXIT
  if [ -n "$user" ]; then
    docker login "${WEB_IMAGE%%/*}" --username "$user" --password-stdin >/dev/null ||
      die "could not log in to ${WEB_IMAGE%%/*} as $user"
  fi

  previous=$(get_var "$var")
  set_var "$var" "$image"
  if ! compose --profile "$env" pull --quiet "web-$env" ||
    ! compose --profile "$env" up -d --no-deps --wait --wait-timeout 60 "web-$env"; then
    set_var "$var" "$previous"
    if [ -n "$previous" ]; then
      compose --profile "$env" up -d --no-deps "web-$env" || true
    else
      compose --profile "$env" rm -sf "web-$env" || true
    fi
    die "$image did not come up healthy; $env is back on what it served before"
  fi
  serve "$env" image
  docker image prune -af --filter "until=720h" >/dev/null || true
  say "$env serves $image"
}

cmd_status() {
  local env host mode
  while read -r env host; do
    [ -n "$env" ] || continue
    mode=$(sed -n 's/^.*import serve-\([a-z]*\) .*$/\1/p' "$APP/caddy/sites/$env.caddy" 2>/dev/null || true)
    case $mode in
      image) echo "$env https://$host image $(get_var "WEB_${env^^}_IMAGE") [$(compose --profile "$env" ps --format '{{.Status}}' "web-$env" 2>/dev/null)]" ;;
      files) echo "$env https://$host files $(readlink "$SRV/sites/$env")" ;;
      *) echo "$env https://$host not deployed" ;;
    esac
  done <"$APP/sites.conf"
}

# Over SSH the command line is what the client asked for; run by hand, it is this script's own arguments.
if [ -n "${SSH_ORIGINAL_COMMAND+set}" ]; then
  read -r -a argv <<<"$SSH_ORIGINAL_COMMAND"
else
  argv=("$@")
fi

case "${argv[0]:-}" in
  rsync)
    [ -n "${SSH_ORIGINAL_COMMAND:-}" ] || die "rsync is for uploads over SSH only"
    # rrsync confines the upload to the releases folder, and lets it write only.
    exec rrsync -wo "$SRV/releases"
    ;;
  files | image | status)
    # One deploy at a time: two environments deploying at once would both rewrite .env and reload Caddy.
    exec 9>"$APP/.lock"
    flock 9
    ;;&
  files) [ ${#argv[@]} -eq 3 ] || usage; cmd_files "${argv[1]}" "${argv[2]}" ;;
  image) [ ${#argv[@]} -eq 3 ] || [ ${#argv[@]} -eq 4 ] || usage; cmd_image "${argv[@]:1}" ;;
  status) cmd_status ;;
  *) usage ;;
esac
