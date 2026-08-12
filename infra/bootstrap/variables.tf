variable "aws_region" {
  description = "Region everything lives in. The deployer policy is scoped to it."
  type        = string
  default     = "us-east-1"
}

variable "aws_profile" {
  description = "Local AWS CLI profile with permission to create IAM roles and an S3 bucket. Only bootstrap needs this much; ../ runs as the deployer role."
  type        = string
  default     = "iicpc"
}

variable "name_prefix" {
  description = <<-EOT
    Prefix on every AWS resource this project creates, and the value of the
    Project tag. One place, so the console never shows a half-renamed
    deployment.

    It is also load-bearing beyond cosmetics: the deployer policy scopes its
    destructive actions with a condition on `Project = <name_prefix>`, and
    ops/aws/status.sh finds the instances by that same tag. Changing it here
    changes both.

    Note this does NOT rename `mebench` — the Postgres user and database, the
    systemd units, /opt/mebench and the MEBENCH_* variables. That is the
    application's own identity, baked into the C++ headers and the SQL schema,
    and none of it is visible in the AWS console.
  EOT
  type        = string
  default     = "flashmatch"

  validation {
    condition     = can(regex("^[a-z][a-z0-9-]{1,20}$", var.name_prefix))
    error_message = "name_prefix must be lowercase letters, digits and hyphens, starting with a letter — it becomes part of an S3 bucket name."
  }
}

variable "bucket_name" {
  description = "Artifact bucket. Defaults to <name_prefix>-artifacts-<account-id>, because bucket names are globally unique and a fixed name collides the moment two people try it."
  type        = string
  default     = null
}

variable "force_destroy_bucket" {
  description = "Let `terraform destroy` delete a bucket that still has objects in it. True is right for a throwaway test and wrong for the real event, where the bucket holds every submission's source."
  type        = bool
  default     = true
}

variable "bucket_versioning" {
  description = "Keep previous versions of artifacts. On by default: it is the only undo for an overwritten submission, and note that with versioning enabled force_destroy must also delete every version, which it does."
  type        = bool
  default     = true
}

variable "node_role_name" {
  description = "Role and instance profile attached to all three instances. Defaults to <name_prefix>-node. The deployer's iam:PassRole is scoped to exactly this name, and ../ must pass the same value as instance_profile."
  type        = string
  default     = null
}

variable "deployer_role_name" {
  description = "Role ../ can assume. Carries the deployment permission set and nothing more. Defaults to <name_prefix>-deployer."
  type        = string
  default     = null
}

variable "deployer_trusted_principal" {
  description = "Who may assume the deployer role. Defaults to whoever runs bootstrap, resolved to a durable role or user ARN rather than the STS session ARN. Set explicitly to hand the role to a different principal."
  type        = string
  default     = null
}
