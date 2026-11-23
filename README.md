# tig-agent

Linux collector for [TiG](https://github.com/TiG-DB/tig-docs), built on
[libtig](https://github.com/TiG-DB/tig-lib). Records device state, changes and
telemetry in a local repository, with optional Git push and TiG Notary timestamps.
Runs as the `tig-agentd` systemd service.

See the [whitepaper](https://github.com/TiG-DB/tig-docs/blob/main/WHITEPAPER.md)
for the rationale and the [specification](https://github.com/TiG-DB/tig-docs)
for the format, particularly:

- [Device vocabulary](https://github.com/TiG-DB/tig-docs/blob/main/vocabularies/DEVICE.md): hardware, OS and Docker Compose records.
- [Events](https://github.com/TiG-DB/tig-docs/blob/main/EVENTS.md) and [telemetry](https://github.com/TiG-DB/tig-docs/blob/main/TELEMETRY.md).
- [Commit signing and notary protocol](https://github.com/TiG-DB/tig-docs/blob/main/COMMITS.md).
- [Roadmap](https://github.com/TiG-DB/tig-docs/blob/main/ROADMAP.md).

## Install

Download the latest `.deb` from [releases](https://github.com/TiG-DB/tig-agent/releases)
and install it on Ubuntu 22.04+ or Debian 12+:

```sh
sudo apt install ./tig-agent_*.deb
```

The service starts recording locally in `/var/lib/tig/repository`. It runs as
user `tig`; keys and status are also kept under `/var/lib/tig`. Package removal
keeps the repository.

To push, add the deploy key shown by `sudo -u tig tig-agent status` to an empty
Git remote with write access. Set `remote` in `/etc/tig-agent/agent.conf`, then:

```sh
sudo systemctl restart tig-agentd
sudo -u tig tig-agent status
journalctl -u tig-agentd -f
```

Pushes are fast-forward only. A rejected push pauses automatic pushing; resolve
the divergence, then retry with `sudo -u tig tig-agent push`.

## Configuration

Edit `/etc/tig-agent/agent.conf` and restart the service. See the
[annotated configuration](packaging/agent.conf) for settings and defaults.
Use `--config PATH` or `$TIG_AGENT_CONFIG` for another file.

Defaults: sample every 60 seconds, compare state and commit every 15 minutes,
push every hour. An empty `remote` keeps data local. Every commit is signed
with the device key; notary timestamps require `notary` to be set.

Docker Compose collection is optional (`docker_host`). Use a read-only socket
proxy; see the configuration comments for access requirements. Docker Hub
version lookups are separately enabled with `docker_versions = hub`.

Manual commands: `init`, `collect`, `commit`, `push`, `status`, `daemon`.
Run `tig-agent --help` for usage. Exit codes: 1 for errors, 2 for usage errors,
3 for a rejected push.

## Updates

Each merge to `main` publishes a `.deb`. The installed
[`tig-agent-update.timer`](packaging/systemd/tig-agent-update.timer) checks for
releases every two minutes. The [updater](packaging/tig-agent-update) verifies
the signed checksum, installs the package and rolls back if its health check
fails. Rollback requires a previously cached package.

## Build

```sh
git clone --recurse-submodules git@github.com:TiG-DB/tig-agent.git
cd tig-agent
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

C99, with `tig-lib` pinned in `external/tig-lib`. On Linux, run `cpack` from
`build/` to produce the `.deb`. macOS builds support only load and root
filesystem telemetry.

## Limitations

- Filesystem telemetry covers only `/`.
- Events and readings use the system clock, without the TiG clock offset.
- Whole-history verification is not implemented; see `tig-cli`.
- The shutdown commit receives a notary timestamp at the next sync.
- Discoveries for newly supported item types are not implemented.

## Licence

Licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE).

The names "TiG" and "TiG Notary" and the TiG logos are not covered by that
licence and may not be used without permission, except to say accurately that your
work is compatible with TiG.
