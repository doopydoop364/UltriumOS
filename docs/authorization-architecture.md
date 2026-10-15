# Ultrium authorization architecture — USH 0.5

**Status: design only. `auth` and `as` are unavailable.**

Ultrium currently runs its shell and filesystem inside a single trusted kernel address space. It has no isolated userspace processes, enforced multiuser permission model or trustworthy authentication service. Kernel code can modify all shell state and forge current handle values. A shell password prompt or identity variable cannot establish a security boundary. This milestone implements no privilege-changing command, credential prompt, administrator mode or fake sudo equivalent.

## Concepts and CLI grammar

`auth` requests a temporary, action-specific authorization grant for the current identity. It does not switch identities. `as` executes one command under another already-authorized identity and returns to the original execution context afterward; it never permanently changes the shell identity.

Planned grammar:

```text
auth <command path> [arguments...]
as <identity> <command path> [arguments...]

auth remove protected.txt
as admin ultrium system configure
as guest view public.txt
as builder new folder projects
```

These are examples of future grammar, not commands that exist today. `ultrium system configure` and item permissions are also future work. `auth help` / `as help` will follow USH help conventions only when those namespaces are implemented. Bare `auth`/`as`, missing identity/command, unknown identity or nested privilege wrappers are errors. No implicit shell reparsing, substitutions, pipelines, interactive identity switching or blanket session administrator grant is implied.

The lexer produces bounded arguments. A future command planner resolves registry handler identifiers and operation requirements into a typed, immutable request. Authorization applies to that exact operation and argument vector. Execution never concatenates a string and recursively calls shell dispatch after approval. Literal paths and canonical objects must be distinguished in the plan. Dynamic user/plugin commands must declare requirements and return structured status before they can participate.

## Identities, roles and ownership

An identity is a kernel-managed principal with an opaque stable identifier, generation, enabled/revoked state and credential-policy reference. Display names are labels, never credentials. Initial identifiers are local to a boot epoch until persistent identity storage exists. A registry prevents reusing an identifier in a way that revives stale grants.

Illustrative roles (`guest`, `builder`, `admin`) are policy bundles of possible rights, not executable kernel modes. Roles are assigned through a trusted policy boundary. An `admin` label alone grants nothing; requesting identity impersonation and requesting an action's capabilities are distinct policy decisions. Deny is the default. Read, enumerate, create, modify, rename and remove are separate rights; administration does not imply unrestricted access to every object.

Filesystem ownership/ACLs require actual stored owner identifiers and explicit enforcement. Existing RAMFS type/length metadata cannot stand in for them. Future storage must distinguish owner, group/role policy and capabilities; choosing an on-disk representation is deferred until persistent storage is designed.

## Kernel-enforced capability model

A capability is an unforgeable, kernel-owned authority reference. The opaque userspace token is validated against a protected table, issuing principal, subject process/agent, generation and boot epoch. Copying token bits is not delegation. Current generation-bearing VFS handles protect stale-handle reuse only and are not this security mechanism.

Every grant records:

- Issuer and authorized subject identity, process and optional agent identifier.
- Exact action rights (for example `file.read`, `folder.enumerate`, `file.create`, `item.rename`, `item.remove`, or a specific system configuration action).
- Object identity or explicit subtree scope, including mount identity, stable object identifier and generation. Path prefixes alone are never authority.
- Operation-specific constraints: maximum bytes/items/depth, permitted destination parent, overwrite policy, permitted mount and whether recursion is allowed.
- Creation/expiry against monotonic time, use count, revocation epoch and any parent grant.
- Whether delegation is allowed, and any narrower delegation policy.
- Request/approval/audit identifiers, with no secret material.

A file copy needs read authority on the source and create/write authority in the destination parent. Rename needs rights on the source and both parents, plus explicit overwrite rights if overwriting is ever introduced. Recursive removal needs a bounded subtree grant plus normal per-object enforcement; `-r` is a traversal choice and `-y` is a UI choice. Neither authorizes a protected operation. Mount crossings require a separate explicit grant and traversal policy; symbolic links must have defined non-follow/follow semantics before support exists.

## Authentication and approval boundaries

Authentication proves a principal through a trusted kernel/service path whose inputs and display cannot be impersonated by an untrusted process or agent. Passwords, hardware factors and recovery policy are future design choices; do not expose a password prompt before the trusted path, secret storage and rate limiting exist.

Authorization evaluates whether an authenticated principal may perform an exact action. Authentication success alone cannot authorize arbitrary capabilities or impersonation. The policy decision point and grant issuer are isolated from requesting applications. Trusted UI must show identity, actor, exact action, canonical target/mount, requested scope, limits and lifetime. Grants may be denied without affecting ordinary unprivileged shell operation.

Interactive `remove ... -r` confirmation is an accident-prevention UI, separate from authorization. `--yes` never suppresses authentication, privilege approval or mandatory policy decisions. An authorization approval does not silently imply consent to unrelated destructive operations.

## Request and execution lifecycle

