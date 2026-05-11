# Security Notes

## Input Validation

All user inputs from the web form are sanitized before use:

| Field | Validation |
|-------|-----------|
| `enabled` / `debug` | Presence check only (checkbox) |
| `profile` | Must be one of: `safe`, `default`, `aggressive`, `custom` |
| `divert_port` | Integer, clamped to 1–65535 via `zapret2_sanitize_port()` |
| `custom_args` | Stripped to printable ASCII via `zapret2_sanitize_args()`; used only when `profile=custom` |

Removed fields (no longer in GUI): `interfaces[]`, `targets[]`, `custom_domains` — these were unused and have been removed to reduce attack surface.

## Shell Argument Safety

- All values passed to shell commands use `escapeshellarg()`
- `dvtws2` command is assembled in PHP from validated config values — never constructed from raw user input
- `custom_args` is marked "Advanced / Dangerous" in the UI and is only passed to dvtws2 when profile is explicitly set to `custom`

## CSRF Protection

AJAX requests (Health Check button) include pfSense's `csrfMagicName`/`csrfMagicToken` from the page, which pfSense validates server-side. Direct POST without a valid token returns a CSRF error page.

## Root and Service Risks

- dvtws2 runs as `root` — required for divert socket and IPFW access on FreeBSD
- The plugin loads kernel modules (`ipfw`, `ipdivert`) on service start — this is expected and safe on pfSense; both modules are standard FreeBSD kernel modules
- `pfctl -d ; pfctl -e` is executed on start — this momentarily disables pf then immediately re-enables it. All pf rules remain intact; this is required for pfSense 2.6+ to allow IPFW to process packets
- The GUI page uses pfSense's `##|+PRIV` access control — only users with the `page-services-zapret2` privilege can access it

## IPFW Rule Safety

- The plugin only manages IPFW rule number `100`
- On stop/uninstall, only rule `100` is deleted: `ipfw delete 100`
- The plugin never calls `ipfw flush` or deletes rules by name or range
- If rule 100 was already used by something else, the plugin will overwrite it on start — check `ipfw list` before installing if you use IPFW for other purposes

## File System Safety

- `rm` is never called on user-controlled paths
- `zapret2_stop()` only kills the PID from `/var/run/zapret2.pid` — never scans for processes by name
- `uninstall.sh` removes only explicitly listed files — no wildcards or recursive deletes on variable paths
- Log file is append-only (`FILE_APPEND` flag in PHP)

## Rollback

If something goes wrong:

```sh
# Stop immediately and clean up IPFW:
kill $(cat /var/run/zapret2.pid) 2>/dev/null
ipfw delete 100 2>/dev/null

# Full removal:
sh /path/to/pfSense-pkg-zapret2/uninstall.sh
```

pfSense config is restored by removing the `<zapret2>` block from `config.xml` (see UNINSTALL.md). All other pfSense settings are untouched.

## Reporting Security Issues

Please report security issues privately via GitHub Security Advisories before any public disclosure.
