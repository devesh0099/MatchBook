# Bootstrap — the things the ACCOUNT OWNER creates, once.
#
# This directory exists to be handed over. In a company account you will not
# have IAM write, so these three resources are what you ask their cloud team
# for; `deployer-policy.json` is the exact policy to attach to whatever they
# give you. Running it here, in an account where you DO have admin, is how you
# find out the ask is correct before you send it.
#
#   terraform -chdir=infra/bootstrap init
#   terraform -chdir=infra/bootstrap apply
#
# Everything else lives in ../ and runs as the deployer role this creates, so
# a missing permission surfaces as an AccessDenied naming the action rather
# than as a surprise on the day.

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
}

data "aws_caller_identity" "current" {}

locals {
  bucket = coalesce(var.bucket_name, "me-platform-artifacts-${data.aws_caller_identity.current.account_id}")
}

# ----------------------------------------------------------------- artifacts
# Source, compiled binaries and histograms, keyed on sha256. Nothing in the
# platform creates this bucket; without it every /run and /submit returns an
# opaque "internal error", because the API stores source before it does
# anything else.

resource "aws_s3_bucket" "artifacts" {
  bucket        = local.bucket
  force_destroy = var.force_destroy_bucket

  tags = { Project = "me-platform" }
}

resource "aws_s3_bucket_public_access_block" "artifacts" {
  bucket                  = aws_s3_bucket.artifacts.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "artifacts" {
  bucket = aws_s3_bucket.artifacts.id
  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

# -------------------------------------------------------------- node role
# Attached to all three instances. This is the ONLY AWS access the platform
# itself has: no EC2, no Secrets Manager, no CloudWatch. If you are asking a
# cloud team for one thing, it is this.

resource "aws_iam_role" "node" {
  name = var.node_role_name

  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { Service = "ec2.amazonaws.com" }
      Action    = "sts:AssumeRole"
    }]
  })

  tags = { Project = "me-platform" }
}

resource "aws_iam_role_policy" "node" {
  name = "${var.node_role_name}-s3"
  role = aws_iam_role.node.id

  policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect   = "Allow"
      Action   = ["s3:GetObject", "s3:PutObject", "s3:ListBucket"]
      Resource = [aws_s3_bucket.artifacts.arn, "${aws_s3_bucket.artifacts.arn}/*"]
    }]
  })
}

# The instance profile is the thing EC2 actually accepts; the role alone is not
# attachable. Same name as the role by convention, and it is the name that goes
# in `iam_instance_profile` on the instances.
resource "aws_iam_instance_profile" "node" {
  name = var.node_role_name
  role = aws_iam_role.node.name
}

# ---------------------------------------------------------- deployer role
# What YOU hold. Deliberately not admin: this is the permission set we intend
# to ask for, and ../ runs under it so that any gap fails loudly here rather
# than in someone else's account.
#
# In a company account they create this (or an equivalent user) and hand you
# the ARN; the policy document is identical either way.

resource "aws_iam_role" "deployer" {
  name = var.deployer_role_name

  # Trusted by whoever ran bootstrap. In a company account this becomes their
  # SSO principal or your IAM user's ARN.
  assume_role_policy = jsonencode({
    Version = "2012-10-17"
    Statement = [{
      Effect    = "Allow"
      Principal = { AWS = data.aws_caller_identity.current.arn }
      Action    = "sts:AssumeRole"
    }]
  })

  max_session_duration = 3600
  tags                 = { Project = "me-platform" }
}

resource "aws_iam_role_policy" "deployer" {
  name = "${var.deployer_role_name}-policy"
  role = aws_iam_role.deployer.id

  # Kept in its own file, templated, so it can be sent verbatim to a cloud
  # team without them needing to read Terraform.
  policy = templatefile("${path.module}/deployer-policy.json.tftpl", {
    region     = var.aws_region
    account_id = data.aws_caller_identity.current.account_id
    bucket     = local.bucket
    node_role  = var.node_role_name
  })
}
