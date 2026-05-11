<?php
/*
 * zapret2.inc — pfSense package helper for Zapret2 DPI bypass
 *
 * This file is included by:
 *  - zapret2.php (GUI page)
 *  - zapret2 rc.d script (via PHP CLI)
 *  - package XML hooks (install/deinstall/resync)
 */

if (!defined('ZAPRET2_INCLUDED')) {
    define('ZAPRET2_INCLUDED', true);
}

define('ZAPRET2_DVTWS_BIN',    '/usr/local/bin/dvtws2');
define('ZAPRET2_LUA_DIR',      '/usr/local/etc/zapret2');
define('ZAPRET2_CONF_DIR',     '/usr/local/etc/zapret2');
define('ZAPRET2_PROFILES_CONF','/usr/local/etc/zapret2/profiles.conf');
define('ZAPRET2_LOG_FILE',     '/var/log/zapret2.log');
define('ZAPRET2_PID_FILE',     '/var/run/zapret2.pid');
define('ZAPRET2_DIVERT_PORT',  990);
define('ZAPRET2_TAG',          'pfSense-pkg-zapret2');

// ---------------------------------------------------------------------------
// Config read/write
// ---------------------------------------------------------------------------

function zapret2_get_config(): array
{
    global $config;
    return $config['installedpackages']['zapret2']['config'][0] ?? [];
}

function zapret2_save_config(array $cfg): void
{
    global $config;
    if (!is_array($config['installedpackages']['zapret2'] ?? null)) {
        $config['installedpackages']['zapret2'] = [];
    }
    if (!is_array($config['installedpackages']['zapret2']['config'] ?? null)) {
        $config['installedpackages']['zapret2']['config'] = [];
    }
    $config['installedpackages']['zapret2']['config'][0] = $cfg;
    write_config('Zapret2: save configuration');
}

// ---------------------------------------------------------------------------
// Input sanitization
// ---------------------------------------------------------------------------

function zapret2_sanitize_iface(string $raw): string
{
    // Interface names: letters, digits, underscores only
    return preg_replace('/[^a-zA-Z0-9_]/', '', $raw);
}

function zapret2_sanitize_domains(string $raw): array
{
    $lines   = preg_split('/[\r\n]+/', $raw);
    $domains = [];
    foreach ($lines as $line) {
        $domain = strtolower(trim($line));
        // Skip empty lines and comments
        if ($domain === '' || $domain[0] === '#') {
            continue;
        }
        // Basic domain validation: allow hostnames and wildcard prefix (*.example.com)
        $check = ltrim($domain, '*.');
        if (preg_match('/^[a-z0-9]([a-z0-9\-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9\-]*[a-z0-9])?)*$/', $check)) {
            $domains[] = $domain;
        }
    }
    return $domains;
}

function zapret2_sanitize_args(string $raw): string
{
    // Strip null bytes and control characters; keep printable ASCII
    return preg_replace('/[^\x20-\x7E]/', '', trim($raw));
}

function zapret2_sanitize_port(int $port): int
{
    if ($port < 1 || $port > 65535) {
        return ZAPRET2_DIVERT_PORT;
    }
    return $port;
}

// ---------------------------------------------------------------------------
// Profile handling
// ---------------------------------------------------------------------------

/*
 * Built-in profile argument templates for dvtws2 Lua-based desync strategies.
 * These map to --lua-desync= arguments passed to dvtws2.
 * See also /usr/local/etc/zapret2/profiles.conf for documentation.
 */
function zapret2_get_profile_args(string $profile, array $cfg = []): array
{
    switch ($profile) {
        case 'safe':
            // Minimal impact: TCP stream splitting only
            return [
                '--lua-desync=multisplit',
            ];

        case 'default':
            // Balanced: multisplit + fake TLS with md5sig
            return [
                '--lua-desync=multisplit',
                '--lua-desync=fake:blob=fake_default_tls:tcp_md5',
            ];

        case 'multidisorder':
            // multidisorder with seqovl — effective on Rostelecom, Beeline, MTS
            return [
                '--lua-desync=multidisorder:pos=1,midsld:seqovl=1',
            ];

        case 'fake_disorder':
            // fake TLS + multidisorder — effective on some regional ISPs
            return [
                '--lua-desync=fake:blob=fake_default_tls:tcp_md5:repeats=11',
                '--lua-desync=multidisorder:pos=1,midsld',
            ];

        case 'wssize':
            // TCP window size reduction — works when fragment-based methods fail
            return [
                '--lua-desync=wssize:wsize=1:scale=6',
                '--lua-desync=multisplit:pos=midsld',
            ];

        case 'custom':
            // User-supplied raw arguments — used verbatim (validated for printable chars only)
            $raw = zapret2_sanitize_args($cfg['custom_args'] ?? '');
            return $raw !== '' ? [$raw] : [];

        default:
            return [];
    }
}

