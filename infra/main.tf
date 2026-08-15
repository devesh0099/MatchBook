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

# The worker group carries NO inline ingress or egress blocks, and that is
# deliberate. Restricted egress needs a rule whose destination is the WEB group
# (Postgres), but the web group's own inline ingress already references this
# one — and Terraform builds its dependency graph from the configuration, not
# from runtime values, so an inline rule pointing back at web makes the two
# resources depend on each other. The result is
#
#   Error: Cycle: aws_security_group.web, aws_security_group.worker
#
# and it fires even with restrict_worker_egress = false, because the reference
# is present in the file whether or not the dynamic block produces anything.
# Nothing can be planned or applied at all.
#
# A standalone rule is its own node in that graph: it depends on both groups,
# and neither group depends on it. Inline blocks are authoritative over the
# whole rule set, so they cannot be mixed with standalone rules on the SAME
# group — hence SSH moves out here too. The web group keeps its inline blocks,
# because nothing points back at it from here.
resource "aws_security_group" "worker" {
  name_prefix = "${var.name_prefix}-worker-"
  description = "Pool and bench nodes. No inbound except administrative SSH."
  vpc_id      = var.vpc_id

  lifecycle {
    create_before_destroy = true
  }
}

# SSH to the workers comes from the WEB GROUP, never from the internet.
#
# These two nodes compile and execute participant-submitted C++, and nothing
# outside the VPC has any reason to reach them. They still hold a public IP,
# because the only egress path in this VPC is the internet gateway and an IGW
# requires one — without it cloud-init cannot reach apt, rustup or GitHub, and
# the worker cannot reach S3 to fetch submission source. A NAT gateway would
# remove that need at about $40/month.
#
# Inbound and outbound are separate concerns, which is what makes this work: a
# security group is default-deny inbound, so with no CIDR rule here nothing on
# the internet can open a connection — packets are dropped, not refused. And
# security groups are STATEFUL, so replies to connections the node itself opens
# come back regardless. The public IP becomes outbound-only.
#
# Operator access is therefore a jump through the web node; ops/aws/lib.sh does
# this automatically.
resource "aws_vpc_security_group_ingress_rule" "worker_ssh_from_web" {
  security_group_id = aws_security_group.worker.id
  # ASCII only, and no em dash. AWS validates rule descriptions against
  # a-zA-Z0-9._-:/()#,@[]+=&;{}!$* and rejects anything else with
  # InvalidParameterValue, which reads as a malformed request rather than as a
  # stray character. Comments elsewhere in this file use em dashes freely; the
  # description fields cannot.
  description                  = "SSH from the web node only; operators jump through it"
  referenced_security_group_id = aws_security_group.web.id
  from_port                    = 22
  to_port                      = 22
  ip_protocol                  = "tcp"
}

# Deliberately empty by default. This is the break-glass path for the case the
# jump host is the thing that is broken: set it to your own /32, apply, fix the
# web node, then set it back. Editing main.tf under time pressure is how
# mistakes get made, so the lever exists ahead of time.
resource "aws_vpc_security_group_ingress_rule" "worker_ssh_direct" {
  for_each = toset(var.worker_ssh_cidrs)

  security_group_id = aws_security_group.worker.id
  description       = "SSH, direct break-glass access"
  cidr_ipv4         = each.value
  from_port         = 22
  to_port           = 22
  ip_protocol       = "tcp"
}

# Wide open, unless restrict_worker_egress says otherwise.
resource "aws_vpc_security_group_egress_rule" "worker_all" {
  count = var.restrict_worker_egress ? 0 : 1

  security_group_id = aws_security_group.worker.id
  description       = "apt, GitHub, S3, and Postgres on the web node"
  cidr_ipv4         = "0.0.0.0/0"
  ip_protocol       = "-1"
}

# The narrowed set. Postgres is excluded here and handled below, because its
# destination is a security group rather than a CIDR.
resource "aws_vpc_security_group_egress_rule" "worker_port" {
  for_each = var.restrict_worker_egress ? {
    for k, v in local.worker_egress : k => v if k != "postgres"
  } : {}

  security_group_id = aws_security_group.worker.id
  description       = each.value.description
  cidr_ipv4         = "0.0.0.0/0"
  from_port         = each.value.port
  to_port           = each.value.port
  ip_protocol       = each.value.protocol
}

# Postgres egress aimed at the web group, so it keeps working when the web node
# is replaced and takes a new private IP. This is the rule that cannot be
# expressed inline; see the comment on aws_security_group.worker.
resource "aws_vpc_security_group_egress_rule" "worker_postgres" {
  count = var.restrict_worker_egress ? 1 : 0

  security_group_id            = aws_security_group.worker.id
  description                  = local.worker_egress.postgres.description
  referenced_security_group_id = aws_security_group.web.id
  from_port                    = local.worker_egress.postgres.port
  to_port                      = local.worker_egress.postgres.port
  ip_protocol                  = local.worker_egress.postgres.protocol
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

# The fleet: one box per participant (PLAN-measurement-redesign §7). Every box
# measures, so every box gets the settings the old bench node had — and two of
# them CANNOT be changed after launch:
#
#   * threads_per_core = 1 — SMT off from boot. A sibling thread sharing the
#     measurement core is exactly the contamination the design refuses. With
#     SMT off a c6i.2xlarge presents 4 whole cores: 0 for the OS and agent,
#     1 isolated for measurement, 2-3 for compiles.
#
#   * tenancy stays default for the fleet — the measured noise floor says
#     shared tenancy is fine for pass/fail rungs, and the golden box is where
#     dedicated tenancy buys its one decisive hour (M6).
resource "aws_instance" "agent" {
  count = var.fleet_size

  ami                         = var.ami_id
  instance_type               = var.agent_instance_type
  subnet_id                   = var.subnet_id
  key_name                    = aws_key_pair.this.key_name
  vpc_security_group_ids      = [aws_security_group.worker.id]
  iam_instance_profile        = local.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true
  associate_public_ip_address = var.associate_public_ip

  cpu_options {
    core_count       = var.agent_core_count
    threads_per_core = 1
  }

  # These boxes compile and run participant-submitted C++. The agent is a
  # plain systemd process, not a container, so one hop is enough — and
  # http_tokens = "required" turns IMDSv1 off: with it optional, any SSRF or
  # isolate escape reads the instance credentials with a single
  # unauthenticated GET.
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

  # 1-based: agent-1 serves participant 1, which is how deploy.sh writes the
  # binding into each box's worker.env.
  tags = { Name = "${var.name_prefix}-agent-${count.index + 1}", Role = "agent" }
}

# The golden box (M6): identical layout, dedicated tenancy for the one
# decisive hour. Count 0 until the rejudge lands — launched for the event's
# final phase, not left running all day.
resource "aws_instance" "golden" {
  count = var.golden_count

  ami                         = var.ami_id
  instance_type               = var.agent_instance_type
  subnet_id                   = var.subnet_id
  key_name                    = aws_key_pair.this.key_name
  vpc_security_group_ids      = [aws_security_group.worker.id]
  iam_instance_profile        = local.instance_profile
  user_data                   = local.user_data
  user_data_replace_on_change = true
  associate_public_ip_address = var.associate_public_ip
  tenancy                     = var.golden_dedicated ? "dedicated" : "default"

  cpu_options {
    core_count       = var.agent_core_count
    threads_per_core = 1
  }

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

  tags = { Name = "${var.name_prefix}-golden", Role = "golden" }
}
