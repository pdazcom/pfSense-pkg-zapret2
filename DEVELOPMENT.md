# Development Guide

## Project Structure

```
pfSense-pkg-zapret2/
├── Makefile                                    — install/update/uninstall/check
├── install.sh                                  — full installer (backend + plugin)
├── uninstall.sh                                — uninstaller
├── pkg-descr                                   — FreeBSD pkg short description
├── pkg/
│   └── zapret2.xml                             — pfSense package manifest
├── files/
│   ├── usr/local/www/zapret2/
│   │   ├── zapret2.php                         — GUI page (Services → Zapret2)
│   │   └── zapret2_test_runner.php             — async profile tester backend (CLI)
│   ├── usr/local/pkg/zapret2/includes/
│   │   └── zapret2.inc                         — config helpers, service control, install hooks
│   ├── usr/local/etc/rc.d/
│   │   └── zapret2                             — FreeBSD rc.d boot script
│   └── usr/local/etc/zapret2/
│       ├── profiles.conf                       — dvtws2 Lua strategy profiles (examples)
│       ├── youtube-hostlist.txt                — generated on start when YouTube bypass enabled
│       └── discord-hostlist.txt                — generated on start when Discord bypass enabled
├── scripts/
│   ├── healthcheck.sh                          — health check (process, IPFW, modules)
│   └── configtest.sh                           — config validation + dry-run
└── examples/
    └── config.xml.snippet                      — annotated config.xml example
```

## Architecture

```
GUI (zapret2.php)
    ↓ POST: save / apply / start / stop / restart / healthcheck_ajax
    ↓ POST: start_test_ajax / poll_test_ajax / stop_test_ajax  (profile tester)
zapret2.inc  →  zapret2_save_config()  →  config.xml
             →  zapret2_resync()
                    ↓ zapret2_load_kernel_modules()     →  kldload ipfw + ipdivert
                    ↓ zapret2_set_ipfw_sysctls()        →  sysctl net.inet...
                    ↓ zapret2_add_ipfw_rule()
                            ipfw add 100 divert tcp 80,443      (always)
                            ipfw add 101 divert udp 443         (YouTube QUIC or Discord UDP)
                            ipfw add 102 divert udp 50000-65535 (Discord voice/video)
                            ipfw table 1                        (alias include mode)
                    ↓ zapret2_write_youtube_hostlist()  →  youtube-hostlist.txt
                    ↓ zapret2_write_discord_hostlist()  →  discord-hostlist.txt
                    ↓ zapret2_build_command()
                            dvtws2 --daemon --port 990
                            --lua-init zapret-lib.lua
                            --lua-init zapret-antidpi.lua
                            --lua-desync <strategy from profile>
                            --hostlist youtube-hostlist.txt   (if youtube_enabled)
                            --hostlist discord-hostlist.txt   (if discord_enabled)
                    ↓ PID → /var/run/zapret2.pid
                    ↓ log → /var/log/zapret2.log
```

Config is stored in pfSense's `config.xml` at:
`$config['installedpackages']['zapret2']['config'][0]`

## Makefile Targets

| Target | What it does |
|--------|-------------|
| `make install HOST=root@<ip>` | Downloads tarball locally, copies to pfSense, runs install.sh |
| `make update HOST=root@<ip>` | Copies plugin files only via scp (fast, no backend re-download) |
| `make uninstall HOST=root@<ip>` | Runs uninstall.sh on pfSense |
| `make uninstall HOST=root@<ip> UNINSTALL_ARGS=--remove-backend` | Also removes dvtws2 + Lua files |
| `make check` | PHP lint + shell -n syntax checks + xmllint |

## Setting Up a Development Environment

### Recommended: pfSense VM + `make update`

1. Download pfSense CE ISO from pfsense.org
2. Create a VM: 1 CPU, 512 MB RAM, 8 GB disk, two NICs (WAN + LAN)
3. Install pfSense, configure LAN/WAN
4. Enable SSH: **System → Advanced → Admin Access → Enable Secure Shell**
5. Run first full install: `make install HOST=root@<vm-ip>`
6. For subsequent changes: `make update HOST=root@<vm-ip>` (fast scp only)

### Local syntax check only (no pfSense)

```sh
make check
```

Requires PHP 8.x CLI. No pfSense needed — just validates syntax.

## Coding Conventions

### PHP (`zapret2.inc`, `zapret2.php`)