// ---------------------------------------------------------------------------
// Command builder
// ---------------------------------------------------------------------------

function zapret2_build_command(): string
{
    $cfg     = zapret2_get_config();
    $profile = $cfg['profile'] ?? 'default';
    $args    = zapret2_get_profile_args($profile, $cfg);

    $divertPort = zapret2_sanitize_port((int) ($cfg['divert_port'] ?? ZAPRET2_DIVERT_PORT));

    $parts = [escapeshellarg(ZAPRET2_DVTWS_BIN)];

    // Daemon mode
    $parts[] = '--daemon';
    $parts[] = '--pidfile=' . escapeshellarg(ZAPRET2_PID_FILE);

    // Divert socket port
    $parts[] = '--port ' . $divertPort;

    // Lua init files (required)
    $parts[] = '--lua-init=@' . escapeshellarg(ZAPRET2_LUA_DIR . '/zapret-lib.lua');
    $parts[] = '--lua-init=@' . escapeshellarg(ZAPRET2_LUA_DIR . '/zapret-antidpi.lua');

    // Append profile Lua desync arguments
    foreach ($args as $arg) {
        $parts[] = $arg;
    }

    // Debug logging
    if (($cfg['debug'] ?? '') === 'on') {
        $parts[] = '--debug';
    }

    return implode(' ', $parts);
}

function zapret2_build_command_dry(): string
{
    return zapret2_build_command();
}

// ---------------------------------------------------------------------------
// Firewall / kernel setup helpers
// ---------------------------------------------------------------------------

function zapret2_load_kernel_modules(): void
{
    mwexec('kldload ipfw 2>/dev/null; true');
    mwexec('kldload ipdivert 2>/dev/null; true');
}

function zapret2_set_ipfw_sysctls(): void
{
    mwexec('sysctl net.inet.ip.pfil.outbound=ipfw,pf 2>/dev/null; true');
    mwexec('sysctl net.inet.ip.pfil.inbound=ipfw,pf 2>/dev/null; true');
    mwexec('sysctl net.inet6.ip6.pfil.outbound=ipfw,pf 2>/dev/null; true');
    mwexec('sysctl net.inet6.ip6.pfil.inbound=ipfw,pf 2>/dev/null; true');
}

function zapret2_add_ipfw_rule(): void
{
    $cfg        = zapret2_get_config();
    $divertPort = zapret2_sanitize_port((int) ($cfg['divert_port'] ?? ZAPRET2_DIVERT_PORT));

    // Re-enable pf after ipfw load (required for pfSense 2.6.0+)
    mwexec('pfctl -d 2>/dev/null; pfctl -e 2>/dev/null; true');

    // Remove any existing rule 100 and add the divert rule
    mwexec('ipfw delete 100 2>/dev/null; true');
    mwexec('ipfw add 100 divert ' . $divertPort . ' tcp from any to any 80,443 out not diverted not sockarg');
}

function zapret2_remove_ipfw_rule(): void
{
    mwexec('ipfw delete 100 2>/dev/null; true');
}

// ---------------------------------------------------------------------------
// Service control
// ---------------------------------------------------------------------------

function zapret2_is_running(): bool
{
    if (!file_exists(ZAPRET2_PID_FILE)) {
        return false;
    }
    $pid = (int) trim(file_get_contents(ZAPRET2_PID_FILE));
    if ($pid <= 0) {
        return false;
    }
    // POSIX signal 0: check process exists without sending a signal
    return posix_kill($pid, 0);
}

function zapret2_get_pids(): array
{
    $pids = [];
    $out = shell_exec('pgrep -f ' . escapeshellarg('dvtws2') . ' 2>/dev/null');
    if ($out !== null) {
        foreach (preg_split('/\s+/', trim($out)) as $pid) {
            if (ctype_digit($pid) && (int)$pid > 0) {
                $pids['dvtws2'][] = (int) $pid;
            }
        }
    }
    return $pids;
}

function zapret2_start(): bool
{
    if (zapret2_is_running()) {
        zapret2_log('already running, skipping start');
        return true;
    }

    zapret2_log('loading kernel modules');
    zapret2_load_kernel_modules();

    zapret2_log('configuring IPFW sysctls');
    zapret2_set_ipfw_sysctls();

    zapret2_log('adding IPFW divert rule');
    zapret2_add_ipfw_rule();

    $cmd = zapret2_build_command();
    zapret2_log('starting dvtws2: ' . $cmd);

    $ret = mwexec($cmd . ' >> ' . escapeshellarg(ZAPRET2_LOG_FILE) . ' 2>&1');
    return $ret === 0;
}

