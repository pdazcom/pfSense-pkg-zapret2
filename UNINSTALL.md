# Uninstall Guide

Back up your pfSense config first: **Diagnostics → Backup & Restore → Download configuration as XML**

---

## Quick uninstall

```sh
# From your local machine — removes plugin, keeps dvtws2 backend:
make uninstall HOST=root@<pfsense-ip>

# Remove everything including dvtws2 and Lua files:
make uninstall HOST=root@<pfsense-ip> UNINSTALL_ARGS=--remove-backend
```

Or run directly on pfSense:

```sh
sh /tmp/pfSense-pkg-zapret2/uninstall.sh
sh /tmp/pfSense-pkg-zapret2/uninstall.sh --remove-backend
```

The uninstaller:
1. Stops the dvtws2 process (via PID file)
2. Removes the IPFW divert rule
3. Removes `zapret2_enable` from `/etc/rc.conf.local`
4. Removes all plugin files
5. Removes the Services → Zapret2 menu and config from pfSense's `config.xml`
6. Optionally removes dvtws2 binary and Lua files (`--remove-backend`)

---

## Manual uninstall

If you don't have the repo available, run these on pfSense:

### 1. Stop the service and clean up IPFW

```sh
# Stop process
if [ -f /var/run/zapret2.pid ]; then
    kill $(cat /var/run/zapret2.pid) 2>/dev/null
    rm -f /var/run/zapret2.pid
fi

# Remove IPFW divert rule
ipfw delete 100 2>/dev/null

# Disable on boot
sed -i '' '/zapret2_enable/d' /etc/rc.conf.local
```

### 2. Remove plugin files

```sh
rm -f /usr/local/www/zapret2/zapret2.php
rm -f /usr/local/pkg/zapret2/includes/zapret2.inc
rm -f /usr/local/etc/rc.d/zapret2
rm -f /usr/local/etc/zapret2/profiles.conf
rm -f /usr/local/share/zapret2/healthcheck.sh
rm -f /usr/local/share/zapret2/configtest.sh
rm -f /var/log/zapret2.log
rmdir /usr/local/www/zapret2 /usr/local/pkg/zapret2/includes \
      /usr/local/pkg/zapret2 /usr/local/share/zapret2 2>/dev/null || true
```

### 3. Remove from pfSense config

```sh
php -r "
  require_once('/etc/inc/globals.inc');
  require_once('/etc/inc/config.inc');
  require_once('/etc/inc/functions.inc');
  global \$config;
  if (is_array(\$config['installedpackages']['menu'] ?? null)) {
    \$config['installedpackages']['menu'] = array_values(array_filter(
      \$config['installedpackages']['menu'],
      function(\$m) { return (\$m['name'] ?? '') !== 'Zapret2'; }
    ));
  }
  unset(\$config['installedpackages']['zapret2']);
  write_config('Zapret2: uninstall');
  echo 'Done' . PHP_EOL;
"
```

### 4. Remove backend (optional)

```sh
rm -f /usr/local/bin/dvtws2
rm -f /usr/local/etc/zapret2/zapret-lib.lua
rm -f /usr/local/etc/zapret2/zapret-antidpi.lua
rmdir /usr/local/etc/zapret2 2>/dev/null || true
```

---

After uninstall, pfSense will not start zapret2 on reboot and the Services → Zapret2 menu entry will be gone after a page reload.
