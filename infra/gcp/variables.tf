variable "project_id" {
  description = "The GCP project the Cloud Run site lives in. It must exist and have billing enabled."
  type        = string
}

variable "region" {
  description = <<-EOT
    Region of the service, its image repository and its domain mapping. Domain mappings exist only in some regions
    (https://cloud.google.com/run/docs/mapping-custom-domains#limitations): europe-west1 is the nearest to fra;
    europe-west3 (Frankfurt) has none.
  EOT
  type        = string
  default     = "europe-west1"
}

variable "service_name" {
  description = "Name of the Cloud Run service, its runtime service account and its Artifact Registry repository."
  type        = string
  default     = "cube-world-web"
}

variable "domain" {
  description = "The custom domain mapped to the service. Its CNAME is created by hand (README.md, step 4)."
  type        = string
}

variable "github_repository" {
  description = "owner/name of the repository whose workflow deploys the service."
  type        = string
  default     = "playserv1/cube-world"
}

variable "deploy_branches" {
  description = "The only branches whose workflow runs can sign in to GCP and deploy (web-cloudrun.yml runs on dev and main)."
  type        = list(string)
  default     = ["main", "dev"]

  validation {
    condition     = length(var.deploy_branches) > 0 && alltrue([for b in var.deploy_branches : can(regex("^[A-Za-z0-9._/-]+$", b))])
    error_message = "Give at least one branch name; names hold letters, digits and . _ / - only."
  }
}

variable "max_instances" {
  description = "Upper bound of instances. The service scales to zero when idle and one instance serves many players' page loads."
  type        = number
  default     = 3
}

variable "image_keep_count" {
  description = "Image versions kept in Artifact Registry; older ones are deleted after a week. Rollback can go back this far."
  type        = number
  default     = 10
}
