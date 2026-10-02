# Cube World on Cloud Run

The browser client (`web/`) also runs on Cloud Run, next to the Vultr VM (`infra/vultr`), not instead of it:

| Where | Branch | Site | Workflow |
|---|---|---|---|
| Vultr VM, prod | `main` | `https://cube-world.playserv.com` | `web-image.yml` / `web-files.yml` |
| Vultr VM, dev | `dev` | `https://dev.cube-world.playserv.com` | `web-image.yml` / `web-files.yml` |
| **Cloud Run, prod** | `main` | `https://run.cube-world.playserv.com` | **`web-cloudrun.yml`** |

A push to `main` that touches `web/**` or `deploy/web/**` deploys to Cloud Run, and to GitHub Pages (`pages.yml`), in
separate workflows. Neither waits for the other, and one failing doesn't stop the other. The VM workflows are
paused for now, deploying by hand only (`infra/vultr/README.md`, *Paused*). Both serve the same image, built from
`deploy/web/Dockerfile`. Each workflow builds and pushes its own copy: the VM pulls from GHCR, Cloud Run from
Artifact Registry.

```
 push to main ─► GitHub Actions (web-cloudrun.yml)
                   │ 1. OIDC token ─► Workload Identity Federation ─► short-lived token of cube-world-web-deploy
                   │ 2. docker build deploy/web/Dockerfile ─► <region>-docker.pkg.dev/<project>/cube-world-web/web:<sha>
                   │ 3. gcloud run services update --image …@sha256:…  (new revision, 100% of traffic)
                   ▼
 CNAME (by hand) run.cube-world ─► ghs.googlehosted.com ─► domain mapping ─► Cloud Run service cube-world-web
                                                                            (Caddy on $PORT, scales to zero)
```

## What Terraform creates

| Resource | What it is |
|---|---|
| `google_project_service.apis` | Enables Cloud Run, Artifact Registry, IAM, IAM Credentials and STS |
| `google_artifact_registry_repository.web` | Docker repository `cube-world-web`. It keeps the newest 10 versions and deletes the rest after a week |
| `google_cloud_run_v2_service.web` | Service `cube-world-web`: 1 vCPU, 256 MiB, 0–3 instances, public. It starts on Google's *hello* sample until the first deploy |
| `google_cloud_run_domain_mapping.web` | `run.cube-world.playserv.com` mapped to the service, with a certificate from Google |
| `google_service_account.runtime` | The identity the service runs as, with no roles |
| `google_service_account.deploy` + three IAM members | What GitHub deploys as: push to the one repository, update the one service, act as the runtime account |
| `google_iam_workload_identity_pool(_provider).github` | Lets GitHub Actions sign in with no key, and accepts only `playserv1/cube-world` on `main` |

Terraform owns everything about the service except its image and traffic. The workflow owns those (see
`lifecycle.ignore_changes` in `main.tf`), so a `terraform apply` never rolls back a deploy.

## Setup checklist

Do the steps in this order. The workflow runs on the first push to `main` that contains it, so GCP and GitHub must
be ready before that merge.

