variable "aws_region" {
  type    = string
  default = "us-east-1"
}

variable "aws_profile" {
  description = "Local profile used to ASSUME deployer_role_arn. It needs sts:AssumeRole and nothing else; all the real work happens under the assumed role."
  type        = string
  default     = "iicpc"
}

variable "deployer_role_arn" {
  description = "Role for Terraform to assume. Leave null when the configured profile already carries the deployment policy. Set it to bootstrap's deployer_role_arn output to run under that role instead."
  type        = string
  default     = null
}

variable "instance_profile" {
  description = "Instance profile attached to all three nodes. From bootstrap."
  type        = string
  default     = "me-platform-node"
}

variable "s3_bucket" {
  description = "Artifact bucket. Not used by Terraform — it goes into worker.env and the compose environment during node setup. Declared here so one tfvars file carries every value the deployment needs."
  type        = string
}

# ------------------------------------------------------------------ network

variable "vpc_id" {
  type = string
}

variable "subnet_id" {
  description = "Must have a route to the internet: the setup scripts pull apt packages, the Rust toolchain and GitHub."
  type        = string
}

variable "ssh_cidrs" {
  description = <<-EOT
    Who may reach SSH. Open by default, because an IP allowlist is unworkable
    against campus and cafe wifi that rotates: a stale /32 locks you out of a
    node that is otherwise healthy, at the worst possible time.

    What protects the port is key-only authentication, not the firewall.
    user_data disables password and keyboard-interactive auth and root login
    explicitly rather than trusting the AMI to keep defaulting that way, so the
    only way in is the private half of the imported key.

    Narrow it if you ever have a stable address; the mechanism stays correct
    either way.
  EOT
  type        = list(string)
  default     = ["0.0.0.0/0"]
}

variable "web_ingress_cidrs" {
  description = "Who may reach Caddy. Also open — the frontend is the thing participants use, and you will be testing it from the same rotating addresses. For the event with option A (Caddy terminates TLS), port 80 MUST be open to 0.0.0.0/0 or Let's Encrypt cannot complete the HTTP-01 challenge, and issuance fails with a challenge error rather than a firewall one."
  type        = list(string)
  default     = ["0.0.0.0/0"]
}

# ----------------------------------------------------------------- instances

variable "ami_id" {
  description = "Ubuntu 24.04 LTS, x86_64. Everything is compiled -march=x86-64-v3, so arm64 is not an option."
  type        = string
}

variable "key_name" {
  type    = string
  default = "me-platform"
}

variable "public_key_path" {
  description = "Public half of a key you already hold. Verify it matches before launching — a mismatched key pair locks you out of instances that look healthy."
  type        = string
  default     = "~/.ssh/id_ed25519.pub"
}

variable "web_instance_type" {
  description = "Caddy, Next.js, axum, Postgres and Redis for 18 people polling every 2s. Measured idle footprint of the whole stack is ~110 MiB, so this is sized for the kickoff burst — 24 MB of vendored Monaco times 18 — and for building the images on the node, not for steady state."
  type        = string
  default     = "m6i.large"
}

variable "pool_instance_type" {
  description = "Correctness lane. Concurrency is min(nproc - 2, POOL_BOXES=8), so 8 vCPU gives 6 parallel jobs. A Run is ~2s of compile, so that is far more than 18 people can saturate."
  type        = string
  default     = "c6i.2xlarge"
}

variable "bench_instance_type" {
  description = "One job at a time. The cores are for isolating the runner, not for speed. The size that matters is L3, because the ranked depth is calibrated against it — do not change this without re-running the depth sweep."
  type        = string
  default     = "c6i.2xlarge"
}

variable "bench_core_count" {
  description = "Physical cores for the bench node, with SMT off. c6i.2xlarge has 4. Must match the instance type or the launch fails."
  type        = number
  default     = 4
}

variable "bench_dedicated" {
  description = "Dedicated tenancy for the bench node. Off by default: it adds a flat $2/hr per region, which is ~59x the per-instance premium and roughly triples the bill. Turn it on only if the noise floor says so. Cannot be changed after launch."
  type        = bool
  default     = false
}

variable "root_volume_gb" {
  description = "8 GB (the AMI default) is not enough: the Rust toolchain, a release build, Docker images and Postgres all land here."
  type        = number
  default     = 40
}

# --------------------------------------------------------------------- code

variable "repo_url" {
  type    = string
  default = "https://github.com/agrawalx/MatchBook.git"
}

variable "repo_ref" {
  description = "Pin a SHA rather than a branch, so 'what did we deploy' has an answer."
  type        = string
  default     = "master"
}
