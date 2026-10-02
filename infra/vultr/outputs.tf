output "ipv4" {
  description = "The reserved IPv4 of the VM: the target of every A record, and the repository variable WEB_VM_HOST. It stays the same when the VM is rebuilt."
  value       = vultr_reserved_ip.web.subnet
}

output "ipv6" {
  description = "The VM's IPv6, for optional AAAA records. Unlike the IPv4 it changes when the VM is rebuilt."
  value       = vultr_instance.web.v6_main_ip
}

output "dns_records" {
  description = "The records to create by hand at the DNS provider (DNS only, not proxied)."
  value       = [for host in values(var.sites) : "${host}  A  ${vultr_reserved_ip.web.subnet}"]
}

output "sites" {
  description = "The URL of each environment."
  value       = { for env, host in var.sites : env => "https://${host}" }
}

output "ssh_admin" {
  description = "How an admin logs in."
  value       = "ssh ops@${vultr_reserved_ip.web.subnet}"
}

output "ssh_known_hosts" {
  description = "The VM's host key as a known_hosts line: the repository variable WEB_DEPLOY_KNOWN_HOSTS."
  value       = "${vultr_reserved_ip.web.subnet} ${trimspace(tls_private_key.host.public_key_openssh)}"
}

output "deploy_private_key" {
  description = "The CI deploy key: the repository secret WEB_DEPLOY_SSH_KEY. `terraform output -raw deploy_private_key`."
  value       = tls_private_key.deploy.private_key_openssh
  sensitive   = true
}
