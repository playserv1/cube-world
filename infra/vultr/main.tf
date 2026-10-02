# The VM that serves the browser client (web/): one edge Caddy in Docker, one site per environment in var.sites,
# certificates from Let's Encrypt. What runs on it is set up by cloud-init (cloud-init.tf); README.md is the guide.

data "vultr_os" "ubuntu" {
  filter {
    name   = "name"
    values = [var.os_name]
  }
}

# The VM's SSH host key is made here, not on the VM, so CI can pin it before the VM exists and keeps the same
# known_hosts line when the VM is rebuilt.
resource "tls_private_key" "host" {
  algorithm = "ED25519"
}

# The key GitHub Actions deploys with. On the VM it logs in as `deploy` and can only run /usr/local/bin/cube-deploy.
resource "tls_private_key" "deploy" {
  algorithm = "ED25519"
}

# An address that outlives the VM: the hand-made DNS records and CI's known_hosts keep pointing at it across rebuilds.
resource "vultr_reserved_ip" "web" {
  label   = var.name
  region  = var.region
  ip_type = "v4"
}

resource "vultr_firewall_group" "web" {
  description = "${var.name}: SSH, HTTP, HTTPS"
}

locals {
  everyone = {
    v4 = { subnet = "0.0.0.0", size = 0 }
    v6 = { subnet = "::", size = 0 }
  }

  public_rules = merge([
    for ip_type, net in local.everyone : {
      "http-${ip_type}"  = { protocol = "tcp", port = "80", ip_type = ip_type, subnet = net.subnet, size = net.size, notes = "HTTP: ACME challenges, redirect to HTTPS" }
      "https-${ip_type}" = { protocol = "tcp", port = "443", ip_type = ip_type, subnet = net.subnet, size = net.size, notes = "HTTPS" }
      "h3-${ip_type}"    = { protocol = "udp", port = "443", ip_type = ip_type, subnet = net.subnet, size = net.size, notes = "HTTP/3" }
      "icmp-${ip_type}"  = { protocol = "icmp", port = null, ip_type = ip_type, subnet = net.subnet, size = net.size, notes = "ping, path MTU" }
    }
  ]...)

  ssh_rules = {
    for cidr in var.ssh_allowed_cidrs : "ssh-${cidr}" => {
      protocol = "tcp"
      port     = "22"
      ip_type  = strcontains(cidr, ":") ? "v6" : "v4"
      subnet   = split("/", cidr)[0]
      size     = tonumber(split("/", cidr)[1])
      notes    = "SSH: admins and GitHub Actions deploys"
    }
  }
}

resource "vultr_firewall_rule" "web" {
  for_each = merge(local.public_rules, local.ssh_rules)

  firewall_group_id = vultr_firewall_group.web.id
  protocol          = each.value.protocol
  ip_type           = each.value.ip_type
  subnet            = each.value.subnet
  subnet_size       = each.value.size
  port              = each.value.port
  notes             = each.value.notes
}

# cloud-init runs once, on the first boot. A change to what it sets up therefore replaces the VM rather than
# quietly leaving the old one as it was: see "Changing the VM" in README.md.
resource "terraform_data" "cloud_init" {
  input = sha256(local.user_data)
}

resource "vultr_instance" "web" {
  label             = var.name
  hostname          = var.name
  region            = var.region
  plan              = var.plan
  os_id             = data.vultr_os.ubuntu.id
  enable_ipv6       = true
  backups           = "disabled" # nothing on the VM is kept: every site is redeployed from git
  activation_email  = false
  firewall_group_id = vultr_firewall_group.web.id
  reserved_ip_id    = vultr_reserved_ip.web.id
  user_data         = local.user_data
  tags              = ["cube-world", "web"]

  lifecycle {
    replace_triggered_by = [terraform_data.cloud_init]
  }
}
