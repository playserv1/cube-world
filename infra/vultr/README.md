# Cube World web VM

The browser client (`web/`) is served from one small Vultr VM instead of GitHub Pages:

| Environment | Branch | Site |
|---|---|---|
| `prod` | `main` | `https://cube-world.playserv.com` |
| `dev` | `dev` | `https://dev.cube-world.playserv.com` |

Only the static client moves. The game servers and functions stay on PlayServ, and `deploy.yml` deploys them as before.

> **Paused.** For now, a push deploys the client to GitHub Pages (`pages.yml`) and Cloud Run (`web-cloudrun.yml`,
> see `infra/gcp`) only. Both VM workflows have their `push:` trigger commented out, so the VM gets no deploys on
> push, and Pages stays live. **Run workflow** on `web-image.yml` / `web-files.yml` still deploys the VM by hand.
>
> To resume the VM: uncomment the three `push:` lines in both workflows and merge that change. Then, when the VM
> sites work, retire Pages (step 7), which deletes `pages.yml`.

```
 push to main / dev
        │
        ▼
 GitHub Actions ── web-image.yml (WEB_DEPLOY_MODE unset or "image") ─ build image → ghcr.io ─┐
        │      └── web-files.yml (WEB_DEPLOY_MODE = "files") ──────── rsync web/ ───────────────┤
        │                                                                                      │ SSH as deploy
        ▼                                                                                      ▼ (forced command)
 DNS: A records (by hand)         ──► Vultr VM  ─  Docker  ─  edge Caddy :80/:443  (Let's Encrypt)
                                          │                 ├─ <prod host> → web-prod container  or  /srv/.../sites/prod
                                          │                 └─ <dev host>  → web-dev container   or  /srv/.../sites/dev
                                          └─ /usr/local/bin/cube-deploy: the only command the CI key can run
```

## What Terraform creates

| Resource | What it is |
|---|---|
| `vultr_instance.web` | Ubuntu 24.04, `vc2-1c-1gb` in `fra` by default, with IPv6, set up by cloud-init (`cloud-init.tf`) |
| `vultr_reserved_ip.web` | The VM's IPv4 (output `ipv4`). It outlives the VM, so the DNS records and CI keep pointing at it after a rebuild |
| `vultr_firewall_group.web` + rules | 80/tcp, 443/tcp, 443/udp (HTTP/3) and ICMP from anywhere; 22/tcp from `ssh_allowed_cidrs` |
| `tls_private_key.host` | The VM's SSH host key, so CI can pin it before the VM exists and keep it across rebuilds |
| `tls_private_key.deploy` | The SSH key GitHub Actions deploys with |

On the VM:

