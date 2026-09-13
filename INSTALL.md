# Installation Guide

- [Quick install](#quick-install)
- [A. Requirements](#a-requirements)
- [B. Installation](#b-installation)
- [C. Post-install: pfSense network configuration](#c-post-install-pfsense-network-configuration)
- [D. Verifying the service](#d-verifying-the-service)
- [E. Traffic Filtering Options](#e-traffic-filtering-options)
- [F. Testing bypass from a LAN client](#f-testing-bypass-from-a-lan-client)

---

## Quick install

Run this on your **local machine** (not on pfSense). You need `make`, `curl`, and SSH access to the router.

```sh
git clone https://github.com/pdazcom/pfSense-pkg-zapret2
cd pfSense-pkg-zapret2
make install HOST=root@<pfsense-ip>
```

This will:
1. Download the zapret2 release tarball (`dvtws2` binary + Lua files) locally
2. Copy everything to pfSense via scp — no internet access needed on pfSense
3. Install plugin files and register the GUI menu
4. Run `configtest.sh` to verify the result

Then open **Services → Zapret2** in the pfSense GUI.

---

## A. Requirements

### On your local machine (runs `make install`)
- `make`, `curl`, `ssh`, `scp`
- Internet access (to download the zapret2 tarball once)

### On pfSense
- pfSense CE 2.6+ or pfSense Plus 23.09+
- FreeBSD **amd64** — `dvtws2` pre-built binary is amd64 only
- SSH root access
- PHP 8.x (included with pfSense)
- No special packages needed — installer copies everything via scp

Verify your environment:

```sh
# On pfSense shell:
cat /etc/version          # pfSense version
uname -r                  # FreeBSD version
uname -m                  # Must be: amd64
```

---

## B. Installation

### Option 1: `make install` (recommended)

```sh
# From your local machine:
git clone https://github.com/pdazcom/pfSense-pkg-zapret2
cd pfSense-pkg-zapret2
make install HOST=root@192.168.1.1
```

The installer will:
- Download `zapret2-v0.9.5.2.tar.gz` from GitHub releases (cached in `/tmp/` after first run)
- Extract and copy `dvtws2` binary to `/usr/local/bin/dvtws2`
- Copy Lua strategy files to `/usr/local/etc/zapret2/`
- Install plugin files to standard pfSense paths
- Register **Services → Zapret2** menu in pfSense config
- Run `configtest.sh` — all lines should show `[OK]`

To reinstall/upgrade the backend binary:

```sh
make install HOST=root@192.168.1.1   # skips backend if already installed
# Force re-install of dvtws2:
ssh root@192.168.1.1 'sh /tmp/pfSense-pkg-zapret2/install.sh --force'
```

### Option 2: Run `install.sh` directly on pfSense

If you can copy the repo to the router:

```sh
scp -r . root@192.168.1.1:/tmp/pfSense-pkg-zapret2
ssh root@192.168.1.1 'cd /tmp/pfSense-pkg-zapret2 && sh install.sh'
```

### Updating plugin files only (no backend re-download)

During development or after pulling updates:

```sh
make update HOST=root@192.168.1.1
```

### Installed file locations

| File | Path on pfSense |
|------|----------------|
| dvtws2 binary | `/usr/local/bin/dvtws2` |
| Lua strategy files | `/usr/local/etc/zapret2/zapret-lib.lua`, `zapret-antidpi.lua` |
| Profile templates | `/usr/local/etc/zapret2/profiles.conf` |
| GUI page | `/usr/local/www/zapret2/zapret2.php` |
| PHP helpers | `/usr/local/pkg/zapret2/includes/zapret2.inc` |
| rc.d script | `/usr/local/etc/rc.d/zapret2` |
| Health check | `/usr/local/share/zapret2/healthcheck.sh` |
| Config test | `/usr/local/share/zapret2/configtest.sh` |
| Log file | `/var/log/zapret2.log` |

---

## PF divert on pfSense CE 2.8.x

1. Ensure the pfSense firewall is enabled. PF mode will not enable a disabled firewall automatically. Confirm a LAN client can access the Internet through pfSense with Zapret2 stopped. Check its gateway and pfSense outbound NAT first.
2. Open **Services → Zapret2**, enable the service, and choose **PF divert** under Traffic Filtering.
3. Select the intended LAN/OPT interfaces and enable **PF Traffic Allowance**. The generated rules explicitly allow matching traffic before normal firewall restrictions and policy routing. WAN is excluded; router addresses are excluded even when using a destination alias.
4. Choose a profile or enter Custom arguments. Enable YouTube QUIC / Discord UDP when the strategy needs those packets; putting `--filter-udp` in Custom arguments alone does not enable UDP interception.
5. Apply, reconnect client sessions, and test from the LAN client. Only IPv4 clients in the selected interface subnets are covered. Routed downstream subnets and IPv6 are outside this PF mode's scope.

The package registers `zapret2_generate_rules` with pfSense's package filter hook. Reboot and firewall Apply regenerate the rules; no edits to `/etc/inc/filter.inc` or `/tmp/rules.debug` are needed. Stop removes the rules and clears states labelled `zapret2` before stopping the listener.

Health Check verifies the selected backend, listener and rules. It does not prove DPI bypass. The Profile Tester runs locally on the router, so PF mode directs you to test from a LAN client instead.

To switch back to IPFW, select **IPFW (legacy)** and Apply. Existing configurations retain IPFW until explicitly changed.

## C. Post-install: pfSense network configuration

### How dvtws2 intercepts traffic

dvtws2 uses **IPFW divert sockets** — this is completely separate from pfSense's pf firewall. The plugin automatically manages the required IPFW rule when starting and stopping the service:

```sh
ipfw add 100 divert 990 tcp from any to any 80,443 out not diverted not sockarg
```

This rule intercepts all outbound TCP on ports 80 and 443, passes packets to dvtws2 on port 990, dvtws2 applies the Lua DPI bypass strategy, and re-injects the modified packets.

> **Do NOT add NAT/port forward rules** for zapret2 — the divert approach operates at a lower level and does not require NAT.

### pfSense 2.6.0+ — pfil hooks

On pfSense 2.6+, the packet filter hook ordering can prevent IPFW from seeing packets. The plugin automatically handles this on every start:

```sh
pfctl -d ; pfctl -e   # momentarily disables and re-enables pf
```

This is safe — pf is re-enabled immediately and all pf rules remain intact.

### Kernel modules

The plugin loads these automatically on start:

```sh
kldload ipfw
kldload ipdivert
```

They are not persisted to `/boot/loader.conf` by default. If you want them loaded at boot independently of the plugin:

```sh
echo 'ipfw_load="YES"'     >> /boot/loader.conf
echo 'ipdivert_load="YES"' >> /boot/loader.conf
```

---

## D. Verifying the service

### Via GUI

1. Open **Services → Zapret2**
2. Select a **Profile** (start with `Default`)
3. Click **Save**, then **Apply**
4. The status block should show **Running** with a valid PID
5. Click **Health Check** — all items should show `[PASS]`

### Via shell

```sh
# Service status
service zapret2 status

# Check process
ps aux | grep dvtws2

# Check IPFW rule is active
ipfw list | grep divert

# Check kernel modules
kldstat | grep -E 'ipfw|ipdivert'

# Run config validation + dry-run
sh /usr/local/share/zapret2/configtest.sh

# Run health check
sh /usr/local/share/zapret2/healthcheck.sh

# Watch log
tail -f /var/log/zapret2.log
```

---

## E. Traffic Filtering Options

### YouTube DPI Bypass

Enable **YouTube DPI Bypass (TCP 443)** to restrict dvtws2 to YouTube and Google Video domains only (youtube.com, googlevideo.com, ytimg.com, youtu.be, and 20+ related domains). dvtws2 applies the selected profile strategy only to matching traffic.

Optionally enable **YouTube QUIC / HTTP3 (UDP 443)** to also intercept UDP 443 — required when browsers use HTTP/3 instead of TCP.

### Discord DPI Bypass

Enable **Discord DPI Bypass (TCP 443)** to restrict dvtws2 to Discord domains (discord.com, discordapp.com, discord.gg, discord.media, etc.).

Optionally enable **Discord Voice/Video (UDP)** to add IPFW rules for UDP 443 (QUIC) and UDP 50000–65535 (voice/video).

### Alias Include Mode

Select a pfSense **Firewall Alias** containing IPs or subnets to restrict dvtws2 to those destinations only. Everything else bypasses dvtws2 entirely. Hostname entries in the alias are skipped (ipfw tables require IPs/CIDRs).

Alias mode is ignored when YouTube or Discord bypass is enabled — in those modes dvtws2 filters by domain name itself.

### Combining modes

YouTube and Discord hostlists can be active simultaneously — dvtws2 receives both `--hostlist=` arguments and processes traffic matching either list.

---

## F. Testing bypass from a LAN client

> **Important:** Test from a LAN client (PC, phone), not from the pfSense shell itself. dvtws2 intercepts traffic routed through the router, not traffic originating from the router.

1. Make sure the client's default gateway is pfSense's LAN IP
2. Make sure no system-level proxy is configured on the client
3. Test with a browser or curl from the client:

```sh
# From a LAN client:
curl -4 -I --max-time 10 https://www.youtube.com/generate_204
curl -4 -I --max-time 10 https://discord.com
curl -4 -I --max-time 10 https://rutracker.org
```

Compare with service stopped:

```sh
# Stop zapret2 on pfSense:
service zapret2 stop

# Test from client — should fail or be slow if ISP blocks it
curl -4 -I --max-time 10 https://rutracker.org

# Start zapret2 again:
service zapret2 start
```
