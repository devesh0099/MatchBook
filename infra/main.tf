# The platform: three instances in one VPC, only the web node reachable.
#
#   terraform -chdir=infra/bootstrap apply     # once — bucket and IAM
#   terraform -chdir=infra init
#   terraform -chdir=infra plan                # creates nothing
#   terraform -chdir=infra apply
#
# This creates infrastructure only. It does not run the node setup scripts or
# calibrate anything: ops/*-setup.sh are the provisioning, and the calibration
# steps are measurements rather than configuration.

terraform {
  required_version = ">= 1.5"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

provider "aws" {
  region  = var.aws_region
  profile = var.aws_profile

  # Assumed only when deployer_role_arn is set. Unset, the profile's own
  # credentials are used directly — correct when those credentials already carry
  # the deployment policy. Set, Terraform runs under the assumed role instead,
  # which constrains an otherwise unrestricted profile to the same policy.
  dynamic "assume_role" {
    for_each = var.deployer_role_arn == null ? [] : [1]
    content {
      role_arn     = var.deployer_role_arn
      session_name = "${var.name_prefix}-terraform"
    }
  }

  # Project is what the deployer policy conditions on and what
  # ops/aws/status.sh filters by, so it is not decoration.
  default_tags {
    tags = {
      Project   = var.name_prefix
      ManagedBy = "terraform"
    }
  }
}

# Every name this module creates. See var.name_prefix.
locals {
  key_name         = coalesce(var.key_name, var.name_prefix)
  instance_profile = coalesce(var.instance_profile, "${var.name_prefix}-node")
}

# ------------------------------------------------------------------ key pair
# Imported rather than generated: a key Terraform generates would have its
# private half in the state file.
#
# key_name_PREFIX, not key_name. With a fixed name, pointing public_key_path at
# a new key replaces the key pair but leaves the NAME unchanged — so the
# instances see no change, are not replaced, and keep the old key in
# authorized_keys. Rotation through Terraform silently did nothing. A generated
# name changes with the material, which forces the replacement that actually
# rotates the key, and incidentally removes the InvalidKeyPair.Duplicate failure
# you get re-applying over a key pair someone made by hand.
resource "aws_key_pair" "this" {
  key_name_prefix = local.key_name
  public_key      = file(var.public_key_path)

  lifecycle {
    create_before_destroy = true
  }
}

# ------------------------------------------------------------ security groups
# Two groups, and the rule that matters is Postgres on the web node accepting
# ONLY from the worker group — a CIDR would either be too wide or break the
# moment an instance is replaced and takes a new private IP.

resource "aws_security_group" "worker" {
  name_prefix = "${var.name_prefix}-worker-"
  description = "Pool and bench nodes. No inbound except administrative SSH."
  vpc_id      = var.vpc_id

  ingress {
    description = "SSH. Key-only auth is the control here, not the CIDR."
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = var.ssh_cidrs
  }

  # One of these two applies, never both. See var.restrict_worker_egress.
  dynamic "egress" {
    for_each = var.restrict_worker_egress ? [] : [1]
    content {
      description = "apt, GitHub, S3, and Postgres on the web node"
      from_port   = 0
      to_port     = 0
      protocol    = "-1"
      cidr_blocks = ["0.0.0.0/0"]
    }
  }

  dynamic "egress" {
    for_each = var.restrict_worker_egress ? local.worker_egress : {}
    content {
      description     = egress.value.description
      from_port       = egress.value.port
      to_port         = egress.value.port
      protocol        = egress.value.protocol
      cidr_blocks     = egress.key == "postgres" ? [] : ["0.0.0.0/0"]
      security_groups = egress.key == "postgres" ? [aws_security_group.web.id] : null
  }

  lifecycle {
    create_before_destroy = true
  }
}

locals {
  # Everything the worker nodes reach outbound. DNS and NTP are the two people
  # forget: without 53 nothing resolves, and without 123 the clock drifts, which
  # on a node whose whole output is timing is not a cosmetic problem.
  worker_egress = {
    http     = { port = 80, protocol = "tcp", description = "apt" }
    https    = { port = 443, protocol = "tcp", description = "apt, rustup, GitHub, S3" }
    postgres = { port = 5432, protocol = "tcp", description = "the job queue on the web node" }
    dns      = { port = 53, protocol = "udp", description = "VPC resolver" }
    dns_tcp  = { port = 53, protocol = "tcp", description = "VPC resolver, large answers" }
    ntp      = { port = 123, protocol = "udp", description = "clock discipline" }
  }
}

resource "aws_security_group" "web" {
  name_prefix = "${var.name_prefix}-web-"
  description = "Web node: participants reach Caddy, workers reach Postgres."
  vpc_id      = var.vpc_id

  ingress {
    description = "SSH. Key-only auth is the control here, not the CIDR."
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = var.ssh_cidrs
  }

  # Postgres is reachable from the WORKER GROUP, not from a CIDR. This is the
  # rule that is fiddly to get right by hand and the reason DB_BIND must be the
  # private IP rather than loopback.
  ingress {
    description     = "Postgres, workers only"
    from_port       = 5432
    to_port         = 5432
    protocol        = "tcp"
    security_groups = [aws_security_group.worker.id]
  }

  ingress {
    description = "HTTP"
    from_port   = 80
    to_port     = 80
    protocol    = "tcp"
    cidr_blocks = var.web_ingress_cidrs
  }

  ingress {
    description = "HTTPS"
    from_port   = 443
    to_port     = 443
    protocol    = "tcp"
    cidr_blocks = var.web_ingress_cidrs
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  lifecycle {
    create_before_destroy = true
  }
}

# ------------------------------------------------------------------ instances

locals {
  user_data = templatefile("${path.module}/user_data.sh.tftpl", {
    repo_url = var.repo_url
    repo_ref = var.repo_ref
  })
}

# user_data_replace_on_change is on for all three. Without it the provider
# updates the attribute in place and leaves the instance running — but
# user_data only ever executes on FIRST boot, so an edit would silently have no
# effect and you would be debugging a fix that never ran.
resource "aws_instance" "web" {
  ami                         = var.ami_id
  instance_type               = var.web_instance_type
  subnet_id                   = var.subnet_id
  key_name                    = aws_key_pair.this.key_name
  vpc_security_group_ids      = [aws_security_group.web.id]
  iam_instance_profile        = local.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true
  associate_public_ip_address = var.associate_public_ip

  # hop limit 2, and ONLY on this node. The API runs in a container on Docker's
  # bridge network, so its request to 169.254.169.254 crosses the host network
  # namespace and arrives at hop 2. At the EC2 default of 1 it is dropped, the
  # Rust SDK's IMDSv2-only client cannot get credentials, and because the API
  # writes submission source to S3 before doing anything else, every /run and
  # /submit returns an opaque internal error — indistinguishable from the
  # missing-bucket failure, and NOT reproducible with curl from the host shell,
  # which is one hop and succeeds.
  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 2
    instance_metadata_tags      = "disabled"
  }

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
    encrypted   = true
  }

  tags = { Name = "${var.name_prefix}-web", Role = "web" }
}

resource "aws_instance" "pool" {
  ami                         = var.ami_id
  instance_type               = var.pool_instance_type
  subnet_id                   = var.subnet_id
  key_name                    = aws_key_pair.this.key_name
  vpc_security_group_ids      = [aws_security_group.worker.id]
  iam_instance_profile        = local.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true
  associate_public_ip_address = var.associate_public_ip

  # This node compiles and runs participant-submitted C++. The worker is a plain
  # systemd process, not a container, so one hop is enough — and keeping it at 1
  # means a container started here later cannot reach the credentials by
  # accident. http_tokens = "required" turns IMDSv1 off: with it optional, any
  # SSRF or isolate escape reads the instance credentials with a single
  # unauthenticated GET, and those credentials can overwrite every participant's
  # source in the artifact bucket.
  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
    instance_metadata_tags      = "disabled"
  }

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
    encrypted   = true
  }

  tags = { Name = "${var.name_prefix}-pool", Role = "pool" }
}

