# tig-agent

`tig-agent` records a Linux machine's timeline into a [TiG](https://github.com/jchown/tig-docs) repository and pushes it to an
ordinary Git remote. It is the collector described in the TiG roadmap (Phase 1, desktop and server prototype), built on
[`tig-lib`](https://github.com/jchown/tig-lib).

This is an MVP. It deliberately knows nothing about hosting: it keeps a local repository, and pushes it with the system's `git`
using its own deploy key. Anything that accepts a fast-forward push works as a remote.

## Design

```
tig-agentd (systemd service, user "tig")

  every 60s   sample ──► tig_telemetry_append  ──► working tree of /var/lib/tig/repository
  every 15m   commit ──► tig_repository_commit ──► local commits
  every 1h    push   ──► git push origin HEAD:main, fast-forward only ──► any Git remote
```

**One binary.** `tig-agent` is a C99 program that links `libtig` statically, with a pinned libgit2 built in, so the only
runtime library it needs is libc. `tig-agent daemon` is the service; the other commands (`init`, `collect`, `commit`, `push`,
`status`) do one step by hand and are what the daemon calls internally.

**Three cadences.** Following [`TELEMETRY.md`](https://github.com/jchown/tig-docs/blob/main/TELEMETRY.md), sampling,
committing and pushing run at different rates. Readings are written to the working tree as soon as they are sampled, so a crash
loses nothing; they are committed in batches; and commits are pushed less often still. All three intervals are configurable.

**Writing goes through libtig, pushing goes through git.** libtig owns the repository layout, file formats and the
append-only rule. Pushing uses the `git` command rather than libgit2, so libgit2 needs no network or SSH support, and SSH
behaves exactly as it does anywhere else on the machine.

**Fast-forward only, never force.** Each repository has a single writer: the machine it describes. If a push is rejected,
something else has written to the remote (a clone, another machine, a person), so the agent logs it, records it in its status
and pauses pushing until someone looks. It never merges or force-pushes, because rewriting history would defeat the point of
TiG. `tig-agent push` retries by hand once it is resolved. Ordinary failures, such as being offline, are retried at the next
interval and commits simply wait locally.

**Its own deploy key.** On first run the agent creates an Ed25519 key in `/var/lib/tig/keys/` (mode 0700) and pushes with
`IdentitiesOnly`, `BatchMode` and its own `known_hosts`, so it never prompts, never uses anyone else's keys and pins the remote's
host key on first use. The key is scoped to one remote, e.g. as a GitHub deploy key on one repository. It is deliberately
separate from the device key, so the device's identity doesn't depend on access to any particular host.

**Every commit is signed by the device key.** The agent also creates `keys/device_ed25519`, the device's permanent,
pseudonymous identity (see [`COMMITS.md`](https://github.com/jchown/tig-docs/blob/main/COMMITS.md), "Device keys"). The
repository's first commit lists it in `.tig/allowed_signers`, and the agent signs each commit through `ssh-keygen -Y sign -n
git`, so anyone with a clone can run `git -c gpg.ssh.allowedSignersFile=.tig/allowed_signers log --show-signature`. A
repository made by an earlier, unsigned agent gains the file in a signed "Add device key" commit at the next commit; the history
before it stays unsigned. Losing the device key means starting a new repository, because the agent won't sign with a key the
file doesn't list.

**The config file is the source of truth.** `/etc/tig-agent/agent.conf` names the remote, and the agent makes the repository's
`origin` match it at start-up and before each push. Removing the remote keeps the repository local.

**Serialised.** The daemon and manual commands take a lock (`/var/lib/tig/agent.lock`) around each step, so running
`tig-agent collect` by hand while the service is running is safe.

**Sandboxed.** The service runs as the unprivileged `tig` user with `ProtectSystem=strict`, `ProtectHome`, `NoNewPrivileges`
and related settings. Everything it reads is world-readable, and it can only write to `/var/lib/tig`.

### What it records

| Stream | Columns | Source |
| --- | --- | --- |
| `system.load` | `load1`, `load5`, `load15` | `getloadavg()` |
| `system.uptime` | `value` (seconds) | `/proc/uptime` |
| `memory` | `total`, `available` (bytes) | `/proc/meminfo` |
| `disk.root` | `size`, `used`, `available` (bytes) | `statvfs("/")` |

All streams use daily chunks and a 5 minute `max_lag`. A stream is only defined once it has its first reading, so a platform
that can't provide one (e.g. macOS during development) leaves nothing behind.

### On disk

```
/etc/tig-agent/agent.conf        configuration (a dpkg conffile, so upgrades keep your edits)
/etc/tig-agent/update.env        GITHUB_TOKEN for the updater (you create this)
/var/lib/tig/repository/         the TiG repository
/var/lib/tig/keys/               device key, deploy key and pinned known_hosts
/var/lib/tig/agent.status        last sample, commit and push, and the last push result
/var/cache/tig-agent/            the installed .deb, kept for rollback
/var/lib/tig-agent-update/       versions that failed their health check
```

The repository is the machine's history, so it is kept even when the package is purged.

## Deployment

Every merge to `main` is built, tested, packaged and released, and the server installs it within a few minutes. The server is
never contacted from outside, so it can sit behind NAT, and CI holds no credentials for it.

```
push to main ──► GitHub Actions ──► build + ctest ──► .deb ──► sign SHA256SUMS ──► GitHub release "build-N"
                                                                                       │
server: tig-agent-update.timer (every 2 min) ◄── poll latest release ◄─────────────────┘
          │
          ├─ newer than installed and not marked bad? download .deb, SHA256SUMS, SHA256SUMS.sig
          ├─ verify the signature with the release key shipped in the installed package, then the .deb's hash
          ├─ dpkg -i (postinst restarts tig-agentd)
          ├─ health check: after 30s, tig-agentd is active and `tig-agent status` succeeds
          └─ on failure: mark the version bad, reinstall the cached previous .deb
```

**Build** (`.github/workflows/build.yml`). Runs in an `ubuntu:22.04` container on every push and pull request, so the package
runs on Ubuntu 22.04+ and Debian 12+. The private `tig-lib` submodule is fetched with a read-only deploy key
(`TIG_LIB_DEPLOY_KEY` secret). The package version is `0.1.0+<run number>.<commit>`, which `dpkg` orders correctly.

**Release.** On `main` only. `SHA256SUMS` is signed with `ssh-keygen -Y sign` using the release key (`RELEASE_SIGNING_KEY`
secret). Its public half is [`packaging/release-signers`](packaging/release-signers) and is installed with the package, so each
release is verified with the key from the one before it. To rotate the key, ship a release that contains the new public key,
signed with the old one, then change the secret.

**Updater** ([`packaging/tig-agent-update`](packaging/tig-agent-update)). A small bash script, run as root by a systemd timer,
and shipped in the package so it updates itself. It needs `curl` and `jq`, both declared as dependencies.

**The .deb** declares its dependencies (`git`, `openssh-client`, `curl`, `jq`, `adduser`), creates the `tig` user, and enables
`tig-agentd.service` and `tig-agent-update.timer`.

## Setting up a server

This is needed once. After it, updates are automatic.

1. **Install the latest release.** From a machine with `gh`:

   ```sh
   gh release download --repo jchown/tig-agent --pattern '*.deb'
   scp tig-agent_*.deb server:
   ssh server sudo apt install ./tig-agent_*.deb
   ```

   The agent starts at once and records locally.

2. **Let the updater read releases.** Create a
   [fine-grained token](https://github.com/settings/personal-access-tokens/new) with access to `jchown/tig-agent` only and
   *Contents: read-only*. Then on the server:

   ```sh
   sudo install -m 600 /dev/null /etc/tig-agent/update.env
   echo 'GITHUB_TOKEN=github_pat_...' | sudo tee /etc/tig-agent/update.env >/dev/null
   ```

   Fine-grained tokens expire, at most a year after they're created. When one does, the updater logs an error and keeps the
   installed version running.

3. **Give the agent somewhere to push.** Show the deploy key and add it to the data repository with write access:

   ```sh
   ssh server sudo -u tig tig-agent status | awk '/^deploy key/ {print $3, $4, $5}' >deploy_key.pub
   gh repo deploy-key add deploy_key.pub --repo jchown/tig-homeserver --allow-write --title "tig-agent@server"
   ```

   Then set `remote = git@github.com:jchown/tig-homeserver.git` in `/etc/tig-agent/agent.conf` and run
   `sudo systemctl restart tig-agentd`. The agent pushes immediately on start.

4. **Check it.**

   ```sh
   sudo -u tig tig-agent status
   journalctl -u tig-agentd -f
   systemctl list-timers tig-agent-update.timer
   journalctl -u tig-agent-update
   ```

The first update after a manual install can't be rolled back, because no earlier package is cached yet. From then on each
update keeps the previous package.

## Configuration

`/etc/tig-agent/agent.conf`, or a file given with `--config` or `$TIG_AGENT_CONFIG`:

| Setting | Default | Meaning |
| --- | --- | --- |
| `data_dir` | `/var/lib/tig` | Where the repository, keys and status are kept. Must be absolute. |
| `remote` | *(empty)* | Git remote to push to. Empty keeps the repository local. |
| `sample_interval` | `60s` | How often telemetry is sampled. |
| `commit_interval` | `15m` | How often readings are committed. |
| `push_interval` | `1h` | How often commits are pushed (each push commits first). |

Durations are a positive integer followed by `s`, `m`, `h` or `d`. Unknown settings are errors, so typos don't go unnoticed.

`tig-agent push` exits with status 3 when the remote rejects the push, 1 on other errors and 2 for usage errors.

## Development

```sh
git clone --recurse-submodules git@github.com:jchown/tig-agent.git
cd tig-agent
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

It builds and its tests pass on macOS too, though only `system.load` and `disk.root` are available there. To build the
package as CI does, run `cd build && cpack` on Linux.

The tests are a unit test for configuration parsing and an end-to-end script that drives the real binary against a local bare
repository: init, collect, push, a second fast-forward push, and a remote that has diverged, which must be refused and left
untouched.

`tig-lib` is a submodule in `external/tig-lib`. To pick up library changes, commit them there, then update the submodule here
in the same pull request as the agent changes that need them.

```
src/
  main.c         command line
  daemon.c       the service loop and its three cadences
  actions.c      collect, commit and sync, each under the agent lock and recorded in the status file
  telemetry.c    the streams and their samplers
  sync.c         origin and `git push`
  repository.c   first-run setup, deploy key, lock
  device.c       device key and commit signing through ssh-keygen
  status.c       status file and `tig-agent status`
  config.c       configuration file
  process.c      running git and ssh-keygen with a timeout
  log.c          stderr logging with journald priorities
packaging/       agent.conf, systemd units, maintainer scripts, updater and release key
tests/           config_test.c and end_to_end.sh
```

## Not yet implemented

These are needed before the agent follows the TiG spec in full. Most of the work is in `tig-lib`.

- **State and events.** `tig_repository_commit` only stages `/telemetry`, so the agent can't yet record its initial state
  snapshot (CPU, memory, disks, OS, packages) or events (package installed, OS upgraded, disk replaced).
- **TiG Notary.** No signature tags yet, so the repository proves which device wrote it but not when.
  When it arrives, the push must include `refs/tags/tig/sig/*`.
- **TiG clock.** Readings use the system clock, without the notary offset.
