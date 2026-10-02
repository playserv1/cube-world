# How GitHub Actions deploys without a key: a run's OIDC token (from this repository, on var.deploy_branch only) is
# exchanged for a short-lived token of the deploy service account, which can push images and update this one service.

resource "google_service_account" "deploy" {
  account_id   = "${var.service_name}-deploy"
  display_name = "Cube World web (GitHub Actions deploys)"

  depends_on = [google_project_service.apis]
}

resource "google_iam_workload_identity_pool" "github" {
  # A deleted pool's id stays taken for 30 days, so it is specific to this service, not a generic "github".
  workload_identity_pool_id = "${var.service_name}-gh"
  display_name              = "GitHub Actions: cube-world web"

  depends_on = [google_project_service.apis]
}

resource "google_iam_workload_identity_pool_provider" "github" {
  workload_identity_pool_id          = google_iam_workload_identity_pool.github.workload_identity_pool_id
  workload_identity_pool_provider_id = "github"
  display_name                       = "GitHub Actions OIDC" # at most 32 characters

  attribute_mapping = {
    "google.subject"       = "assertion.sub"
    "attribute.repository" = "assertion.repository"
    "attribute.ref"        = "assertion.ref"
  }
  # Tokens of any other repository or branch are refused before any role is looked at.
  attribute_condition = "assertion.repository == '${var.github_repository}' && assertion.ref == 'refs/heads/${var.deploy_branch}'"

  oidc {
    issuer_uri = "https://token.actions.githubusercontent.com"
  }
}

resource "google_service_account_iam_member" "deploy_from_github" {
  service_account_id = google_service_account.deploy.name
  role               = "roles/iam.workloadIdentityUser"
  member             = "principalSet://iam.googleapis.com/${google_iam_workload_identity_pool.github.name}/attribute.repository/${var.github_repository}"
}

# What the deploy account may do, each on one resource only.
resource "google_artifact_registry_repository_iam_member" "deploy_push" {
  repository = google_artifact_registry_repository.web.name
  location   = google_artifact_registry_repository.web.location
  role       = "roles/artifactregistry.writer"
  member     = google_service_account.deploy.member
}

resource "google_cloud_run_v2_service_iam_member" "deploy_update" {
  name     = google_cloud_run_v2_service.web.name
  location = google_cloud_run_v2_service.web.location
  role     = "roles/run.developer"
  member   = google_service_account.deploy.member
}

# A new revision runs as the runtime account, and deploying it means acting as that account.
resource "google_service_account_iam_member" "deploy_acts_as_runtime" {
  service_account_id = google_service_account.runtime.name
  role               = "roles/iam.serviceAccountUser"
  member             = google_service_account.deploy.member
}