1. Resolve a registered handler and validate grammar, paths, flags and limits without side effects.
2. Build the immutable typed request with all required rights, source/destination object references and expected namespace generations. Unknown requirements fail closed.
3. Check the current context's capabilities and normal object policy. Request only missing rights. A plain `auth` request with no missing rights may execute normally without issuing a broader grant.
4. Obtain authentication or human approval through a trusted boundary when policy requires it; a noninteractive caller receives a structured pending/denied result, never an invisible prompt.
5. Issue a short-lived, minimum-scope, normally one-use grant bound to the request and subject. For `as`, separately authorize execution as the named identity; reduce to that identity's policy and action-specific rights.
6. Revalidate object/mount/namespace generations and expiry immediately before each operation. If a path now resolves to another object or the plan changed, deny and require a fresh plan. Hold references/locks across validation and commit where needed to close TOCTOU races.
7. Execute under an explicit temporary security context. The VFS, memory, device and system boundaries check authority independently of shell syntax. Never use a mutable global shell identity for this context.
8. Record success, failure or partial completion. Consume/revoke the temporary grant and release all object references and context resources on every exit. Restore the caller's identity even on cancellation or error.

Multi-object operations need an explicit policy for transaction scope. RAMFS has no transactions, so recursive removal can partially succeed. Each step checks scope and revocation; failure reports both completed and remaining work. Do not broaden a grant to recover automatically or retry destructive actions without a new decision.

## Lifetimes, delegation and revocation

Default grants are short-lived and bound to one request, one subject and one boot epoch. Expiry uses a trusted monotonic clock, not user-writable wall time. Suspend/reboot behavior must be defined before supporting persisted grants; current volatile operation would invalidate all grants on reboot. Long-running jobs require explicit bounded renewals through the original policy decision point.

Delegation is denied by default. A permitted child grant can only attenuate the parent's rights, object set, byte/item limits, lifetime and allowed recipients. The kernel checks the complete delegation chain; a child cannot outlive or escape a revoked parent. Role/identity revocation advances an epoch invalidating relevant grants. In-flight work rechecks at bounded operation boundaries and stops conservatively; completed destructive work cannot be undone by revocation.

Revocation must not rely on an agent honoring a message. Kernel-maintained epochs/tables are authoritative. Future scheduling/SMP requires synchronization, lock ordering, cancellation points and safe reference reclamation before this model can be enforced concurrently.

## Agents and human control

An AI agent is an untrusted subject with a distinct principal/process binding and explicit capability budget. It cannot inherit all interactive-shell privileges, approve its own requests, impersonate the human, forge approval events, invoke the grant issuer directly or delegate to a helper to widen scope. Agent proposals and retrieved text are data, never approval evidence.

The human approval channel belongs to a separately authenticated trusted component. It displays the exact immutable operation, requested scope and lifetime, and binds consent to the request identifier. Approval from another agent is insufficient. Agents cannot edit policy, identity assignments, audit records or their own budgets. If the trusted human channel is absent, required approval is denied/pending and privileged execution does not proceed.

Approval should not expose secrets in model context. Policy and capabilities operate without an AI service; ordinary CLI administration must remain complete without AI or networking.

## Auditing and failure behavior

Audit records include monotonic sequence/time, boot epoch, request ID, authenticated actor, delegated subject, agent/process, action, object/mount identifiers, requested/granted rights, scope/lifetime, approver, outcome and denial reason. Redact credentials and secret file contents; path labels may also require redaction. Logs must be inaccessible to untrusted writers and have explicit retention and exhaustion behavior.

Volatile logs cannot provide durable forensic guarantees. Persistent authenticated audit storage is a prerequisite for durable claims. For operations requiring an audit record, inability to reserve audit capacity denies execution before mutation. The policy must specify whether ordinary reads may continue when audit storage is unavailable.

Allocation failure, token exhaustion, stale generation, unsupported scope, malformed request, missing approver, failed authentication, expiry, revocation and unknown policy all fail closed. Cancellation cleans temporary state; it never executes a partially approved command. Report denied/unavailable distinctly from operation failure and partial completion. Do not log or echo authentication secrets in shell history, terminal test capture or COM1 diagnostics.

## Prerequisites and implementation order

Before enabling `auth` or `as`:

1. Add isolated userspace address spaces, user/kernel transitions and validated syscall boundaries; prevent user writes to kernel memory and protected tables.
2. Implement process/task security contexts and object lifetimes, plus scheduling/SMP synchronization when applicable. Shell remains an unprivileged client of enforced APIs.
3. Add stable identities, authentication and a trusted approval input/display path with protected policy/credential storage and failure/rate-limit rules.
4. Add VFS ownership/permissions, stable object/mount identifiers, namespace generations and enforcement at every filesystem entry point. Define link and mount behavior.
5. Implement protected capability issuance/validation, explicit scoped requests, monotonic expiry, delegation attenuation, revocation and structured result/audit APIs.
6. Add atomic plan validation and object-reference/locking rules; test races, cancellation, partial failure, context restoration and grant cleanup.
7. Implement `auth` for one narrowly scoped operation first. Implement `as` only after impersonation policy and per-command context restoration are enforced. Add agent mediation last, through the same checked kernel interfaces.

Acceptance tests must demonstrate unprivileged denial even when bypassing the shell, forged/stale token rejection, subtree/mount escape rejection, reduced delegation, revocation during work, expiry, identity restoration after every failure path, approver/agent separation, protected auditing and secret redaction. Shell-only happy-path tests cannot substantiate privilege isolation.
