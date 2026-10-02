# The browser client (web/) on Cloud Run: the same image as the Vultr VM's image deploy (deploy/web/Dockerfile),
# built by .github/workflows/web-cloudrun.yml into Artifact Registry. Only the prod site, from main. README.md is the guide.

resource "google_project_service" "apis" {
  for_each = toset([
    "run.googleapis.com",
    "artifactregistry.googleapis.com",
    "iam.googleapis.com",
    "iamcredentials.googleapis.com", # GitHub's federated identity is exchanged for the deploy account's token
    "sts.googleapis.com",
  ])

  service            = each.value
  disable_on_destroy = false # other things in the project may use them too
}

resource "google_artifact_registry_repository" "web" {
  repository_id = var.service_name
  location      = var.region
  format        = "DOCKER"
  description   = "Cube World web client images, pushed by ${var.github_repository} (.github/workflows/web-cloudrun.yml)"

  cleanup_policy_dry_run = false
  cleanup_policies {
    id     = "keep-recent"
    action = "KEEP"
    most_recent_versions {
      keep_count = var.image_keep_count
    }
  }
  cleanup_policies {
    id     = "delete-old"
    action = "DELETE" # KEEP wins over DELETE: only versions beyond the newest image_keep_count go
    condition {
      tag_state  = "ANY"
      older_than = "604800s"
    }
  }

  depends_on = [google_project_service.apis]
}

# The identity the service runs as. It is given no roles: the client is static files and calls nothing in GCP.
resource "google_service_account" "runtime" {
  account_id   = "${var.service_name}-run"
  display_name = "Cube World web (Cloud Run runtime)"

  depends_on = [google_project_service.apis]
}

resource "google_cloud_run_v2_service" "web" {
  name                = var.service_name
  location            = var.region
  ingress             = "INGRESS_TRAFFIC_ALL"
  deletion_protection = false # `terraform destroy` removes it; nothing in it is kept anyway

  template {
    service_account = google_service_account.runtime.email

    scaling {
      min_instance_count = 0
      max_instance_count = var.max_instances
    }

    containers {
      # Google's sample, until the first deploy replaces it. The workflow owns the image from then on (lifecycle below).
      image = "us-docker.pkg.dev/cloudrun/container/hello"

      ports {
        container_port = 8080
      }

      resources {
        limits = {
          cpu    = "1"
          memory = "256Mi"
        }
        cpu_idle          = true # billed only while serving a request
        startup_cpu_boost = true
      }
    }
  }

  lifecycle {
    # The workflow deploys new images (gcloud run services update) and moves traffic to them; a hand-made rollback
    # pins traffic too. Terraform keeps the rest of the service.
    ignore_changes = [
      template[0].containers[0].image,
      traffic,
      client,
      client_version,
      annotations,
      labels,
      template[0].annotations,
      template[0].labels,
    ]
  }

  depends_on = [google_project_service.apis]
}

# The site is public. An organization policy restricting allUsers blocks this: see README.md, Troubleshooting.
resource "google_cloud_run_v2_service_iam_member" "public" {
  name     = google_cloud_run_v2_service.web.name
  location = google_cloud_run_v2_service.web.location
  role     = "roles/run.invoker"
  member   = "allUsers"
}

# Creating it needs you to be a verified owner of the domain (README.md, step 2). The certificate is Google's,
# issued once the CNAME from the `dns_record` output resolves.
resource "google_cloud_run_domain_mapping" "web" {
  name     = var.domain
  location = var.region

  metadata {
    namespace = var.project_id
  }

  spec {
    route_name = google_cloud_run_v2_service.web.name
  }
}