- PHP 8.x syntax, strict types not required (pfSense compatibility)
- All shell arguments via `escapeshellarg()`
- Config read via `zapret2_get_config()`, write via `zapret2_save_config()`
- All functions prefixed `zapret2_` to avoid namespace collisions
- CSRF protection: AJAX requests must include `csrfMagicName`/`csrfMagicToken`

### Shell (`install.sh`, `uninstall.sh`, `healthcheck.sh`, `configtest.sh`, `rc.d/zapret2`)

- POSIX `#!/bin/sh` only — no bash-isms
- Quote all variables: `"${VAR}"`
- `[ ]` not `[[ ]]`
- No `rm -rf` on user-controlled or variable paths
- Must pass `sh -n` with no errors

### Key pfSense API used

| Function | Purpose |
|----------|---------|
| `write_config('msg')` | Serialize config array to config.xml |
| `get_configured_interface_with_descr()` | List LAN/OPT interfaces |
| `mwexec($cmd)` | Run shell command synchronously |
| `print_info_box($msg, 'success')` | Show notification |
| `Form`, `Form_Section`, `Form_Checkbox`, `Form_Select`, `Form_Textarea`, `Form_Input` | pfSense form builder |

## Iterating Quickly

```sh
# Edit a file locally, then push just that file:
make update HOST=root@192.168.1.1

# Or push a single file manually:
scp files/usr/local/www/zapret2/zapret2.php root@192.168.1.1:/usr/local/www/zapret2/zapret2.php

# On pfSense, check the log:
ssh root@192.168.1.1 'tail -f /var/log/zapret2.log'
```

## Profiles

Profiles are defined in `zapret2_get_profile_args()` inside `zapret2.inc`. Each profile maps to `--lua-desync=` arguments passed to `dvtws2`:

| Profile | Strategy |
|---------|---------|
| `safe` | `--lua-desync=multisplit` |
| `default` | `--lua-desync=multisplit --lua-desync=fake:blob=fake_default_tls:tcp_md5` |
| `multidisorder` | `--lua-desync=multidisorder:pos=1,midsld:seqovl=1` — Rostelecom, Beeline, MTS |
| `fake_disorder` | `--lua-desync=fake:blob=fake_default_tls:tcp_md5:repeats=11 --lua-desync=multidisorder:pos=1,midsld` |
| `wssize` | `--lua-desync=wssize:wsize=1:scale=6 --lua-desync=multisplit:pos=midsld` |
| `fake_aggressive` | `--lua-desync=fake:blob=fake_default_tls:tcp_md5:repeats=15 --lua-desync=multidisorder:pos=1,midsld` |
| `syndata` | `--lua-desync=syndata --lua-desync=multisplit:pos=host` |
| `wssize_disorder` | `--lua-desync=wssize:wsize=1:scale=6 --lua-desync=multidisorder:pos=1,midsld:seqovl=1` |
| `tls_clone` | `--lua-desync=tls_client_hello_clone --lua-desync=multisplit:pos=midsld` |
| `custom` | raw `custom_args` from config verbatim |

`profiles.conf` is a human-readable documentation file; the actual values are in `zapret2.inc`.

## Releasing

1. Update `ZAPRET2_VERSION` in `Makefile` and `install.sh` if upgrading upstream
2. Tag: `git tag v1.x.x`
3. Push tag — no build step needed (pure PHP/shell package)

## PF backend and regression checks

The package XML declares `filter_rules_needed`; installation/update registers the corresponding `filter_rule_function` in `installedpackages/package`. `zapret2_generate_rules("pfearly")` returns scoped PF anchors only while the listener is running. The active settings live in `/var/run/zapret2-active.json`, so saving unapplied settings does not silently change running rules during a firewall reload.

PF start launches the listener before enabling rules. Stop removes interception and labelled states before terminating the listener. PF mode loads `ipdivert`, without loading IPFW or changing pfil ordering. Rule regeneration uses the same includes as `/etc/rc.filter_configure_sync`.

Run `make check` for syntax and PF scope/validation checks. On a pfSense test VM also verify start/restart/stop, ordinary filter reload, reboot, destination alias updates, failed daemon startup, and LAN TCP/QUIC traffic. Retest legacy IPFW on its supported release before declaring the version matrix fully verified.

The installer must deploy `zapret2_test_runner.php`. Test progress is written atomically, cancellation uses a separate flag, and a process lock prevents concurrent runners. Startup failure, stale progress, and bootstrap errors are visible to the GUI. Cleanup restores the previous running/stopped state. PF mode rejects router-local profile tests because they do not traverse the selected LAN interfaces.
