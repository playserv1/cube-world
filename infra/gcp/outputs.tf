locals {
  image = "${var.region}-docker.pkg.dev/${var.project_id}/${google_artifact_registry_repository.web.repository_id}/web"
}

output "service_url" {
  description = "The service's own run.app URL. It works before the custom domain does."
  value       = google_cloud_run_v2_service.web.uri
}

output "site" {
  description = "The custom domain, once its CNAME resolves and Google has issued the certificate."
  value       = "https://${var.domain}"
}

output "dns_records" {
  description = "The records to create by hand at the DNS provider (DNS only, not proxied)."
  value = [
    for r in try(google_cloud_run_domain_mapping.web.status[0].resource_records, []) :
    "${r.name == "" ? var.domain : r.name}  ${r.type}  ${r.rrdata}"
  ]
}

output "github_variables" {
  description = "The variables of the cloud-run-prod GitHub environment (README.md, step 5)."
  value = {
    GCP_PROJECT_ID                 = var.project_id
    GCP_REGION                     = var.region
    GCP_WORKLOAD_IDENTITY_PROVIDER = google_iam_workload_identity_pool_provider.github.name
    GCP_DEPLOY_SERVICE_ACCOUNT     = google_service_account.deploy.email
    CLOUD_RUN_SERVICE              = google_cloud_run_v2_service.web.name
    CLOUD_RUN_IMAGE                = local.image
    CLOUD_RUN_HOST                 = var.domain
  }
}