# The bench node. Two settings here CANNOT be changed after launch, which is
# the main reason this file exists at all:
#
#   * tenancy — "dedicated" costs a flat $2/hr per region on top of a ~10%
#     instance premium, so it is off by default. Turn it on only if
#     ops/noise-floor/analyze.py says the spread justifies it. A cheaper plan
#     is to leave it off all day and launch a second, dedicated bench node for
#     the rejudge block alone: same instance type, roughly two hours, ~$5.
#
#   * threads_per_core = 1 — SMT off. A sibling thread sharing the core with
#     the timed run is exactly the contamination the whole design refuses.
#     With SMT off a c6i.2xlarge presents 4 cores, which is what BENCH_CPU and
#     ISOLATED_CPUS must be set against.
resource "aws_instance" "bench" {
  ami                         = var.ami_id
  instance_type               = var.bench_instance_type
  subnet_id                   = var.subnet_id
  key_name                    = aws_key_pair.this.key_name
  vpc_security_group_ids      = [aws_security_group.worker.id]
  iam_instance_profile        = local.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true
  associate_public_ip_address = var.associate_public_ip
  tenancy                     = var.bench_dedicated ? "dedicated" : "default"

  cpu_options {
    core_count       = var.bench_core_count
    threads_per_core = 1
  }

  # As pool: untrusted code, no containers, so one hop and IMDSv2 only.
  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
    instance_metadata_tags      = "disabled"
  }

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
    encrypted   = true
  }

  tags = { Name = "${var.name_prefix}-bench", Role = "bench" }
}
