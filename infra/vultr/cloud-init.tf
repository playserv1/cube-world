# What the VM runs, set up by cloud-init on its first boot:
#
#   /opt/cube-world/compose.yaml        caddy (the edge: ports 80/443, certificates, one site per environment)
#                                       web-<env> (an environment's image, when it is served as one; profile <env>)
#   /opt/cube-world/caddy/Caddyfile     the edge config; caddy/sites/<env>.caddy is written by cube-deploy
#   /opt/cube-world/.env                each environment's image (WEB_<ENV>_IMAGE), written by cube-deploy
#                                       (deploy owns .env, caddy/sites and /srv/cube-world; the rest is root's)
#   /srv/cube-world/releases/<env>/     releases uploaded with rsync; sites/<env> links to the one being served
#   /usr/local/bin/cube-deploy          the deploy command (files/cube-deploy.sh)
#
# Users: ops (admins, sudo) and deploy (CI: docker, forced to run cube-deploy). Root and passwords cannot log in.

locals {
  app_dir = "/opt/cube-world"
  srv_dir = "/srv/cube-world"

  hardened = {
    cap_drop     = ["ALL"]
    cap_add      = ["NET_BIND_SERVICE"] # the caddy binary carries this file capability; without it, it cannot start
    security_opt = ["no-new-privileges:true"]
  }

  compose = {
    name = "cube-world"
    services = merge(
      {
        caddy = merge(local.hardened, {
          image   = var.caddy_image
          restart = "unless-stopped"
          ports   = ["80:80", "443:443", "443:443/udp"]
          volumes = [
            "./caddy:/etc/caddy:ro", # the folder, not the file: cube-deploy replaces site files by renaming them
            "${local.srv_dir}:${local.srv_dir}:ro",
            "caddy_data:/data", # certificates and ACME account: they survive container updates
            "caddy_config:/config",
          ]
        })
      },
      {
        for env in keys(var.sites) : "web-${env}" => merge(local.hardened, {
          # Set by `cube-deploy image`; the fallback only keeps compose from failing before the first image deploy.
          image    = "$${WEB_${upper(env)}_IMAGE:-${var.caddy_image}}"
          profiles = [env]
          restart  = "unless-stopped"
          healthcheck = {
            test         = ["CMD", "wget", "-qO", "/dev/null", "http://127.0.0.1:8080/"]
            interval     = "10s"
            timeout      = "3s"
            retries      = 3
            start_period = "5s"
          }
        })
      }
    )
    volumes = {
      caddy_data   = {}
      caddy_config = {}
    }
  }

  files = concat(
    [
      {
        path        = "/etc/ssh/sshd_config.d/10-cube-world.conf"
        permissions = "0644"
        content     = <<-EOT
          # Keys only, and only the two users cloud-init made. Sorted before 50-cloud-init.conf, so these win.
          PermitRootLogin no
          PasswordAuthentication no
          KbdInteractiveAuthentication no
          AllowUsers ops deploy
        EOT
      },
      {
        path        = "/etc/docker/daemon.json"
        permissions = "0644"
        content = jsonencode({
          "log-driver"   = "json-file"
          "log-opts"     = { "max-size" = "10m", "max-file" = "3" }
          "live-restore" = true
        })
      },
      {
        path        = "${local.app_dir}/compose.yaml"
        permissions = "0644"
        content     = "# Managed by Terraform (infra/vultr/cloud-init.tf).\n${yamlencode(local.compose)}"
      },
      {
        path        = "${local.app_dir}/caddy/Caddyfile"
        permissions = "0644"
        content     = templatefile("${path.module}/files/Caddyfile.tftpl", { acme_email = var.acme_email })
      },
      {
        path        = "${local.app_dir}/sites.conf"
        permissions = "0644"
        content     = join("", [for env, host in var.sites : "${env} ${host}\n"])
      },
      {
        path        = "${local.app_dir}/deploy.conf"
        permissions = "0644"
        content     = "WEB_IMAGE=${var.web_image}\n"
      },
      {
        path        = "/usr/local/bin/cube-deploy"
        permissions = "0755"
        content     = file("${path.module}/files/cube-deploy.sh")
      },
    ],
    [
      for env in keys(var.sites) : {
        path        = "${local.srv_dir}/releases/${env}/placeholder/index.html"
        permissions = "0644"
        content     = file("${path.module}/files/placeholder.html")
      }
    ],
  )

  cloud_config = {
    hostname        = var.name
    package_update  = true
    package_upgrade = true

    apt = {
      sources = {
        docker = {
          source = "deb [arch=amd64 signed-by=$KEY_FILE] https://download.docker.com/linux/ubuntu $RELEASE stable"
          key    = file("${path.module}/files/docker.asc") # Docker's release key, 9DC8 5822 9FC7 DD38 854A E2D8 8D81 803C 0EBF CD88
        }
      }
    }
    packages = ["docker-ce", "docker-ce-cli", "containerd.io", "docker-compose-plugin", "rsync", "unattended-upgrades"]

    # Made before the users, so deploy can join it; the docker-ce package keeps an existing docker group.
    groups = ["docker"]
    users = [
      {
        name                = "ops"
        gecos               = "Cube World admin"
        groups              = ["sudo", "docker"]
        sudo                = "ALL=(ALL) NOPASSWD:ALL"
        shell               = "/bin/bash"
        lock_passwd         = true
        ssh_authorized_keys = var.admin_ssh_public_keys
      },
      {
        name        = "deploy"
        gecos       = "Cube World CI deploys"
        groups      = ["docker"]
        shell       = "/bin/bash"
        lock_passwd = true
        ssh_authorized_keys = [
          "restrict,command=\"/usr/local/bin/cube-deploy\" ${trimspace(tls_private_key.deploy.public_key_openssh)} github-actions",
        ]
      },
    ]
    disable_root = true
    ssh_pwauth   = false

    # The host key Terraform made (main.tf), and no other.
    ssh_deletekeys  = true
    ssh_genkeytypes = []
    ssh_keys = {
      ed25519_private = tls_private_key.host.private_key_openssh
      ed25519_public  = trimspace(tls_private_key.host.public_key_openssh)
    }

    swap = {
      filename = "/swapfile"
      size     = "1G"
    }

    write_files = local.files

    runcmd = concat(
      [
        ["systemctl", "restart", "ssh"],
        # deploy owns only what cube-deploy writes; the config Terraform wrote stays root's.
        ["install", "-d", "-o", "deploy", "-g", "deploy", "${local.srv_dir}/sites", "${local.app_dir}/caddy/sites"],
        ["install", "-o", "deploy", "-g", "deploy", "-m", "0644", "/dev/null", "${local.app_dir}/.env"],
        ["install", "-o", "deploy", "-g", "deploy", "-m", "0644", "/dev/null", "${local.app_dir}/.lock"],
        ["chown", "-R", "deploy:deploy", local.srv_dir],
        ["sudo", "-u", "deploy", "docker", "compose", "-f", "${local.app_dir}/compose.yaml", "up", "-d", "caddy"],
      ],
      # Each site starts on the placeholder page; this also proves the deploy command works on this VM.
      [for env in keys(var.sites) : ["sudo", "-u", "deploy", "/usr/local/bin/cube-deploy", "files", env, "placeholder"]],
    )
  }

  user_data = "#cloud-config\n${yamlencode(local.cloud_config)}"
}
