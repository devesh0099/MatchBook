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
      session_name = "me-platform-terraform"
    }
  }

  default_tags {
    tags = {
      Project = "me-platform"
    }
  }
}

# ------------------------------------------------------------------ key pair
# Imported rather than generated: a key Terraform generates would have its
# private half in the state file.

resource "aws_key_pair" "this" {
  key_name   = var.key_name
  public_key = file(var.public_key_path)
}

# ------------------------------------------------------------ security groups
# Two groups, and the rule that matters is Postgres on the web node accepting
# ONLY from the worker group — a CIDR would either be too wide or break the
# moment an instance is replaced and takes a new private IP.

resource "aws_security_group" "worker" {
  name_prefix = "me-worker-"
  description = "Pool and bench nodes. No inbound except administrative SSH."
  vpc_id      = var.vpc_id

  ingress {
    description = "SSH. Key-only auth is the control here, not the CIDR."
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = var.ssh_cidrs
  }

  egress {
    description = "apt, GitHub, S3, and Postgres on the web node"
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  lifecycle {
    create_before_destroy = true
  }
}

resource "aws_security_group" "web" {
  name_prefix = "me-web-"
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
  iam_instance_profile        = var.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
  }

  tags = { Name = "me-web", Role = "web" }
}

resource "aws_instance" "pool" {
  ami                         = var.ami_id
  instance_type               = var.pool_instance_type
  subnet_id                   = var.subnet_id
  key_name                    = aws_key_pair.this.key_name
  vpc_security_group_ids      = [aws_security_group.worker.id]
  iam_instance_profile        = var.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
  }

  tags = { Name = "me-pool", Role = "pool" }
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
  iam_instance_profile        = var.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true
  tenancy                     = var.bench_dedicated ? "dedicated" : "default"

  cpu_options {
    core_count       = var.bench_core_count
    threads_per_core = 1
  }

  root_block_device {
    volume_size = var.root_volume_gb
    volume_type = "gp3"
  }

  tags = { Name = "me-bench", Role = "bench" }
}