function zapret2_stop(): bool
{
    if (!file_exists(ZAPRET2_PID_FILE)) {
        zapret2_log('no pid file found, nothing to stop');
        zapret2_remove_ipfw_rule();
        return true;
    }

    $pid = (int) trim(file_get_contents(ZAPRET2_PID_FILE));
    if ($pid <= 0) {
        @unlink(ZAPRET2_PID_FILE);
        zapret2_remove_ipfw_rule();
        return true;
    }

    if (posix_kill($pid, SIGTERM)) {
        zapret2_log('sent SIGTERM to dvtws2 pid ' . $pid);
        // Wait up to 3 seconds for clean exit
        for ($i = 0; $i < 6; $i++) {
            usleep(500000);
            if (!posix_kill($pid, 0)) {
                break;
            }
        }
        if (posix_kill($pid, 0)) {
            posix_kill($pid, SIGKILL);
            zapret2_log('sent SIGKILL to dvtws2 pid ' . $pid);
        }
    }

    @unlink(ZAPRET2_PID_FILE);
    zapret2_remove_ipfw_rule();
    return true;
}

function zapret2_restart(): bool
{
    zapret2_stop();
    sleep(1);
    return zapret2_start();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

function zapret2_get_status(): array
{
    $cfg     = zapret2_get_config();
    $running = zapret2_is_running();
    $pids    = zapret2_get_pids();

    // Read last 20 lines of log
    $logLines = [];
    if (file_exists(ZAPRET2_LOG_FILE)) {
        $logLines = array_slice(file(ZAPRET2_LOG_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES), -20);
    }

    return [
        'enabled'     => ($cfg['enabled'] ?? '') === 'on',
        'running'     => $running,
        'pids'        => $pids,
        'profile'     => $cfg['profile'] ?? 'default',
        'divert_port' => $cfg['divert_port'] ?? ZAPRET2_DIVERT_PORT,
        'log_tail'    => $logLines,
    ];
}

function zapret2_status_html(): string
{
    $s = zapret2_get_status();

    $enabledBadge = $s['enabled']
        ? '<span class="badge bg-success">Enabled</span>'
        : '<span class="badge bg-secondary">Disabled</span>';

    $runningBadge = $s['running']
        ? '<span class="badge bg-success">Running</span>'
        : '<span class="badge bg-danger">Stopped</span>';

    $pidStr = '';
    foreach ($s['pids'] as $proc => $pidList) {
        $pidStr .= htmlspecialchars($proc) . ': ' . implode(', ', array_map('htmlspecialchars', $pidList)) . '<br>';
    }
    if ($pidStr === '') {
        $pidStr = '<em>none</em>';
    }

    $logHtml = '';
    foreach (array_reverse($s['log_tail']) as $line) {
        $logHtml .= htmlspecialchars($line) . "\n";
    }

    $divertPort = htmlspecialchars((string) $s['divert_port']);

    return <<<HTML
<div class="panel panel-default" id="zapret2-status-panel">
  <div class="panel-heading"><h2 class="panel-title">Zapret2 Status</h2></div>
  <div class="panel-body" style="padding:15px 20px">
    <dl class="dl-horizontal">
      <dt>Status</dt>
      <dd>{$enabledBadge} {$runningBadge}</dd>
      <dt>Profile</dt>
      <dd>{$s['profile']}</dd>
      <dt>Divert Port</dt>
      <dd>{$divertPort}</dd>
      <dt>Processes</dt>
      <dd>{$pidStr}</dd>
    </dl>
    <h4>Recent log entries</h4>
    <pre style="max-height:200px;overflow-y:auto;font-size:12px;padding:8px 12px">{$logHtml}</pre>
  </div>
</div>
HTML;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

function zapret2_log(string $msg): void
{
    $line = '[' . date('Y-m-d H:i:s') . '] [zapret2] ' . $msg . "\n";
    file_put_contents(ZAPRET2_LOG_FILE, $line, FILE_APPEND);
}

// ---------------------------------------------------------------------------
// rc.conf.local management
// ---------------------------------------------------------------------------

function zapret2_set_rcconf(bool $enable): void
{
    $rcconf = '/etc/rc.conf.local';
    $key    = 'zapret2_enable';
    $val    = $enable ? 'YES' : 'NO';

    $lines = file_exists($rcconf) ? file($rcconf, FILE_IGNORE_NEW_LINES) : [];

    $found = false;
    foreach ($lines as &$line) {
        if (preg_match('/^\s*' . preg_quote($key, '/') . '\s*=/', $line)) {
            $line  = $key . '="' . $val . '"';
            $found = true;
            break;
        }
    }
    unset($line);

    if (!$found) {
        $lines[] = $key . '="' . $val . '"';
    }

    file_put_contents($rcconf, implode("\n", $lines) . "\n");
}

// ---------------------------------------------------------------------------
// Package hooks
// ---------------------------------------------------------------------------

function zapret2_install(): void
{
    // Create config directory
    if (!is_dir(ZAPRET2_CONF_DIR)) {
        mkdir(ZAPRET2_CONF_DIR, 0755, true);
    }

    // Create empty log file
    if (!file_exists(ZAPRET2_LOG_FILE)) {
        file_put_contents(ZAPRET2_LOG_FILE, '');
        chmod(ZAPRET2_LOG_FILE, 0640);
    }

    // Make rc.d script executable
    $rcd = '/usr/local/etc/rc.d/zapret2';
    if (file_exists($rcd)) {
        chmod($rcd, 0555);
    }

    // Verify required backend files are present
    if (!file_exists(ZAPRET2_DVTWS_BIN)) {
        zapret2_log('WARNING: dvtws2 binary not found at ' . ZAPRET2_DVTWS_BIN . ' — install zapret2 backend first');
    }
    if (!file_exists(ZAPRET2_LUA_DIR . '/zapret-lib.lua')) {
        zapret2_log('WARNING: zapret-lib.lua not found in ' . ZAPRET2_LUA_DIR . ' — install zapret2 backend first');
    }
    if (!file_exists(ZAPRET2_LUA_DIR . '/zapret-antidpi.lua')) {
        zapret2_log('WARNING: zapret-antidpi.lua not found in ' . ZAPRET2_LUA_DIR . ' — install zapret2 backend first');
    }

    zapret2_log('package installed');
}

function zapret2_deinstall(): void
{
    zapret2_stop();
    zapret2_set_rcconf(false);
    zapret2_log('package deinstalled');
}

function zapret2_resync(): void
{
    $cfg = zapret2_get_config();
    $enabled = ($cfg['enabled'] ?? '') === 'on';

    zapret2_set_rcconf($enabled);

    if ($enabled) {
        zapret2_restart();
    } else {
        zapret2_stop();
    }

    zapret2_log('resync: enabled=' . ($enabled ? 'yes' : 'no'));
}

// ---------------------------------------------------------------------------
// CLI entry point (when invoked directly as: php zapret2.inc <cmd>)
// ---------------------------------------------------------------------------

if (php_sapi_name() === 'cli' && isset($argv[0]) && basename($argv[0]) === 'zapret2.inc') {
    // Load pfSense environment
    if (file_exists('/etc/inc/globals.inc')) {
        require_once('/etc/inc/globals.inc');
        require_once('/etc/inc/config.inc');
        require_once('/etc/inc/functions.inc');
    }

    $cmd = $argv[1] ?? 'status';
    switch ($cmd) {
        case 'start':
            exit(zapret2_start() ? 0 : 1);
        case 'stop':
            exit(zapret2_stop() ? 0 : 1);
        case 'restart':
            exit(zapret2_restart() ? 0 : 1);
        case 'status':
            $s = zapret2_get_status();
            echo 'enabled: '      . ($s['enabled'] ? 'yes' : 'no') . "\n";
            echo 'running: '      . ($s['running'] ? 'yes' : 'no') . "\n";
            echo 'profile: '      . $s['profile'] . "\n";
            echo 'divert_port: '  . $s['divert_port'] . "\n";
            $allPids = [];
            foreach ($s['pids'] as $proc => $pidList) {
                $allPids[] = $proc . ':' . implode(',', $pidList);
            }
            echo 'pids: ' . (implode(' ', $allPids) ?: 'none') . "\n";
            exit(0);
        case 'configtest':
            $exitCode = 0;
            if (!file_exists(ZAPRET2_DVTWS_BIN)) {
                fwrite(STDERR, 'ERROR: dvtws2 not found at ' . ZAPRET2_DVTWS_BIN . "\n");
                $exitCode = 1;
            } elseif (!is_executable(ZAPRET2_DVTWS_BIN)) {
                fwrite(STDERR, 'ERROR: dvtws2 is not executable at ' . ZAPRET2_DVTWS_BIN . "\n");
                $exitCode = 1;
            }
            if (!file_exists(ZAPRET2_LUA_DIR . '/zapret-lib.lua')) {
                fwrite(STDERR, 'ERROR: zapret-lib.lua not found in ' . ZAPRET2_LUA_DIR . "\n");
                $exitCode = 1;
            }
            if (!file_exists(ZAPRET2_LUA_DIR . '/zapret-antidpi.lua')) {
                fwrite(STDERR, 'ERROR: zapret-antidpi.lua not found in ' . ZAPRET2_LUA_DIR . "\n");
                $exitCode = 1;
            }
            if ($exitCode !== 0) {
                exit($exitCode);
            }
            echo 'OK: ' . zapret2_build_command_dry() . "\n";
            exit(0);
        default:
            fwrite(STDERR, "Usage: zapret2.inc [start|stop|restart|status|configtest]\n");
            exit(1);
    }
}