1. [Prepare](#1-prepare): a project with billing, `gcloud`, `terraform.tfvars`.
2. [Verify the domain with Google](#2-verify-the-domain-with-google). This is required before a domain mapping can be created.
3. [Apply](#3-apply).
4. [Create the CNAME](#4-create-the-cname) by hand, and wait for the certificate.
5. [Configure GitHub](#5-configure-github): the `cloud-run-prod` environment and its variables.
6. [Check the platform accepts the new origins](#6-check-the-platform-accepts-the-new-origins).
7. [First deploy](#7-first-deploy).

---

## 1. Prepare

- **A GCP project with billing enabled.** You need its **id**, which isn't the same as its name. You also need
  **Owner** on it, or enough roles to enable APIs and to create service accounts, Workload Identity pools, Artifact
  Registry repositories, Cloud Run services and IAM bindings.
- **Tools:** Terraform 1.6 or newer, and the [gcloud CLI](https://cloud.google.com/sdk/docs/install).
- **Credentials for Terraform:** your own user account, no key file.

  ```bash
  gcloud auth login
  gcloud auth application-default login
  gcloud config set project <gcp-project-id>
  ```

- **`terraform.tfvars`** is already in this folder (gitignored; `terraform.tfvars.example` is the committed copy). Set
  `project_id`. `domain` defaults to `run.cube-world.playserv.com`, because `cube-world.playserv.com` is the VM's
  prod site. `region` defaults to `europe-west1`. Keep a region that
  [supports domain mappings](https://cloud.google.com/run/docs/mapping-custom-domains#limitations):
  `europe-west3` (Frankfurt) doesn't.

## 2. Verify the domain with Google

Google creates a domain mapping only for a domain the caller has verified in Search Console. Here the caller is the
account you run Terraform with. You do this once, and it covers every subdomain of what you verify.

1. Run `gcloud domains verify cube-world.playserv.com`, or verify all of `playserv.com` instead. Either opens
   **Google Search Console**.
2. Pick a **Domain** property. Google shows a TXT record like `google-site-verification=…`.
3. In Cloudflare → `playserv.com` → **DNS** → **Add record**, create it: type `TXT`, name `cube-world` (or `@` for
   the whole domain), content the value Google showed.
4. Back in Search Console, click **Verify**. It can take a few minutes for the TXT record to be seen.
5. Check that `gcloud domains list-user-verified` lists the domain.

If someone else verified the domain already, they can add you under **Search Console → Settings → Users and
permissions → Add user** as an **Owner**. The account that runs Terraform must be an owner, not just a user.

## 3. Apply

```bash
cd infra/gcp
terraform init
terraform plan          # 17 to add
terraform apply
```

The first apply enables the APIs. If a resource then fails with *"API has not been used in project … or it is
disabled"*, the API is still propagating: wait a minute and run `terraform apply` again.

Check the service works before the domain does. It shows Google's *hello* page until the first deploy:

```bash
curl -s "$(terraform output -raw service_url)" | grep -i 'congratulations\|hello'
```

## 4. Create the CNAME

```bash
terraform apply -refresh-only -auto-approve   # picks up the records the mapping asks for
terraform output dns_records                  # e.g.  run.cube-world  CNAME  ghs.googlehosted.com.
```

If the output is empty, wait a minute and refresh again: Google fills in the records shortly after the mapping is
created. Then, in Cloudflare → `playserv.com` → **DNS** → **Add record**:

| Type | Name | Target | Proxy status | TTL |
|---|---|---|---|---|
| CNAME | `run.cube-world` | `ghs.googlehosted.com` | **DNS only** (grey cloud) | Auto |

It must be **DNS only**. Behind Cloudflare's proxy, Google can't see the CNAME and never issues the certificate.

Google issues the certificate once the CNAME resolves. That usually takes 15–30 minutes, and can take up to
24 hours. Watch it with:

```bash
dig +short run.cube-world.playserv.com     # ghs.googlehosted.com. and Google's addresses
gcloud beta run domain-mappings describe --domain run.cube-world.playserv.com --region europe-west1 \
  --format 'table(status.conditions[].type, status.conditions[].status, status.conditions[].message)'
```

The mapping is ready when `Ready`, `CertificateProvisioned` and `DomainRoutable` are all `True`.

## 5. Configure GitHub

In **Settings → Environments → New environment**, create `cloud-run-prod`. Under **Deployment branches and tags**
choose **Selected branches** and add `main`. GCP refuses other branches anyway; this keeps GitHub from even
starting a job for them.

Then set its variables. They are all in one Terraform output. From `infra/gcp`:

```bash
echo '{"deployment_branch_policy":{"protected_branches":false,"custom_branch_policies":true}}' |
  gh api -X PUT repos/playserv1/cube-world/environments/cloud-run-prod --input -
gh api -X POST repos/playserv1/cube-world/environments/cloud-run-prod/deployment-branch-policies -f name=main -f type=branch

terraform output -json github_variables | jq -r 'to_entries[] | "\(.key) \(.value)"' |
  while read -r name value; do gh variable set "$name" --env cloud-run-prod --body "$value"; done
gh variable list --env cloud-run-prod
```

| Variable | Example |
|---|---|
| `GCP_PROJECT_ID` | `my-project` |
| `GCP_REGION` | `europe-west1` |
| `GCP_WORKLOAD_IDENTITY_PROVIDER` | `projects/123456789/locations/global/workloadIdentityPools/cube-world-web-gh/providers/github` |
| `GCP_DEPLOY_SERVICE_ACCOUNT` | `cube-world-web-deploy@my-project.iam.gserviceaccount.com` |
| `CLOUD_RUN_SERVICE` | `cube-world-web` |
| `CLOUD_RUN_IMAGE` | `europe-west1-docker.pkg.dev/my-project/cube-world-web/web` |
| `CLOUD_RUN_HOST` | `run.cube-world.playserv.com` |
| `WEB_CONFIG_JS` (optional) | A whole `config.js` to use instead of the committed one, as for the VM's `web-prod` |

There are no secrets: none of these values grants anything on its own. Only a run of this repository's `main` can
turn them into a token.

## 6. Check the platform accepts the new origins

The page calls the PlayServ API from `https://run.cube-world.playserv.com`, and from the `service_url` if anyone
opens that one. If the platform or the project restricts which browser origins may use the client key, add both.
This is the same check as the VM guide's step 5: open the site, open DevTools → Console, click **Play**, and look for
CORS errors.

## 7. First deploy

The workflow runs on pushes to `main`, so it starts when this work reaches `main`, normally with the usual merge of
`dev` into `main`. That merge also brings `web/config.js`, which `main` lacks today; or set `WEB_CONFIG_JS` on
`cloud-run-prod`. After that, **Actions → Web client → Cloud Run → Run workflow** on `main` deploys again at any
time.

A good run:

1. Signs in as `cube-world-web-deploy@…` with no key.
2. Pushes `…/cube-world-web/web:<commit sha>`.
3. Runs `gcloud run services update`: a new revision gets 100% of traffic.
4. Ends with `https://run.cube-world.playserv.com and https://cube-world-web-….run.app serve <sha12>`.

If the certificate isn't issued yet, the run ends green with a warning that the custom domain doesn't answer yet.

---

## Operations

| Task | How |
|---|---|
| Revisions and traffic | `gcloud run revisions list --service cube-world-web --region europe-west1` |
| Logs | Cloud Console → Cloud Run → `cube-world-web` → **Logs**, or `gcloud logging read 'resource.type="cloud_run_revision" AND resource.labels.service_name="cube-world-web"' --limit 50` |
| Roll back | `gcloud run services update-traffic cube-world-web --region europe-west1 --to-revisions <revision>=100`. The next deploy moves traffic back to the newest revision, so revert the bad commit too. Or re-run an older successful run in Actions, which deploys that commit again |
| Change size or limits | Edit `main.tf` (`resources`, `scaling`) → `terraform apply`. It makes a new revision of the current image |
| Deploy from another branch | Change `deploy_branch` (apply), and both `branches:` and the `if:` in `web-cloudrun.yml`, and the environment's branch rule |

### Cost

With the traffic of a demo this stays within Cloud Run's free tier: 2 million requests and 180,000 vCPU-seconds a
month. The service scales to zero, and with `cpu_idle` it is billed only while it answers. Artifact Registry is free
up to 0.5 GB, and 10 versions of a ~50 MB image is about that. Domain mappings and their certificates cost nothing.

### Tear down

```bash
terraform destroy
```

Then delete the `run.cube-world` CNAME in Cloudflare and the `cloud-run-prod` GitHub environment. Delete the
verification TXT record too, unless anything else uses it, and disable or delete `web-cloudrun.yml`. A destroyed
Workload Identity pool keeps its id taken for 30 days, so an apply within that time fails on the pool. Either
restore it (`gcloud iam workload-identity-pools undelete cube-world-web-gh --location global`) and
`terraform import` it, or change `service_name`.

## Security notes

- **No keys anywhere.** Terraform uses your own login. GitHub gets a token valid for about an hour, and only for a
  run of `playserv1/cube-world` on `main` (`attribute_condition` in `github.tf`).
- **The deploy account's reach is narrow:** it can push to one Artifact Registry repository, change one Cloud Run
  service (`roles/run.developer` on that service only) and act as one runtime account. That runtime account itself
  has no roles.
- **The site is public** (`allUsers` → `roles/run.invoker`). It serves only static files and the public client key.

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `apply`: domain mapping `Caller is not authorized to administer the domain` | Step 2 isn't done for the account in `gcloud auth application-default login`, or that account is not an *Owner* in Search Console |
| `apply`: domain mapping `Location … is not supported` | `region` has no domain mappings. Pick one from the [list](https://cloud.google.com/run/docs/mapping-custom-domains#limitations). Changing it re-creates everything regional |
| `apply`: `allUsers` refused (`… do not belong to a permitted customer`) | The org policy *Domain restricted sharing* (`iam.allowedPolicyMemberDomains`) forbids public IAM. Ask for an exception on this project, or delete `google_cloud_run_v2_service_iam_member.public` and set `invoker_iam_disabled = true` on the service instead |
| `apply`: `… API has not been used in project` | The APIs were just enabled. Wait a minute and apply again |
| `apply`: pool `cube-world-web-gh` `already exists` | It was destroyed less than 30 days ago (see [Tear down](#tear-down)) |
| Custom domain: `CertificateProvisioned: False` for hours | The CNAME is proxied (orange cloud), points elsewhere, or a CAA record on `playserv.com` doesn't allow `pki.goog` |
| Workflow, *Sign in to GCP*: `unauthorized_client` / `attribute condition` | The run isn't from `playserv1/cube-world` on `main`, for example the repo was renamed or the run is on another branch |
| Workflow: `Permission 'iam.serviceAccounts.getAccessToken' denied` | The `workloadIdentityUser` binding is new (wait 5 minutes), or `GCP_DEPLOY_SERVICE_ACCOUNT` / `GCP_WORKLOAD_IDENTITY_PROVIDER` doesn't match the outputs |
| Workflow, push: `denied: Permission "artifactregistry.repositories.uploadArtifacts"` | `CLOUD_RUN_IMAGE` points at another repository or region than Terraform's |
| Workflow, deploy: `PERMISSION_DENIED … iam.serviceaccounts.actAs` | `google_service_account_iam_member.deploy_acts_as_runtime` is missing; run `terraform apply` |
| Workflow, deploy: `PERMISSION_DENIED` on `run.operations.get` or similar | Your gcloud version needs a project-level permission. Grant `roles/run.developer` on the project to the deploy account, or add an IAM condition limiting it to this service |
| New revision doesn't start (`container failed to listen on PORT`) | The image doesn't listen on `$PORT`. Check `deploy/web/Caddyfile` keeps `:{$PORT:8080}` |
| Workflow: `web/config.js is missing` | See [step 7](#7-first-deploy) |
