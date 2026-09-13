# pfSense-pkg-zapret2

**Status: MVP / Experimental**

A pfSense GUI plugin for managing [zapret2](https://github.com/bol-van/zapret2) — a DPI bypass tool that helps circumvent deep packet inspection used by some ISPs to block or throttle services.

On FreeBSD/pfSense, zapret2 uses `dvtws2` — a divert-socket based engine (not nfqws/tpws which are Linux-only).

---

## What It Does

- Adds a **Services → Zapret2** page to the pfSense web interface
- Downloads and installs the zapret2 backend (`dvtws2` + Lua files) automatically via `make install`
- Manages the service lifecycle (start/stop/restart) via FreeBSD rc.d
- Supports IPFW divert and opt-in PF divert for IPv4 LAN clients on pfSense 2.8.x
- Registers PF rules through the pfSense package filter hook so they are regenerated on firewall Apply
- **YouTube DPI bypass** — hostlist filter for YouTube and Google Video domains (youtube.com, googlevideo.com, ytimg.com, etc.); optional UDP 443 divert for QUIC/HTTP3
- **Discord DPI bypass** — hostlist filter for Discord domains; optional UDP divert for voice/video (UDP 443 + 50000–65535)
- **Alias include mode** — restrict processing to specific IP/CIDR firewall alias (ipfw table 1)
- Shows service status, active IPFW rules, hostlist state, live dvtws2 arguments, and recent log entries
- Built-in **Health Check** (process, PID file, kernel modules, IPFW rules, hostlists)
- Built-in **Profile Tester** — tests each profile via HTTP/TLS or DPI checks and recommends the best one

## What It Does NOT Do

- Does **not** guarantee bypass effectiveness — results depend on your ISP's DPI system
- PF mode adds early allow/divert rules for explicitly selected LAN/OPT subnets; normal firewall restrictions and policy routing do not apply to matching traffic
- Does **not** edit pfSense core files or manually patch `/tmp/rules.debug`
- Does **not** modify any existing pfSense configuration outside its own config section
- Does **not** reboot the system automatically

## Supported pfSense Versions

| pfSense Version | Status |
|----------------|--------|
| 2.8.x (CE)     | PF backend; lifecycle verified on 2.8.1, LAN validation required |
| 2.7.x (CE)     | Tested |
| 2.6.x (CE)     | Should work |
| 23.09+ (Plus)  | Should work |

FreeBSD 13.x/14.x with IPFW; FreeBSD 15 with PF. amd64 only.

## Quick Start

```sh
# Clone the repository on your local machine (not on pfSense)
git clone https://github.com/pdazcom/pfSense-pkg-zapret2
cd pfSense-pkg-zapret2

# Install everything on pfSense in one command
# (downloads zapret2 backend locally, copies everything via scp)
make install HOST=root@<pfsense-ip>
```

Then open **Services → Zapret2** in the pfSense GUI, select a profile, click **Apply**.

For pfSense 2.8.x, choose **PF divert**, select the LAN/OPT interfaces, and explicitly enable **PF Traffic Allowance**. PF processes IPv4 traffic from those interface subnets and excludes router addresses. TCP 80/443 is enabled; QUIC and Discord UDP remain optional. Reconnect existing client sessions after enabling.

The built-in Profile Tester runs on the router and cannot evaluate this LAN-only PF path. PF mode explains this instead of starting a misleading test; use a LAN client. For IPFW tests, missing/crashed runners now report errors and write diagnostics to `/var/log/zapret2-test.log`.

See [INSTALL.md](INSTALL.md) for the full guide.

## Makefile Targets

| Target | Description |
|--------|-------------|
| `make install HOST=root@<ip>` | Full install: backend + plugin |
| `make update HOST=root@<ip>` | Update plugin files only (no backend re-download) |
| `make uninstall HOST=root@<ip>` | Remove plugin (keep backend) |
| `make uninstall HOST=root@<ip> UNINSTALL_ARGS=--remove-backend` | Remove everything |
| `make check` | PHP lint + shell syntax check |

## Project Structure

```
pfSense-pkg-zapret2/
├── Makefile                                    — install/update/uninstall/check
├── install.sh                                  — installer script (called by make install)
├── uninstall.sh                                — uninstaller script
├── pkg-descr                                   — FreeBSD pkg short description
├── pkg/
│   └── zapret2.xml                             — pfSense package manifest
├── files/
│   ├── usr/local/www/zapret2/zapret2.php       — GUI page
│   ├── usr/local/pkg/zapret2/includes/zapret2.inc — PHP helpers
│   ├── usr/local/etc/rc.d/zapret2              — rc.d boot script
│   └── usr/local/etc/zapret2/profiles.conf     — dvtws2 strategy profiles
├── scripts/
│   ├── healthcheck.sh                          — health check
│   └── configtest.sh                           — config validation / dry-run
└── examples/
    └── config.xml.snippet                      — config.xml documentation
```

## How It Works

```
GUI (zapret2.php) → save config → config.xml
                  → apply/start → zapret2.inc CLI
                                      ↓ kldload ipfw + ipdivert
                                      ↓ ipfw add 100 divert 990 tcp 80,443 ...
                                      ↓ ipfw add 101 divert 990 udp 443 ...   (YouTube QUIC / Discord)
                                      ↓ ipfw add 102 divert 990 udp 50000-65535 ... (Discord voice)
                                      ↓ write youtube-hostlist.txt / discord-hostlist.txt
                                      ↓ dvtws2 --daemon --port 990
                                            --lua-init zapret-lib.lua
                                            --lua-init zapret-antidpi.lua
                                            --lua-desync <profile strategy>
                                            --hostlist youtube-hostlist.txt   (if YouTube enabled)
                                            --hostlist discord-hostlist.txt   (if Discord enabled)
```

## License

BSD 2-Clause.