| Path | What |
|---|---|
| `/opt/cube-world/compose.yaml` | `caddy` (the edge) and `web-<env>` (an environment's image, when it is served as one) |
| `/opt/cube-world/caddy/Caddyfile` | The edge config. `caddy/sites/<env>.caddy` is written by `cube-deploy` on every deploy |
| `/opt/cube-world/.env` | Each environment's image (`WEB_<ENV>_IMAGE`), written by `cube-deploy` |
| `/srv/cube-world/releases/<env>/` | Releases uploaded by rsync (the newest 5 are kept). `sites/<env>` links to the one being served |
| `/usr/local/bin/cube-deploy` | The deploy command (`files/cube-deploy.sh`) |

There are two users: `ops` for admins (your keys, with sudo) and `deploy` for CI (the docker group, forced to run
`cube-deploy`). Root and passwords cannot log in over SSH.

## Migration checklist

Do the steps in this order. Once the VM workflows are resumed (see *Paused* above), merging that change into `dev`
starts the first deploy, so the VM and the GitHub settings must exist before you merge it.

1. [Prepare](#1-prepare): a Vultr API key, your SSH key, `terraform.tfvars`.
2. [Reserve the IP and create the DNS records](#2-reserve-the-ip-and-create-the-dns-records), by hand.
3. [Provision the VM](#3-provision-the-vm) with `terraform apply`, then check it serves the placeholder page over HTTPS.
4. [Configure GitHub](#4-configure-github): environments, variables, the secret, GHCR.
5. [Check the platform accepts the new origins](#5-check-the-platform-accepts-the-new-origins).
6. [First deploy](#6-first-deploy): merge into `dev` and check the dev site; then get `main` deployed.
7. [Retire GitHub Pages](#7-retire-github-pages).

---

## 1. Prepare

### Tools on your machine

- Terraform 1.6 or newer (`terraform version`)
- `ssh`, `curl`, `dig`
- The GitHub CLI `gh`, logged in with admin rights on `playserv1/cube-world`. It's optional: every `gh` command
  below has a web UI equivalent.

### Vultr API key

1. Go to <https://my.vultr.com> → **Account** → **API** → **Enable API**.
2. Under **Access Control**, add your public IPv4 as `/32` (`curl -4 ifconfig.me`), and your IPv6 as `/128` if you
   have one. Vultr refuses the key from any other address. Add the address again when it changes.
3. Copy the key. Keep it in your password manager, never in this folder.

### Your admin SSH key

If you have no ed25519 key yet, run `ssh-keygen -t ed25519`. Terraform needs the public half, `~/.ssh/id_ed25519.pub`.
Add your teammates' public keys too if they will administer the VM.

### `terraform.tfvars`

```bash
cd infra/vultr
cp terraform.tfvars.example terraform.tfvars   # gitignored
```

Fill in `acme_email` and `admin_ssh_public_keys`. `sites` already holds the two hostnames,
`cube-world.playserv.com` (prod) and `dev.cube-world.playserv.com` (dev); change them there if needed. The other
variables have defaults; they are described in `variables.tf`.

## 2. Reserve the IP and create the DNS records

Terraform doesn't manage DNS. Reserve the VM's IPv4 first, on its own, so the records can exist before the VM
boots. Caddy then gets its certificates on the first try.

```bash
cd infra/vultr
export VULTR_API_KEY=...            # from step 1

terraform init
terraform apply -target=vultr_reserved_ip.web     # 1 to add; Terraform warns that -target is for exceptions, which is fine here
terraform output -raw ipv4
```

In Cloudflare (<https://dash.cloudflare.com> → `playserv.com` → **DNS** → **Records** → **Add record**), create one
record per site:

| Type | Name | IPv4 address | Proxy status | TTL |
|---|---|---|---|---|
| A | `cube-world` | the `ipv4` output | **DNS only** (grey cloud) | Auto |
| A | `dev.cube-world` | the `ipv4` output | **DNS only** (grey cloud) | Auto |

- **DNS only matters.** With the orange cloud, Cloudflare terminates TLS in front of Caddy. That needs SSL/TLS mode
  *Full (strict)*, hides visitors' IPs from the VM's logs, and isn't what this setup was tested with.
- **IPv6 is optional.** The VM has an IPv6 too (`terraform output ipv6` after step 3), and you can add AAAA records
  for it. But unlike the reserved IPv4, it changes every time the VM is rebuilt, and then the AAAA records must be
  updated by hand. A stale AAAA record breaks the site for IPv6 visitors, so skip AAAA unless you need it.
- Delete any old record for these names first, for example a CNAME to `playserv1.github.io`.
- The IPv4 never changes while the reserved IP exists, so you won't touch these records again, not even after a VM
  rebuild. Only a `terraform destroy` releases it.

Check:

```bash
dig +short cube-world.playserv.com          # the ipv4 output
dig +short dev.cube-world.playserv.com
```

## 3. Provision the VM

```bash
terraform plan                      # expect 15 to add
terraform apply
```

The apply takes a minute or two. cloud-init then needs about **3–5 more minutes** on the VM to update packages,
install Docker and start Caddy. Caddy gets the certificates as soon as it starts, because the records from step 2
already point at the VM. If you created them later, Caddy keeps retrying and picks them up by itself.

Trust the VM's host key and log in:

```bash
terraform output -raw ssh_known_hosts >> ~/.ssh/known_hosts
ssh ops@$(terraform output -raw ipv4)

cloud-init status --wait --long          # should end in "status: done"
sudo -u deploy cube-deploy status        # both environments: "files ../releases/<env>/placeholder"
docker compose -f /opt/cube-world/compose.yaml logs caddy | grep -i certificate
```

From your machine:

```bash
curl -sI https://cube-world.playserv.com | head -1          # HTTP/2 200
curl -s  https://dev.cube-world.playserv.com | grep 'first deploy'
```

Both sites should show the placeholder page *"The server is up. The game appears here after its first deploy."*
over a valid certificate. If they don't, see [Troubleshooting](#troubleshooting).

**Back up `terraform.tfstate`.** It holds the VM's SSH host key and the CI deploy key, and it is the only record of
what Terraform manages. Keep an encrypted copy, for example as an attachment in your password manager, and never
commit it (`.gitignore` keeps it out).

## 4. Configure GitHub

All of this is under **Settings** of `playserv1/cube-world`. The `gh` commands do the same thing; run them from
`infra/vultr` after the apply.

### Environments

Create two environments under **Settings → Environments → New environment**:

| Environment | Deployment branches and tags | Variable `WEB_HOST` | Variable `WEB_CONFIG_JS` (optional) |
|---|---|---|---|
| `web-prod` | Selected branches: `main` | the prod hostname | a full `config.js` for prod (see below) |
| `web-dev` | Selected branches: `dev` | the dev hostname | — |

`WEB_CONFIG_JS`, if set, replaces `web/config.js` in that environment's deploy. Use it when prod should talk to
another platform, client key or slug than the `config.js` committed on the branch. Its value is the whole file:

```js
window.CUBEWORLD = {
  api: "https://dev.platform.playserv.io",
  clientKey: "pk_...",
  slug: "cubeworld",
};
```

```bash
for env in prod dev; do
  echo '{"deployment_branch_policy":{"protected_branches":false,"custom_branch_policies":true}}' |
    gh api -X PUT repos/playserv1/cube-world/environments/web-$env --input -
done
gh api -X POST repos/playserv1/cube-world/environments/web-prod/deployment-branch-policies -f name=main -f type=branch
gh api -X POST repos/playserv1/cube-world/environments/web-dev/deployment-branch-policies  -f name=dev  -f type=branch
gh variable set WEB_HOST --env web-prod --body "$(terraform output -json sites | jq -r .prod | sed 's|https://||')"
gh variable set WEB_HOST --env web-dev  --body "$(terraform output -json sites | jq -r .dev  | sed 's|https://||')"
```

You can also add **Required reviewers** to `web-prod` if a prod deploy should wait for approval.

### Repository variables and secret

Set these under **Settings → Secrets and variables → Actions**:

| Name | Kind | Value |
|---|---|---|
| `WEB_VM_HOST` | variable | `terraform output -raw ipv4` |
| `WEB_DEPLOY_KNOWN_HOSTS` | variable | `terraform output -raw ssh_known_hosts` (a public key, not a secret) |
| `WEB_DEPLOY_MODE` | variable | `image` (or `files`, see [Switching deploy modes](#switching-deploy-modes)) |
| `WEB_DEPLOY_SSH_KEY` | **secret** | `terraform output -raw deploy_private_key` |

```bash
gh variable set WEB_VM_HOST            --body "$(terraform output -raw ipv4)"
gh variable set WEB_DEPLOY_KNOWN_HOSTS --body "$(terraform output -raw ssh_known_hosts)"
gh variable set WEB_DEPLOY_MODE        --body image
terraform output -raw deploy_private_key | gh secret set WEB_DEPLOY_SSH_KEY
```

### GHCR (image mode only)

The image workflow pushes `ghcr.io/playserv1/cube-world-web` with the job's own `GITHUB_TOKEN`. No personal token is
needed, and the VM keeps no registry credential: each deploy hands the VM that job's token, which expires with the job.

- **Organization → Settings → Packages → Package creation**: members must be allowed to create **private** packages.
  If they aren't, the first push fails with `denied: installation not allowed to Create organization package`.
- After the first image push, open the package (**Organization → Packages → cube-world-web → Package settings**) and
  check that **Manage Actions access** lists `cube-world` with the **Write** role. A package created by the repo's own
  workflow gets this automatically. The package can stay private.

## 5. Check the platform accepts the new origins

The page calls the PlayServ API (`api` in `config.js`) from the browser, so its origin changes from
`https://playserv1.github.io` to the new hostnames. If the platform or the project restricts which browser origins
may use the client key (CORS), add `https://cube-world.playserv.com` and `https://dev.cube-world.playserv.com` there before the first
deploy. Nothing in this repository sets that list.

To check it: open the new site, open DevTools → Console, type a name and click **Play**. A CORS error on
`/auth/players/anon` or `/rooms/...:browse` means the origin is not allowed yet. The game servers' `wss://`
connections are not affected by the page's origin.

## 6. First deploy

### dev

Resume the VM workflows (see *Paused* above) on a branch, open a PR into `dev` and merge it. The merge changes the
workflow files, so it starts **Web client → VM (image)** on `dev`. The run should:

1. Build and push `ghcr.io/playserv1/cube-world-web:<sha12>` and `:dev`.
2. SSH to the VM: `cube-deploy: dev serves ghcr.io/...@sha256:...`.
3. End with a notice like `https://dev.cube-world.playserv.com serves <sha12> from its image`.

Then open the dev site and play a round: sign in, see the server list, enter a server, place a block.

The **Run workflow** button in the Actions tab only appears once a workflow file is on the default branch, `main`.
Until then, a push to `dev` that touches `web/**` or `deploy/web/**` is how to deploy `dev` again.

### prod

`main` does not have a `web/config.js` today: it is gitignored there and only committed on `dev`. A prod deploy
from `main` as it is now stops at **Check config.js** with a clear error. Do one of these:

- Merge `dev` into `main` as usual. That brings the workflows, `deploy/web` and `config.js` together, and the push to
  `main` deploys prod.
- Or set `WEB_CONFIG_JS` on the `web-prod` environment first.

Afterwards, check the prod site the same way.

## 7. Retire GitHub Pages

Pages keeps publishing on every push while the VM is paused. Retire it only after the VM sites work:

1. Check both new sites work (step 6), then delete `.github/workflows/pages.yml` and merge that change. From then on
   nothing publishes to Pages, but the last Pages deployment stays online until you take it down.
2. Optional: to keep old links working, replace the Pages site with a redirect for a while. Push a one-file branch
   with `<meta http-equiv="refresh" content="0; url=https://cube-world.playserv.com/">` as `index.html`, and set
   **Settings → Pages → Source** to *Deploy from a branch* on that branch.
3. **Settings → Pages** → **Unpublish site** (or set **Source** to *None*).
4. **Settings → Environments** → delete `github-pages`.
5. Update the links you shared: the demo script, Jira, Slack bookmarks, the Unreal launcher notes, and so on.

---

## Switching deploy modes

Both workflows exist. `WEB_DEPLOY_MODE` decides which one runs: `files` runs `web-files.yml`, anything else
(including unset) runs `web-image.yml`. Each deploy also rewrites that environment's site on the VM, so it is served
from what was just deployed.

| | `image` (web-image.yml) | `files` (web-files.yml) |
|---|---|---|
| What is shipped | A Caddy image with `web/` inside, pushed to GHCR | `web/` itself, rsynced into a new release folder |
| On the VM | Container `web-<env>`, which the edge proxies to | The edge serves `/srv/cube-world/sites/<env>` itself |
| Versions kept | Every image in GHCR (`:<sha12>`) | The newest 5 releases on the VM |
| Fast rollback | `cube-deploy image <env> …:<old sha12>` | `cube-deploy files <env> <old sha12>` |
| Needs | GHCR package creation in the org | Nothing else |

To switch:

1. Set the variable: `gh variable set WEB_DEPLOY_MODE --body files` (or `image`).
2. Deploy each environment once with the workflow of the new mode: push to `dev` and `main`, or use **Run workflow**
   on each branch. The mode is read when a run starts, so a run of the other workflow, re-run or new, is skipped.
3. `ssh ops@<ip> sudo -u deploy cube-deploy status` shows `image` or `files` for each environment.

Until an environment is deployed again, it keeps being served the old way. A switch is never half done.

## Operations

All of these run on the VM after `ssh ops@<ip>`.

| Task | Command |
|---|---|
| What each site serves | `sudo -u deploy cube-deploy status` |
| Edge logs (requests, certificates) | `docker compose -f /opt/cube-world/compose.yaml logs -f caddy` |
| An environment's container | `docker compose -f /opt/cube-world/compose.yaml --profile prod logs web-prod` |
| Releases on disk | `ls -lt /srv/cube-world/releases/prod` |
| The cloud-init log | `less /var/log/cloud-init-output.log` |

### Rollback

- **Either mode, from GitHub:** open the last good run *of the current mode's workflow* → **Re-run all jobs**. It
  checks out that run's commit and deploys it again. A run of the other workflow is skipped.
- **files, on the VM, instantly:** `sudo -u deploy cube-deploy files prod <release>`, where `<release>` is one of the
  folders in `/srv/cube-world/releases/prod`.
- **image, on the VM:** the package is private, so the VM needs a token to pull. Use a personal access token
  (classic) with `read:packages`:
  `echo "$PAT" | sudo -u deploy cube-deploy image prod ghcr.io/playserv1/cube-world-web:<sha12> <your GitHub login>`.

The next push to the branch deploys over a rollback, so revert the bad commit too.

### Updates

- **OS:** `unattended-upgrades` installs security updates every day. Reboot now and then (`sudo reboot`). The
  containers come back by themselves (`restart: unless-stopped`).
- **Caddy patch releases** (within `caddy:2.11-alpine`):
  `cd /opt/cube-world && sudo docker compose pull caddy && sudo docker compose up -d caddy`. The edge restarts in about
  a second, and the certificates stay in the `caddy_data` volume. The web image follows `deploy/web/Dockerfile` on its
  next build.
- **Caddy minor version:** change `caddy_image` in Terraform and the `FROM` line in `deploy/web/Dockerfile` (this
  rebuilds the VM, see below).

### Changing the VM

cloud-init runs only on the first boot. So any change to what it sets up **replaces the VM** on the next
`terraform apply`, and the plan says so (`terraform_data.cloud_init` must be replaced). That covers the Caddyfile,
`cube-deploy`, compose, the users and keys, and `sites`. The reserved IPv4 and the host key stay the
same, so neither the DNS records nor CI need a change (except AAAA records, if you added them: update them to
the new `terraform output ipv6`). What a rebuild costs:

- About 5 minutes of downtime.
- Both sites come back on the placeholder page. Deploy them again by re-running the latest run on `main` and on
  `dev`, or by **Run workflow**.
- New Let's Encrypt certificates. The limit is 5 for the same set of names per week, so don't rebuild more often
  than that.

For a quick fix, you can also edit the file on the VM (`sudo` as `ops`) and then
`docker compose exec caddy caddy reload --config /etc/caddy/Caddyfile`. Make the same change here in Terraform, or the
next rebuild loses it.

Common changes:

| Change | How |
|---|---|
| Add or remove an admin | Edit `admin_ssh_public_keys` → `terraform apply` (rebuild). For an immediate fix without a rebuild: edit `~ops/.ssh/authorized_keys` on the VM *and* tfvars |
| Rotate the CI deploy key | `terraform apply -replace=tls_private_key.deploy` (rebuild), then `terraform output -raw deploy_private_key \| gh secret set WEB_DEPLOY_SSH_KEY` |
| Restrict SSH to fixed networks | `ssh_allowed_cidrs` (the firewall changes in place, no rebuild). GitHub-hosted runners then can no longer deploy unless you add [their ranges](https://api.github.com/meta) or use a self-hosted runner |
| Add an environment (e.g. `qa`) | Add it to `sites` → apply (rebuild), create its A record, add a `web-qa` GitHub environment, and map its branch in both workflows (`SITE`, `environment.name`, `if:`) |
| Bigger VM | `plan`. Vultr resizes in place without a rebuild (upsizing only) |

### Cost

About $5 a month for `vc2-1c-1gb`, plus about $3 a month for the reserved IPv4. Check
<https://www.vultr.com/pricing/>. GHCR storage for a ~50 MB image per commit is free for private packages
within the org's included storage. Delete old versions now and then under the package's settings.

### Tear down

```bash
terraform destroy
```

This removes the VM, the reserved IP and the firewall. Then delete the two A records in Cloudflare, and delete `WEB_*` from the repository's
variables and secrets, the `web-prod`/`web-dev` environments, and the `cube-world-web` package. Disable the two
workflows, or delete them.

## Security notes

- **The CI key can only deploy.** Its `authorized_keys` line is `restrict,command="/usr/local/bin/cube-deploy"`: no
  shell, no port forwarding, no PTY. `cube-deploy` accepts `files`, `image` and `status` with checked arguments,
  pulls only from `web_image`, and lets rsync write only below `/srv/cube-world/releases` (`rrsync -wo`). `deploy` is
  in the docker group, which is root-equivalent, so the forced command is the boundary. Don't give that key anything
  else.
- **SSH is open to the world by default**, because GitHub-hosted runners have no fixed addresses. Logins take keys
  only, root can't log in, and only `ops` and `deploy` are allowed.
- **Secrets on disk:** `terraform.tfstate` holds the host and deploy private keys. Keep it out of git and backed up
  encrypted. The Vultr API key lives only in your shell's environment.
- **Nothing secret is served.** `config.js` holds the client key (`pk_*`), which is public by design. Server keys
  (`sk_*`) never go into `web/`.
- The dev site sends `X-Robots-Tag: noindex`, so search engines index prod only.

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `terraform apply`: Vultr `401` / `Unauthorized IP address` | Your IP is not in the API key's **Access Control** list (step 1) |
| `ssh ops@…`: `Permission denied (publickey)` | cloud-init is still running (wait 5 minutes), or your key is not in `admin_ssh_public_keys`. The Vultr web console works as a fallback: log in as root with the password from the Vultr panel |
| `ssh`: `REMOTE HOST IDENTIFICATION HAS CHANGED` | The host key was replaced (`-replace=tls_private_key.host`). Update `~/.ssh/known_hosts` and `WEB_DEPLOY_KNOWN_HOSTS` |
| Browser: certificate error, or Caddy logs `challenge failed` | DNS doesn't point at the VM yet (`dig +short <host>`), the record is proxied (orange cloud: switch it to DNS only), or port 80 is blocked. Caddy retries by itself |
| Site shows the placeholder | Nothing has been deployed since the VM was built. Run the workflow for that branch |
| `502` from the site | Image mode, and the `web-<env>` container is down: `cube-deploy status`, then `docker compose … --profile <env> logs web-<env>`; deploy again |
| Workflow: `Host key verification failed` | `WEB_DEPLOY_KNOWN_HOSTS` doesn't match `terraform output -raw ssh_known_hosts`, or `WEB_VM_HOST` isn't the IP in that line |
| Workflow: `Permission denied (publickey)` | `WEB_DEPLOY_SSH_KEY` doesn't match the current deploy key (after a rotation), or SSH is restricted by `ssh_allowed_cidrs` |
| Workflow: `denied: installation not allowed to Create organization package` | Org package creation is off (see [GHCR](#ghcr-image-mode-only)) |
| Workflow: `could not log in to ghcr.io` or the pull is `denied` on the VM | The package doesn't grant the repo access: **Manage Actions access** → add `cube-world` |
| Workflow: `web/config.js is missing` | See [prod](#prod) |
| Neither workflow runs | The branch isn't `main` or `dev`, or the push touched no path in the workflow's `paths` |
| Game: CORS errors in the console | See [step 5](#5-check-the-platform-accepts-the-new-origins) |
