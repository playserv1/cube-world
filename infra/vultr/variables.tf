variable "name" {
  description = "Label and hostname of the VM, and the prefix of everything else this folder creates in Vultr."
  type        = string
  default     = "cube-world-web"
}

variable "region" {
  description = "Vultr region id (`curl -s https://api.vultr.com/v2/regions | jq '.regions[].id'`). fra is next to the project's platform region."
  type        = string
  default     = "fra"
}

variable "plan" {
  description = "Vultr plan id. The VM only serves static files, so the smallest shared-CPU plan is plenty."
  type        = string
  default     = "vc2-1c-1gb"
}

variable "os_name" {
  description = "Exact Vultr OS name; cloud-init and the package names in cloud-init.tf are written for Ubuntu 24.04."
  type        = string
  default     = "Ubuntu 24.04 LTS x64"
}

variable "sites" {
  description = <<-EOT
    One site per environment: environment name → hostname. The names are the ones the deploy workflows use
    (main → prod, dev → dev). Each hostname needs a DNS record pointing at the VM's IP, created by hand.
  EOT
  type        = map(string)

  validation {
    condition     = length(var.sites) > 0 && alltrue([for env in keys(var.sites) : can(regex("^[a-z][a-z0-9]{0,15}$", env))])
    error_message = "Give at least one site; environment names are lowercase letters and digits, starting with a letter."
  }
}

variable "acme_email" {
  description = "E-mail Let's Encrypt writes to about expiring or revoked certificates."
  type        = string
}

variable "admin_ssh_public_keys" {
  description = "Public SSH keys of the people who administer the VM; they log in as `ops` (sudo)."
  type        = list(string)

  validation {
    condition     = length(var.admin_ssh_public_keys) > 0
    error_message = "Give at least one admin key, or nobody can log in: root and passwords are off."
  }
}

variable "ssh_allowed_cidrs" {
  description = <<-EOT
    Networks allowed to reach SSH. GitHub-hosted runners deploy over SSH from addresses that change all the time,
    so the default is everyone; SSH takes keys only, and the CI key can run nothing but the deploy command.
  EOT
  type        = list(string)
  default     = ["0.0.0.0/0", "::/0"]

  validation {
    condition     = alltrue([for cidr in var.ssh_allowed_cidrs : can(cidrhost(cidr, 0))])
    error_message = "Every entry must be a CIDR, e.g. 203.0.113.7/32 or 2001:db8::/32."
  }
}

variable "web_image" {
  description = "Repository of the web client image the image deploy pulls (built by .github/workflows/web-image.yml). The VM refuses any other image."
  type        = string
  default     = "ghcr.io/playserv1/cube-world-web"
}

variable "caddy_image" {
  description = "Image of the edge Caddy on the VM."
  type        = string
  default     = "caddy:2.11-alpine"
}
