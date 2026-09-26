terraform {
  required_version = ">= 1.6"

  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 8.5"
    }
  }
}

# Credentials are your own: `gcloud auth application-default login` (README.md, step 1). Nothing in this folder
# holds a key, and GitHub Actions gets none either: it signs in through Workload Identity Federation (github.tf).
provider "google" {
  project = var.project_id
  region  = var.region
}
