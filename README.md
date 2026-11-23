# tig-agent

`tig-agent` records a Linux machine's timeline into a [TiG](https://github.com/TiG-DB/tig-docs) repository and pushes it to an
ordinary Git remote. It is the collector described in the TiG roadmap (Phase 1, desktop and server prototype), built on
[`tig-lib`](https://github.com/TiG-DB/tig-lib).

This is an MVP. It deliberately knows nothing about hosting: it keeps a local repository, and pushes it with the system's `git`
using its own deploy key. Anything that accepts a fast-forward push works as a remote.

## Design

```
tig-agentd (systemd service, user "tig")

  every 60s   sample  ──► tig_telemetry_append  ──► working tree of /var/lib/tig/repository
  every 15m   compare ──► tig_event_add         ──► working tree (only when something changed)
              commit  ──► tig_repository_commit ──► local commits
  every 1h    sign    ──► TiG Notary signs the latest commit ──► refs/tags/tig/sig/<time>     (if a notary is set)
              push    ──► git push origin HEAD:main, fast-forward only ──► any Git remote
```

**One binary.** `tig-agent` is a C99 program that links `libtig` statically, with a pinned libgit2 built in, so the only
runtime library it needs is libc. `tig-agent daemon` is the service; the other commands (`init`, `collect`, `commit`, `push`,
`status`) do one step by hand and are what the daemon calls internally. `push` commits, has the notary sign (if one is set),
then pushes.

**Three cadences.** Following [`TELEMETRY.md`](https://github.com/TiG-DB/tig-docs/blob/main/TELEMETRY.md), sampling,
committing and pushing run at different rates. Readings are written to the working tree as soon as they are sampled, so a crash
loses nothing; they are committed in batches; and commits are pushed less often still. All three intervals are configurable.

**State is compared, not sampled.** At each commit, and so as soon as it starts, the agent reads the device's components
and compares them with its state, replayed from `state/initial.yaml` and `/events`. Anything different becomes an event, so
the repository says when a disk was replaced or the OS upgraded, not just what the machine looks like now. A boot is recorded
once, the first time the agent runs in it.

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
pseudonymous identity (see [`COMMITS.md`](https://github.com/TiG-DB/tig-docs/blob/main/COMMITS.md), "Device keys"). The
repository's first commit lists it in `.tig/allowed_signers`, and the agent signs each commit through `ssh-keygen -Y sign -n
git`, so anyone with a clone can run `git -c gpg.ssh.allowedSignersFile=.tig/allowed_signers log --show-signature`. A
repository made by an earlier, unsigned agent gains the file in a signed "Add device key" commit at the next commit; the history
before it stays unsigned. Losing the device key means starting a new repository, because the agent won't sign with a key the
file doesn't list.

**Signed by TiG Notary, when asked to.** A device key shows which device wrote a commit, but not when. With `notary` set, each
sync (hourly by default) first asks the notary to sign the latest commit, as [`COMMITS.md`](https://github.com/TiG-DB/tig-docs/blob/main/COMMITS.md)
("Signing") describes: the agent sends the commit's hash, the time and the device's public key, signed with the device key, and
gets back a signed Git tag, which says the commit and all its history existed no later than the tag's time. The notary never
sees the repository. The agent checks the tag before it keeps it (it is for that commit, that device and the configured
`issuer`, named for its own time, and its signature verifies against the keys the issuer publishes at
`https://<issuer>/.well-known/tig-public-key`), stores it as `refs/tags/tig/sig/<time>` and pushes it with the commits. The
published keys are kept in `/var/lib/tig/notary/` and fetched again when a tag doesn't verify against them, e.g. after the
notary changes key. A commit that is already signed isn't signed again.

Signing is best effort. If the notary is down, rate limiting or unreachable, the agent logs it, shows it in `tig-agent status`
and pushes anyway; the next sync signs the latest commit, which covers everything committed meanwhile. If the device's
clock is out by more than the notary allows, the notary says so and the agent asks again with the notary's time. It is off
unless `notary` is set, because the request tells the notary the device's public key and its address, and the commit's hash.
Verifying needs only Git: `git -c gpg.ssh.allowedSignersFile=<published keys> verify-tag <tag>`.

**The config file is the source of truth.** `/etc/tig-agent/agent.conf` names the remote, and the agent makes the repository's
`origin` match it at start-up and before each push. Removing the remote keeps the repository local.

**Serialised.** The daemon and manual commands take a lock (`/var/lib/tig/agent.lock`) around each step, so running
`tig-agent collect` by hand while the service is running is safe.

**Sandboxed.** The service runs as the unprivileged `tig` user with `ProtectSystem=strict`, `ProtectHome=read-only`,
`NoNewPrivileges` and related settings. Everything it reads is world-readable, except the device's serial number, which the
package's install script copies from DMI to `/etc/tig-agent/product_serial` (readable by the `tig` group). It can only write
to `/var/lib/tig`. `/home` is read-only rather than hidden so that a filesystem mounted there can be measured; the agent
never reads files in it.

### What it records

The repository uses the [`tig-device/1`](https://github.com/TiG-DB/tig-docs/blob/main/vocabularies/DEVICE.md) vocabulary.
It describes the device, not its owner: model and serial numbers and MAC addresses are recorded, but IP addresses,
the hostname, network names, user accounts and files are not, since the repository may be shown to a future owner.

**State.** `state/initial.yaml` holds only the entity: the device's vendor, model, serial number, architecture and, for
a virtual machine, its hypervisor. Components are items, added by events:

| Item | Attributes | Linux source |
| --- | --- | --- |
| `firmware` | `vendor`, `version`, `date` | `/sys/class/dmi/id/bios_*` |
| `cpu/<socket>` | `model`, `cores`, `threads` | `/proc/cpuinfo` |
| `memory` | `capacity` | `/sys/devices/system/memory`, else `MemTotal` rounded up to a GiB |
| `disk/<hash>` | `model`, `serial`, `wwn`, `capacity`, `transport`, `rotational` | `/sys/block`, `/run/udev/data` |
| `filesystem/<mount>` | `type`, `uuid`, `size`, `disks` | `/proc/self/mountinfo`, `statvfs()`, udev |
| `network/<hash>` | `mac`, `medium`, `driver`, `hardware` | `/sys/class/net`, `ETHTOOL_GPERMADDR` |
| `os` | `id`, `version`, `name`, `kernel` | `/etc/os-release`, `uname()` |
| `battery/<hash>` | `manufacturer`, `model`, `serial`, `technology`, `design-energy` or `design-charge` | `/sys/class/power_supply` |

Only internal disks are recorded, not removable or USB ones, and only filesystems of a persistent type on them. A disk with
no model and serial, WWN or bus path to tell it apart (e.g. inside a container without udev) is left out, along with its
filesystems.

**Events.**

| Event | When | Time |
| --- | --- | --- |
| `discovery` | When the repository is created: every component found, each with `since` (when a filesystem, or the OS on the root filesystem, was created; otherwise `unknown`) | Now |
| `boot` | Once per boot, with the boot's ID, and the new kernel if it changed | The boot, from `btime` |
| Changes | When the device no longer matches its state: `installed`, `removed`, `replaced`, `resized`, `upgraded` or `reconfigured` | The boot, if found at the first comparison after one; otherwise now. `not_before` is the previous comparison. |

**Telemetry.**

| Stream | Columns | Source |
| --- | --- | --- |
| `system.load` | `load1`, `load5`, `load15` | `getloadavg()` |
| `memory` | `available` (bytes) | `/proc/meminfo` |
| `filesystem.root` | `used`, `available` (bytes) | `statvfs("/")` |

Capacities and sizes are state, not telemetry, and the time since boot is given by boot events. All streams use daily
chunks and a 5 minute `max_lag`. A stream is only defined once it has its first reading, so a platform that can't provide
one (e.g. macOS during development) leaves nothing behind.

### Docker Compose stacks

Optional, and off until `docker_host` is set. The agent reads containers that Docker Compose made from the Docker Engine API and
records them following the "Compose stacks" section of
[`DEVICE.md`](https://github.com/TiG-DB/tig-docs/blob/main/vocabularies/DEVICE.md):

- **State and events.** Each Compose project is an item `stack/<project>` and each replica of a service is `container/<project>/<service>/<n>`,
  with its `image`, image `digest` and `created` time. When the setting is first turned on, what is already running is
  found in one `discovery` event. After that, a container appearing is `installed`, one going is `removed`, one recreated from a
  new image or digest is `upgraded`, and one recreated from the same image is `reconfigured`. These are found when the agent
  compares the device with its state at each commit.
- **Telemetry.** A stream `container.<project>.<service>.<n>` per container, sampled with the rest, with `state`, `health`,
  `restarts`, `cpu` (cores in use since the previous reading) and `memory` (bytes, without the page cache). `cpu` is empty on a
  first reading, such as one made by `tig-agent collect` by hand.
- **Versions (optional).** `docker_versions = hub` names the release behind a tag such as `latest`. For each image the
  agent asks Docker Hub for the tags that share the digest of the image the container runs, and records the most specific
  release among them (e.g. `19.5.5-ce.0` for `gitlab/gitlab-ce:latest`) as `version` beside `image` and `digest`, so an
  `upgraded` event shows `19.5.4-ce.0` becoming `19.5.5-ce.0`. Labels in the image are not used, because they are
  inherited from its base image (GitLab's say Ubuntu's 24.04). It is off by default: apart from TiG Notary it is the only
  thing the agent requests from the Internet, and it tells Docker Hub which public images the machine runs. Only Docker Hub images are
  looked up, by name; ones from other registries or built locally get no `version`, and neither does an image whose
  tags Hub lists beyond the 500 most recent. Answers are kept while the agent runs, an image with none is asked about
  again after an hour, and a version that can't be found now never removes one already recorded for the same image. On
  a Docker that reports an image's ID as its configuration digest, the registry digest is read from
  `/images/<id>/json`, which needs `IMAGES=1` on the socket proxy.
- **Not recorded.** Environment variables, commands, ports, volumes, networks, other labels and the Compose file, and containers
  that Compose didn't make or that `docker compose run` started for a one-off task.
- **If Docker can't be reached,** the agent logs it once and records nothing about Docker until it can. Stacks already recorded
  are left alone, not taken as removed.

**Access.** The agent only ever sends `GET` requests, through `curl`, to `/containers/json`, `/containers/<id>/json` and
`/containers/<id>/stats`. The Docker socket itself is root-equivalent, so rather than giving the `tig` user access to it, run a
read-only proxy that allows only those requests and point `docker_host` at that. For example, with
[`tecnativa/docker-socket-proxy`](https://github.com/Tecnativa/docker-socket-proxy), which refuses everything but `GET` by default:

```yaml
services:
  docker-proxy:
    image: tecnativa/docker-socket-proxy
    environment:
      CONTAINERS: 1
    volumes:
      - /var/run/docker.sock:/var/run/docker.sock:ro
    ports:
      - 127.0.0.1:2375:2375
```

```
docker_host = http://127.0.0.1:2375
```

`docker_host` accepts only `unix:///path` and plain `http://host:port`, with no credentials in it. The proxy publishes only on the
loopback interface, so nothing else on the network can reach it.

### On disk

```
/etc/tig-agent/agent.conf        configuration (a dpkg conffile, so upgrades keep your edits)
/var/lib/tig/repository/         the TiG repository
/var/lib/tig/keys/               device key, deploy key and pinned known_hosts
/etc/tig-agent/product_serial    the device's serial number, copied from DMI by the install script
/var/lib/tig/agent.status        last sample, comparison, commit and push, and the last push result
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
runs on Ubuntu 22.04+ and Debian 12+. The package version is `0.2.0+<run number>.<commit>`, which `dpkg` orders correctly.

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
   gh release download --repo TiG-DB/tig-agent --pattern '*.deb'
   scp tig-agent_*.deb server:
   ssh server sudo apt install ./tig-agent_*.deb
   ```

   The agent starts at once and records locally.

2. **Give the agent somewhere to push.** Show the deploy key and add it to the data repository with write access:

   ```sh
   ssh server sudo -u tig tig-agent status | awk '/^deploy key/ {print $3, $4, $5}' >deploy_key.pub
   gh repo deploy-key add deploy_key.pub --repo you/machine --allow-write --title "tig-agent@server"
   ```

   Then set `remote = git@github.com:you/machine.git` in `/etc/tig-agent/agent.conf` and run
   `sudo systemctl restart tig-agentd`. The agent pushes immediately on start.

3. **Check it.**

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
| `docker_host` | *(empty)* | Docker Engine API to record Compose stacks from: `unix:///path` or `http://host:port`. Empty records nothing about Docker. |
| `docker_versions` | *(empty)* | `hub` to name the release behind image tags such as `latest` from Docker Hub. Empty or `off` asks nothing. |
| `notary` | *(empty)* | TiG Notary's URL, e.g. `https://notary.tigdb.org`, to have each sync's latest commit signed. Empty asks for no signatures. Must be `https`, or `http` to this machine. |
| `issuer` | `tigdb.org` | The hostname that publishes the notary's keys, which a signature is checked against. Only used with `notary`. |
| `issuer_keys` | *(empty)* | Where the issuer publishes its keys if not `https://<issuer>/.well-known/tig-public-key`. Same rules as `notary`. |
| `sample_interval` | `60s` | How often telemetry is sampled. |
| `commit_interval` | `15m` | How often readings are committed. |
| `push_interval` | `1h` | How often commits are pushed (each push commits first). |

Durations are a positive integer followed by `s`, `m`, `h` or `d`. Unknown settings are errors, so typos don't go unnoticed.

`tig-agent push` exits with status 3 when the remote rejects the push, 1 on other errors and 2 for usage errors.

## Development

```sh
git clone --recurse-submodules git@github.com:TiG-DB/tig-agent.git
cd tig-agent
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

It builds and its tests pass on macOS too, though only `system.load` and `filesystem.root` are available there, and
there are no boots or components to record. To build the
package as CI does, run `cd build && cpack` on Linux.

The tests are unit tests for configuration parsing and for recording state and events (discovery, boots and each kind of
change, from made-up inventories), and an end-to-end script that drives the real binary against a local bare repository:
init with its initial state, discovery and boot events, collect, push, a second fast-forward push, and a remote that has
diverged, which must be refused and left untouched. A second script runs it against a fake Docker Engine API (it needs
`python3`, and is skipped without it): stacks found at install, a container upgraded and another removed, Docker going away,
and stacks turned on in a repository that already has its discovery. It also checks that no configuration reaches the events.
A third runs it against a fake TiG Notary and its published keys (also `python3`): signatures made and pushed, nothing signed
twice, a notary that is down, rate limiting or ahead of the device's clock, tags that must be refused (another issuer, a key
that isn't published, a key published for another principal), and a notary that changes key.

`tig-lib` is a submodule in `external/tig-lib`. To pick up library changes, commit them there, then update the submodule here
in the same pull request as the agent changes that need them.

```
src/
  main.c         command line
  daemon.c       the service loop and its three cadences
  actions.c      collect, commit and sync, each under the agent lock and recorded in the status file
  telemetry.c    the streams and their samplers
  docker.c       Docker Compose stacks: items and telemetry, read from the Engine API
  inventory.c    the device's components, from the Linux sources in DEVICE.md
  record.c       initial state, discovery, boot and change events
  sha256.c       hashed item IDs
  sync.c         origin and `git push`, which also sends the signature tags
  notary.c       asking TiG Notary to sign a commit, checking the tag it returns and storing it
  repository.c   first-run setup, deploy key, lock
  device.c       device key and commit signing through ssh-keygen
  status.c       status file and `tig-agent status`
  config.c       configuration file
  process.c      running git and ssh-keygen with a timeout
  log.c          stderr logging with journald priorities
packaging/       agent.conf, systemd units, maintainer scripts, updater and release key
tests/           unit tests, end_to_end.sh, end_to_end_docker.sh with its fake Docker API (fake_docker.py) and
                 end_to_end_notary.sh with a fake notary (fake_notary.py)
```

## Not yet implemented

- **Later discoveries.** `DEVICE.md` has a new agent version record a discovery for a kind of item it starts recording.
  This version records every kind from the start, so there is nothing to discover later yet.
- **More components and packages.** Memory modules, graphics cards and installed packages are open questions in
  `DEVICE.md`.
- **Telemetry for other filesystems.** Only `filesystem.root` is sampled, though `DEVICE.md` names a stream for each.
- **TiG clock.** Readings and events use the system clock, without the offset measured against the notary. The agent
  uses the notary's time only to make a signing request that the notary has said is too far out.
- **Verifying a repository.** The agent checks each signature as it stores it, but doesn't yet check a whole history
  (`COMMITS.md`, "Verifying"); `tig-cli` is meant to.
- **Signing on stopping.** The daemon commits when it stops but doesn't ask for a signature, so that commit is signed at the
  next sync after it starts.

## Licence

Licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE).

The names "TiG" and "TiG Notary" and the TiG logos are not covered by that
licence and may not be used without permission, except to say accurately that your
work is compatible with TiG.
