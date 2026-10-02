terraform {
  required_version = ">= 1.6"

  required_providers {
    vultr = {
      source  = "vultr/vultr"
      version = "~> 2.32"
    }
    tls = {
      source  = "hashicorp/tls"
      version = "~> 4.4"
    }
  }
}

# The API key comes from the environment (VULTR_API_KEY, Vultr → Account → API), never from a file in this folder.
# DNS is not managed here: the records for var.sites are created by hand (README.md, step 2).
provider "vultr" {}
