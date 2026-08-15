variable "aws_region" {
  type    = string
  default = "us-east-1"
}

variable "aws_profile" {
  description = "Local profile used to ASSUME deployer_role_arn. It needs sts:AssumeRole and nothing else; all the real work happens under the assumed role. Null means the ordinary AWS credential chain — naming a profile that does not exist on the machine is a hard failure, so it is not a good default."
  type        = string
  default     = null
}

variable "deployer_role_arn" {
  description = "Role for Terraform to assume. Leave null when the configured profile already carries the deployment policy. Set it to bootstrap's deployer_role_arn output to run under that role instead."
  type        = string
  default     = null
}

variable "name_prefix" {
  description = <<-EOT
    Prefix on every AWS resource and the value of the Project tag. MUST match
    the name_prefix bootstrap was applied with: the deployer policy scopes its
    destructive actions with a condition on `Project = <name_prefix>`, so a
    mismatch fails at the first tagged write with AccessDenied rather than with
    anything mentioning names.

    Does not rename `mebench` — the Postgres user, the systemd units,
    /opt/mebench and the MEBENCH_* variables are the application's own identity
    and are invisible in the AWS console.
  EOT
  type        = string
  default     = "flashmatch"

  validation {
    condition     = can(regex("^[a-z][a-z0-9-]{1,20}$", var.name_prefix))
    error_message = "name_prefix must be lowercase letters, digits and hyphens, starting with a letter."
  }
}

variable "instance_profile" {
  description = "Instance profile attached to all three nodes. From bootstrap; defaults to <name_prefix>-node."
  type        = string
  default     = null
}

variable "s3_bucket" {
  description = "Artifact bucket. Not used by Terraform — it goes into worker.env and the compose environment during node setup. Declared here so one tfvars file carries every value the deployment needs."
  type        = string
}

# ------------------------------------------------------------------ network

variable "vpc_id" {
  type = string

  validation {
    condition     = can(regex("^vpc-[0-9a-f]{8,17}$", var.vpc_id))
    error_message = "vpc_id must look like vpc-0123456789abcdef0."
  }
}

variable "subnet_id" {
  description = "Must have a route to the internet: the setup scripts pull apt packages, the Rust toolchain and GitHub."
  type        = string

  validation {
    condition     = can(regex("^subnet-[0-9a-f]{8,17}$", var.subnet_id))
    error_message = "subnet_id must look like subnet-0123456789abcdef0."
  }
}

variable "associate_public_ip" {
  description = <<-EOT
    Give each node a public IP, explicitly rather than inheriting the subnet's
    map_public_ip_on_launch.

    Left to the subnet, a private subnet behind NAT satisfies everything
    DEPLOYMENT.md asks for ("a route to the internet") and still produces
    instances with no public address. Nothing errors: aws_instance.public_ip is
    an empty string, apply reports success, and the outputs print
    `ssh -i ~/.ssh/flashmatch ubuntu@` — so the readiness poll spins forever
    against nodes that are perfectly healthy.

    True is what the documented deployment assumes, because provisioning and
    operator access are over SSH from a laptop. Set it false only if you reach
    the nodes another way (VPN, bastion, SSM), in which case ops/aws/*.sh will
    tell you they cannot find a public IP rather than failing at the ssh call.
  EOT
  type        = bool
  default     = true
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

variable "worker_ssh_cidrs" {
  description = <<-EOT
    Break-glass direct SSH to the pool and bench nodes. Empty by default, which
    is the point: those two nodes accept SSH only from the web security group,
    so operators reach them by jumping through the web node and nothing on the
    internet can open a connection to them at all.

    Set this to your own /32 only when the jump host itself is what is broken.
    ssh_cidrs still governs the web node, which must stay reachable.
  EOT
  type        = list(string)
  default     = []
}

variable "restrict_worker_egress" {
  description = <<-EOT
    Narrow the worker nodes' outbound rules to the ports they actually use
    (HTTP/HTTPS for apt, rustup and S3; 5432 to the web node; DNS and NTP)
    instead of allowing everything.

    These two nodes compile and execute participant-submitted C++. isolate
    denies the sandbox a network namespace, so this is defence in depth against
    an escape rather than the primary control — but wide-open egress is also the
    exfiltration and command-and-control path an escape would use.

    Default false because it changes network behaviour that the current
    deployment has been tested with, and a missing port here fails at apt or
    rustup during provisioning rather than at plan time. Turn it on, run a full
    ops/aws/deploy.sh, and leave it on if provisioning succeeds.
  EOT
  type        = bool
  default     = false
}

# ----------------------------------------------------------------- instances

variable "ami_id" {
  description = "Ubuntu 24.04 LTS, x86_64. Everything is compiled -march=x86-64-v3, so arm64 is not an option."
  type        = string
}

variable "key_name" {
  description = "Base name for the imported EC2 key pair; a unique suffix is appended. Defaults to <name_prefix>."
  type        = string
  default     = null
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

variable "agent_ami_id" {
  description = "The baked fleet AMI from ops/aws/bake-ami.sh. Null (the default) launches agents from the stock Ubuntu ami_id and provisions from source (~25 min/box); set, agents boot fully provisioned in minutes — kernel isolation, toolchain, baked streams and all — and only the connect phase (worker.env binding) remains."
  type        = string
  default     = null
}

variable "fleet_size" {
  description = "One box per participant (PLAN-measurement-redesign §7). agent-N serves participant N. ~$0.34/hr each in ap-south-1; a 20-box fleet over an 8h event is ~$55 of compute."
  type        = number
  default     = 2
}

variable "agent_instance_type" {
  description = "Every box measures, so every box is the calibrated type. c6i.xlarge with SMT off presents 2 whole cores — the design's minimum: OS, agent and compiles share core 0; core 1 is the isolated measurement core. Do not change without re-running M5: the deadlines are calibrated against this type's effective cache."
  type        = string
  default     = "c6i.xlarge"
}

variable "agent_core_count" {
  description = "Physical cores with SMT off. c6i.xlarge has 2. Must match the instance type or the launch fails."
  type        = number
  default     = 2
}

variable "golden_count" {
  description = "The golden box for the Phase III rejudge (M6). 0 until the event's final phase — it is launched for its one decisive hour, not left running all day."
  type        = number
  default     = 0
}

variable "golden_dedicated" {
  description = "Dedicated tenancy for the golden box: the flat regional fee that was too expensive fleet-wide is nothing for one box for one hour, and it removes co-tenancy from the only numbers that are final. Cannot be changed after launch."
  type        = bool
  default     = true
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
