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
define('ZAPRET2_ACTIVE_FILE',  '/var/run/zapret2-active.json');
define('ZAPRET2_DIVERT_PORT',    990);
define('ZAPRET2_TAG',            'pfSense-pkg-zapret2');
define('ZAPRET2_DISCORD_HOSTLIST', '/usr/local/etc/zapret2/discord-hostlist.txt');
define('ZAPRET2_YOUTUBE_HOSTLIST', '/usr/local/etc/zapret2/youtube-hostlist.txt');

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

        case 'fake_aggressive':
            // Aggressive fake TLS with high repeat count + multidisorder — for stubborn DPI
            return [
                '--lua-desync=fake:blob=fake_default_tls:tcp_md5:repeats=15',
                '--lua-desync=multidisorder:pos=1,midsld',
            ];

        case 'syndata':
            // SYN packet with data — alternative when fragment-based methods are blocked
            return [
                '--lua-desync=syndata',
                '--lua-desync=multisplit:pos=host',
            ];

        case 'wssize_disorder':
            // Window size reduction combined with multidisorder — combo method
            return [
                '--lua-desync=wssize:wsize=1:scale=6',
                '--lua-desync=multidisorder:pos=1,midsld:seqovl=1',
            ];

        case 'tls_clone':
            // TLS Client Hello clone — mirrors the handshake to confuse stateful DPI
            return [
                '--lua-desync=tls_client_hello_clone',
                '--lua-desync=multisplit:pos=midsld',
            ];

        case 'hostfakesplit':
            // fake TLS with TCP MD5 fooling + hostfakesplit — ALT9-style; works where seqovl-based methods fail
            return [
                '--lua-desync=fake:blob=fake_default_tls:tcp_md5',
                '--lua-desync=hostfakesplit',
            ];

        case 'fake_multisplit_seqovl':
            // fake + multisplit with seqovl and TLS pattern — ALT11/ALT12 style
            return [
                '--lua-desync=fake:blob=fake_default_tls:tcp_md5:repeats=8',
                '--lua-desync=multisplit:pos=1:seqovl=681',
            ];

        case 'fake_ts':
            // fake with timestamp fooling (ts) instead of md5sig — lighter on some DPI systems
            return [
                '--lua-desync=fake:blob=fake_default_tls:ts:repeats=6',
                '--lua-desync=multisplit:pos=1',
            ];

        case 'fake_disorder_sni':
            // fake TLS with rnd+dupsid+sni=www.google.com + multidisorder — ALT3/FAKE TLS AUTO style
            return [
                '--lua-desync=fake:blob=fake_default_tls:tcp_md5:repeats=11:tls_mod=rnd,dupsid,sni=www.google.com',
                '--lua-desync=multidisorder:pos=1,midsld',
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
// Discord hostlist writer
// ---------------------------------------------------------------------------

function zapret2_write_discord_hostlist(): void
{
    $domains = [
        'discord.com',
        '*.discord.com',
        'discordapp.com',
        '*.discordapp.com',
        'discord.gg',
        'discord.media',
        '*.discord.media',
        'discordapp.net',
        '*.discordapp.net',
        'discordcdn.com',
        '*.discordcdn.com',
        'discord.new',
        'discord.co',
    ];
    $result = file_put_contents(ZAPRET2_DISCORD_HOSTLIST, implode("\n", $domains) . "\n");
    if ($result === false) {
        zapret2_log('ERROR: failed to write discord hostlist to ' . ZAPRET2_DISCORD_HOSTLIST);
        return;
    }
    chmod(ZAPRET2_DISCORD_HOSTLIST, 0644);
}

// ---------------------------------------------------------------------------
// YouTube hostlist writer
// ---------------------------------------------------------------------------

function zapret2_write_youtube_hostlist(): void
{
    $domains = [
        'youtube.com',
        '*.youtube.com',
        'youtu.be',
        'youtube-nocookie.com',
        '*.youtube-nocookie.com',
        'youtube-ui.l.google.com',
        'wide-youtube.l.google.com',
        'youtubekids.com',
        '*.youtubekids.com',
        'youtube.googleapis.com',
        'youtubei.googleapis.com',
        'youtubeembeddedplayer.googleapis.com',
        'yt-video-upload.l.google.com',
        'googlevideo.com',
        '*.googlevideo.com',
        'yt3.ggpht.com',
        'yt4.ggpht.com',
        'yt3.googleusercontent.com',
        'ytimg.com',
        '*.ytimg.com',
        'ytimg.l.google.com',
        'jnn-pa.googleapis.com',
        'play.google.com',
    ];
    $result = file_put_contents(ZAPRET2_YOUTUBE_HOSTLIST, implode("\n", $domains) . "\n");
    if ($result === false) {
        zapret2_log('ERROR: failed to write youtube hostlist to ' . ZAPRET2_YOUTUBE_HOSTLIST);
        return;
    }
    chmod(ZAPRET2_YOUTUBE_HOSTLIST, 0644);
}

// ---------------------------------------------------------------------------
// Alias → ipfw table resolver
// ---------------------------------------------------------------------------

function zapret2_resolve_alias_to_table(): bool
{
    if (!function_exists('filter_expand_alias_array')) {
        require_once('/etc/inc/filter.inc');
    }

    $cfg       = zapret2_get_config();
    $aliasName = trim($cfg['alias_name'] ?? '');

    if ($aliasName === '') {
        return false;
    }

    mwexec('ipfw table 1 flush 2>/dev/null; true');

    $entries = filter_expand_alias_array($aliasName);

    if (empty($entries)) {
        zapret2_log('WARNING: alias "' . $aliasName . '" empty or not found — falling back to "any"');
        return false;
    }

    $added = 0;
    foreach ($entries as $entry) {
        $entry = trim($entry);
        if (!preg_match('/^[0-9]{1,3}(\.[0-9]{1,3}){3}(\/[0-9]+)?$/', $entry)
            && !preg_match('/^[0-9a-fA-F:]+\/[0-9]+$/', $entry)
            && !preg_match('/^[0-9a-fA-F:]+$/', $entry)) {
            zapret2_log('alias entry "' . $entry . '" is not IP/CIDR, skipping');
            continue;
        }
        if (mwexec('ipfw table 1 add ' . escapeshellarg($entry) . ' 2>/dev/null') === 0) {
            $added++;
        }
    }

    if ($added === 0) {
        zapret2_log('WARNING: alias "' . $aliasName . '" has no valid IP/CIDR entries — falling back to "any"');
        return false;
    }

    zapret2_log('alias "' . $aliasName . '": ' . $added . ' entries loaded into ipfw table 1');
    return true;
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

    // For non-custom profiles: prepend hostlist args before desync args so they
    // apply to the single implicit profile. For custom profile: the user manages
    // hostlists inside their own args (with --new separators), so we skip them.
    if ($profile !== 'custom') {
        if (($cfg['discord_enabled'] ?? '') === 'on') {
            $parts[] = '--hostlist=' . escapeshellarg(ZAPRET2_DISCORD_HOSTLIST);
        }
        if (($cfg['youtube_enabled'] ?? '') === 'on') {
            $parts[] = '--hostlist=' . escapeshellarg(ZAPRET2_YOUTUBE_HOSTLIST);
        }
    }

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

// PF divert is opt-in: its early pass rules explicitly allow the selected traffic.
// pfSense invokes this package hook during every filter regeneration.
function zapret2_backend(?array $cfg = null): string
{
    $cfg = $cfg ?? zapret2_get_config();
    return ($cfg['firewall_backend'] ?? 'ipfw') === 'pf' ? 'pf' : 'ipfw';
}

function zapret2_pf_interfaces(): array
{
    global $config;
    $interfaces = [];
    foreach ($config['interfaces'] ?? [] as $name => $iface) {
        if ($name !== 'wan' && isset($iface['enable']) && !empty($iface['if'])) {
            $interfaces[$name] = $iface['descr'] ?? strtoupper($name);
        }
    }
    return $interfaces;
}

function zapret2_pf_rules(array $cfg): string
{
    global $config;
    if (PHP_OS_FAMILY === 'FreeBSD' && (int) php_uname('r') < 15) {
        throw new RuntimeException('PF divert requires FreeBSD 15 / pfSense CE 2.8.x or later.');
    }
    if (($cfg['pf_allow'] ?? '') !== 'on') {
        throw new RuntimeException('Confirm the PF traffic allowance before enabling PF divert.');
    }
    $selected = array_filter(explode(',', $cfg['pf_interfaces'] ?? ''));
    if (!$selected || array_diff($selected, array_keys(zapret2_pf_interfaces()))) {
        throw new RuntimeException('Select at least one enabled LAN/OPT interface for PF divert.');
    }
    $port = zapret2_sanitize_port((int) ($cfg['divert_port'] ?? ZAPRET2_DIVERT_PORT));
    $alias = $cfg['alias_name'] ?? '';
    $dst = '! (self)';
    if ($alias !== '' && ($cfg['youtube_enabled'] ?? '') !== 'on' && ($cfg['discord_enabled'] ?? '') !== 'on') {
        $valid = false;
        foreach ($config['aliases']['alias'] ?? [] as $entry) {
            if (($entry['name'] ?? '') === $alias && in_array($entry['type'] ?? '', ['host', 'network', 'urltable'], true)) {
                $valid = true;
            }
        }
        if (!$valid || !preg_match('/^[a-zA-Z0-9_]+$/D', $alias)) {
            throw new RuntimeException('PF destination alias is missing or invalid.');
        }
        $dst = '<' . $alias . '>';
    }
    $rules = '';
    foreach ($selected as $name) {
        if (!preg_match('/^[a-zA-Z0-9_]+$/D', $name)) {
            throw new RuntimeException('Invalid configured interface key.');
        }
        $iface = $config['interfaces'][$name]['if'];
        if (!preg_match('/^[a-zA-Z0-9_.]+$/D', $iface)) {
            throw new RuntimeException('Invalid PF interface name.');
        }
        // The outer anchor never matches router addresses, including alias mode.
        $rules .= 'anchor "zapret2_' . $name . '" in quick on ' . $iface . ' inet from (' . $iface . ':network) to ! (self) {' . "\n";
        $common = ' from (' . $iface . ':network) to ' . $dst;
        $end = ' divert-to 127.0.0.1 port ' . $port . ' keep state label "zapret2"' . "\n";
        $rules .= 'pass in quick inet proto tcp' . $common . ' port { 80, 443 }' . $end;
        if (($cfg['youtube_quic_enabled'] ?? '') === 'on' || ($cfg['discord_udp_enabled'] ?? '') === 'on') {
            $rules .= 'pass in quick inet proto udp' . $common . ' port 443' . $end;
        }
        if (($cfg['discord_udp_enabled'] ?? '') === 'on') {
            $rules .= 'pass in quick inet proto udp' . $common . ' port 50000:65535' . $end;
        }
        $rules .= "}\n";
    }
    return $rules;
}

function zapret2_active_config(): array
{
    return json_decode(@file_get_contents(ZAPRET2_ACTIVE_FILE) ?: '{}', true) ?: [];
}

function zapret2_generate_rules(string $type): string
{
    $cfg = zapret2_active_config();
    if ($type !== 'pfearly' || zapret2_backend($cfg) !== 'pf' || !zapret2_is_running()) {
        return '';
    }
    try {
        return zapret2_pf_rules($cfg);
    } catch (Throwable $e) {
        zapret2_log('PF rules not generated: ' . $e->getMessage());
        return '';
    }
}

function zapret2_reload_pf(): bool
{
    $expected = zapret2_generate_rules('pfearly') !== '';
    if (!zapret2_pf_enabled()) {
        // Do not let filter_configure_sync silently re-enable a disabled firewall.
        if ($expected) {
            zapret2_log('PF is disabled; enable the pfSense firewall before using PF divert');
            return false;
        }
        $anchors = shell_exec('/sbin/pfctl -s Anchors 2>/dev/null') ?? '';
        foreach (explode("\n", $anchors) as $anchor) {
            $anchor = trim($anchor);
            if (preg_match('/^zapret2_[a-zA-Z0-9_]+$/D', $anchor)
                && mwexec('/sbin/pfctl -a ' . escapeshellarg($anchor) . ' -F rules') !== 0) {
                return false;
            }
        }
        return true;
    }
    require_once('/etc/inc/filter.inc');
    require_once('/etc/inc/shaper.inc');
    require_once('/etc/inc/ipsec.inc');
    require_once('/etc/inc/vpn.inc');
    filter_configure_sync();
    // Verify the running ruleset, not just a successful generator invocation.
    $rules = shell_exec('/sbin/pfctl -a "*" -sr 2>/dev/null') ?? '';
    return (strpos($rules, 'label "zapret2"') !== false) === $expected;
}

function zapret2_pf_enabled(): bool
{
    return preg_match('/^Status:\s+Enabled\b/m', shell_exec('/sbin/pfctl -si 2>/dev/null') ?? '') === 1;
}

function zapret2_register_pf_hook(): void
{
    global $config;
    $packages = $config['installedpackages']['package'] ?? [];
    foreach ($packages as &$package) {
        if (($package['name'] ?? '') === 'zapret2') {
            $package['version'] = '1.1.0';
            $package['configurationfile'] = 'zapret2.xml';
            $package['include_file'] = '/usr/local/pkg/zapret2/includes/zapret2.inc';
            $package['filter_rule_function'] = 'zapret2_generate_rules';
            $config['installedpackages']['package'] = $packages;
            write_config('Zapret2: register PF filter hook');
            return;
        }
    }
    unset($package);
    $packages[] = [
        'name'                => 'zapret2',
        'version'             => '1.1.0',
        'configurationfile'   => 'zapret2.xml',
        'include_file'        => '/usr/local/pkg/zapret2/includes/zapret2.inc',
        'filter_rule_function' => 'zapret2_generate_rules',
    ];
    $config['installedpackages']['package'] = $packages;
    write_config('Zapret2: register PF filter hook');
}

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

function zapret2_reset_ipfw_sysctls(): void
{
    mwexec('sysctl net.inet.ip.pfil.outbound=pf 2>/dev/null; true');
    mwexec('sysctl net.inet.ip.pfil.inbound=pf 2>/dev/null; true');
    mwexec('sysctl net.inet6.ip6.pfil.outbound=pf 2>/dev/null; true');
    mwexec('sysctl net.inet6.ip6.pfil.inbound=pf 2>/dev/null; true');
}

function zapret2_add_ipfw_rule(): void
{
    $cfg           = zapret2_get_config();
    $divertPort    = zapret2_sanitize_port((int) ($cfg['divert_port'] ?? ZAPRET2_DIVERT_PORT));
    $aliasName     = trim($cfg['alias_name'] ?? '');
    $discordOn     = ($cfg['discord_enabled'] ?? '') === 'on';
    $discordUdpOn  = ($cfg['discord_udp_enabled'] ?? '') === 'on';
    $youtubeOn     = ($cfg['youtube_enabled'] ?? '') === 'on';
    $youtubeQuicOn = ($cfg['youtube_quic_enabled'] ?? '') === 'on';

    // Re-enable pf after ipfw load (required for pfSense 2.6.0+)
    mwexec('pfctl -d 2>/dev/null; pfctl -e 2>/dev/null; true');

    mwexec('ipfw delete 100 2>/dev/null; true');
    mwexec('ipfw delete 101 2>/dev/null; true');
    mwexec('ipfw delete 102 2>/dev/null; true');

    // Destination selector shared by TCP and UDP divert rules.
    // When any hostlist mode is on, use 'any' — dvtws2 filters by domain itself.
    // When both are off, optionally restrict to alias table(1). Applying the same
    // restriction to UDP prevents the QUIC fake from hitting unrelated UDP 443
    // traffic such as DNS-over-QUIC, which otherwise breaks name resolution.
    if ($discordOn || $youtubeOn) {
        $dst = 'any';
    } elseif ($aliasName !== '' && zapret2_resolve_alias_to_table()) {
        $dst = "'table(1)'";
    } else {
        $dst = 'any';
        if ($aliasName !== '') {
            mwexec('ipfw table 1 flush 2>/dev/null; true');
        }
    }

    mwexec('ipfw add 100 divert ' . $divertPort .
        ' tcp from any to ' . $dst . ' 80,443 out not diverted not sockarg');

    // UDP 443 (QUIC): add when Discord UDP or YouTube QUIC is enabled
    if ($discordUdpOn || $youtubeQuicOn) {
        mwexec('ipfw add 101 divert ' . $divertPort .
            ' udp from any to ' . $dst . ' 443 out not diverted not sockarg');
    }

    // Discord voice/video UDP ports (only when Discord UDP is on)
    if ($discordUdpOn) {
        mwexec('ipfw add 102 divert ' . $divertPort .
            ' udp from any to ' . $dst . ' 50000-65535 out not diverted not sockarg');
    }
}

function zapret2_remove_ipfw_rule(): void
{
    mwexec('ipfw delete 100 2>/dev/null; true');
    mwexec('ipfw delete 101 2>/dev/null; true');
    mwexec('ipfw delete 102 2>/dev/null; true');
    mwexec('ipfw table 1 flush 2>/dev/null; true');
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

    $cfg = zapret2_get_config();
    if (zapret2_backend($cfg) === 'pf') {
        if (!zapret2_pf_enabled()) {
            zapret2_log('PF is disabled; enable the pfSense firewall before using PF divert');
            return false;
        }
        try {
            zapret2_pf_rules($cfg);
        } catch (Throwable $e) {
            zapret2_log($e->getMessage());
            return false;
        }
        mwexec('kldload ipdivert 2>/dev/null');
    } else {
        zapret2_load_kernel_modules();
        zapret2_set_ipfw_sysctls();
    }

    $cfg = zapret2_get_config();
    if (($cfg['discord_enabled'] ?? '') === 'on') {
        zapret2_write_discord_hostlist();
        zapret2_log('discord hostlist written to ' . ZAPRET2_DISCORD_HOSTLIST);
    }
    if (($cfg['youtube_enabled'] ?? '') === 'on') {
        zapret2_write_youtube_hostlist();
        zapret2_log('youtube hostlist written to ' . ZAPRET2_YOUTUBE_HOSTLIST);
    }

    $cmd = zapret2_build_command();
    zapret2_log('starting dvtws2: ' . $cmd);

    $ret = mwexec($cmd . ' >> ' . escapeshellarg(ZAPRET2_LOG_FILE) . ' 2>&1');
    // The daemon parent can exit before its child has written the PID file.
    for ($i = 0; $ret === 0 && !zapret2_is_running() && $i < 30; $i++) {
        usleep(100000);
        clearstatcache(true, ZAPRET2_PID_FILE);
    }
    if ($ret !== 0 || !zapret2_is_running()) {
        zapret2_log('dvtws2 failed to start');
        return false;
    }
    file_put_contents(ZAPRET2_ACTIVE_FILE, json_encode($cfg), LOCK_EX);
    chmod(ZAPRET2_ACTIVE_FILE, 0600);
    if (zapret2_backend($cfg) === 'pf') {
        if (!zapret2_reload_pf()) {
            zapret2_log('Failed to activate PF divert rules');
            zapret2_stop();
            return false;
        }
    } else {
        zapret2_add_ipfw_rule();
    }
    return true;
}

function zapret2_stop(): bool
{
    $active = zapret2_active_config();
    $backend = zapret2_backend($active);
    $ipfwActive = zapret2_ipfw_rules_active();
    @unlink(ZAPRET2_ACTIVE_FILE);
    if ($backend === 'pf') {
        if (!zapret2_reload_pf()) {
            // Keep the listener alive if rules could not be removed.
            file_put_contents(ZAPRET2_ACTIVE_FILE, json_encode($active), LOCK_EX);
            zapret2_log('Failed to remove PF rules; service left running');
            return false;
        }
        mwexec('/sbin/pfctl -k label -k zapret2 2>/dev/null');
    }
    // An upgrade from a legacy IPFW installation may predate the active-state
    // file. Always remove package-owned divert rules before stopping the daemon.
    if ($ipfwActive && mwexec('kldstat -q -m ipfw') === 0) {
        zapret2_remove_ipfw_rule();
        zapret2_reset_ipfw_sysctls();
    }

    // Kill by PID file first
    if (file_exists(ZAPRET2_PID_FILE)) {
        $pid = (int) trim(file_get_contents(ZAPRET2_PID_FILE));
        if ($pid > 0 && posix_kill($pid, SIGTERM)) {
            zapret2_log('sent SIGTERM to dvtws2 pid ' . $pid);
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
    }

    // Kill any remaining dvtws2 processes not tracked by the PID file
    mwexec('pkill -x dvtws2 2>/dev/null; true');
    usleep(300000);
    mwexec('pkill -9 -x dvtws2 2>/dev/null; true');

    zapret2_log('stopped');
    return true;
}

function zapret2_ipfw_rules_active(): bool
{
    $rules = shell_exec('/sbin/ipfw list 2>/dev/null') ?? '';
    return preg_match('/^0*10[012]\s+divert\s+/m', $rules) === 1;
}

function zapret2_restart(): bool
{
    if (!zapret2_stop()) {
        return false;
    }
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
        'enabled'         => ($cfg['enabled'] ?? '') === 'on',
        'running'         => $running,
        'backend'         => zapret2_backend($running ? (zapret2_active_config() ?: $cfg) : $cfg),
        'pids'            => $pids,
        'profile'         => $cfg['profile'] ?? 'default',
        'divert_port'     => $cfg['divert_port'] ?? ZAPRET2_DIVERT_PORT,
        'alias_name'      => $cfg['alias_name'] ?? '',
        'discord_enabled' => ($cfg['discord_enabled'] ?? '') === 'on',
        'discord_udp'     => ($cfg['discord_udp_enabled'] ?? '') === 'on',
        'youtube_enabled' => ($cfg['youtube_enabled'] ?? '') === 'on',
        'youtube_quic'    => ($cfg['youtube_quic_enabled'] ?? '') === 'on',
        'log_tail'        => $logLines,
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

    $divertPort    = htmlspecialchars((string) $s['divert_port']);
    $aliasDisplay  = $s['alias_name'] !== '' ? htmlspecialchars($s['alias_name']) : '<em>none</em>';
    $discordDisplay = $s['discord_enabled']
        ? ('TCP ' . ($s['discord_udp'] ? '+ UDP' : 'only'))
        : '<em>off</em>';
    $youtubeDisplay = $s['youtube_enabled']
        ? ('TCP ' . ($s['youtube_quic'] ? '+ QUIC' : 'only'))
        : '<em>off</em>';

    return <<<HTML
<div class="panel panel-default" id="zapret2-status-panel">
  <div class="panel-heading"><h2 class="panel-title">Zapret2 Status</h2></div>
  <div class="panel-body" style="padding:15px 20px">
    <dl class="dl-horizontal">
      <dt>Status</dt>
      <dd>{$enabledBadge} {$runningBadge}</dd>
      <dt>Profile</dt>
      <dd>{$s['profile']}</dd>
      <dt>Firewall backend</dt>
      <dd>{$s['backend']}</dd>
      <dt>Divert Port</dt>
      <dd>{$divertPort}</dd>
      <dt>Alias (include)</dt>
      <dd>{$aliasDisplay}</dd>
      <dt>Discord</dt>
      <dd>{$discordDisplay}</dd>
      <dt>YouTube</dt>
      <dd>{$youtubeDisplay}</dd>
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

    zapret2_register_pf_hook();
    if (!file_exists('/var/log/zapret2-test.log')) {
        touch('/var/log/zapret2-test.log');
    }
    chmod('/var/log/zapret2-test.log', 0600);
    zapret2_log('package installed');
}

function zapret2_deinstall(): void
{
    if (!zapret2_stop()) {
        throw new RuntimeException('Cannot uninstall Zapret2 while its firewall rules are active.');
    }
    zapret2_set_rcconf(false);
    zapret2_log('package deinstalled');
}

function zapret2_resync(): bool
{
    $cfg = zapret2_get_config();
    $enabled = ($cfg['enabled'] ?? '') === 'on';

    zapret2_set_rcconf($enabled);

    if ($enabled) {
        $ok = zapret2_restart();
    } else {
        $ok = zapret2_stop();
    }

    zapret2_log('resync: enabled=' . ($enabled ? 'yes' : 'no'));
    return $ok;
}

// ---------------------------------------------------------------------------
// Profile auto-tester
// ---------------------------------------------------------------------------

// HTTP test targets — mirrors zapret-discord-youtube targets.txt
define('ZAPRET2_HTTP_TARGETS', serialize([
    'DiscordMain'          => 'https://discord.com',
    'DiscordGateway'       => 'https://gateway.discord.gg',
    'DiscordCDN'           => 'https://cdn.discordapp.com',
    'DiscordUpdates'       => 'https://updates.discord.com',
    'YouTubeWeb'           => 'https://www.youtube.com',
    'YouTubeShort'         => 'https://youtu.be',
    'YouTubeImage'         => 'https://i.ytimg.com',
    'YouTubeVideoRedirect' => 'https://redirector.googlevideo.com',
    'GoogleMain'           => 'https://www.google.com',
    'GoogleGstatic'        => 'https://www.gstatic.com',
    'CloudflareWeb'        => 'https://www.cloudflare.com',
    'CloudflareCDN'        => 'https://cdnjs.cloudflare.com',
]));

define('ZAPRET2_PING_TARGETS', serialize([
    'CloudflareDNS1111' => '1.1.1.1',
    'CloudflareDNS1001' => '1.0.0.1',
    'GoogleDNS8888'     => '8.8.8.8',
    'GoogleDNS8844'     => '8.8.4.4',
    'Quad9DNS9999'      => '9.9.9.9',
]));

// DPI suite URL — fetched at test time from hyperion-cs project
define('ZAPRET2_DPI_SUITE_URL', 'https://hyperion-cs.github.io/dpi-checkers/ru/tcp-16-20/suite.v2.json');

function zapret2_test_profile(string $profile, array $cfg, string $testMode): array
{
    global $config;

    $testCfg           = $cfg;
    $testCfg['profile'] = $profile;
    $config['installedpackages']['zapret2']['config'][0] = $testCfg;

    zapret2_stop();
    sleep(1);
    $started = zapret2_start();

    $results = ['started' => $started];

    if ($started) {
        sleep(2);
        if ($testMode === 'http') {
            $results['http']  = zapret2_run_http_test();
        } elseif ($testMode === 'dpi') {
            $results['dpi']   = zapret2_run_dpi_test();
        }
    }

    $config['installedpackages']['zapret2']['config'][0] = $cfg;

    return $results;
}

function zapret2_run_http_test(): array
{
    $httpTargets = unserialize(ZAPRET2_HTTP_TARGETS);
    $pingTargets = unserialize(ZAPRET2_PING_TARGETS);
    $results     = [];

    foreach ($httpTargets as $name => $url) {
        $results[$name] = [
            'url'   => $url,
            'http'  => zapret2_curl_check($url, ''),
            'tls12' => zapret2_curl_check($url, 'tls12'),
            'tls13' => zapret2_curl_check($url, 'tls13'),
            'ping'  => zapret2_ping_check(parse_url($url, PHP_URL_HOST)),
        ];
    }

    foreach ($pingTargets as $name => $ip) {
        $results[$name] = [
            'url'  => $ip,
            'ping' => zapret2_ping_check($ip),
        ];
    }

    return $results;
}

function zapret2_curl_check(string $url, string $tlsMode): array
{
    $timeout  = 5;
    $baseArgs = '-s -I -o /dev/null -w "%{http_code}" --max-time ' . $timeout
              . ' --connect-timeout ' . $timeout;

    $tlsArgs = match ($tlsMode) {
        'tls12' => ' --tlsv1.2 --tls-max 1.2',
        'tls13' => ' --tlsv1.3 --tls-max 1.3',
        default => ' --http1.1',
    };

    $cmd    = 'curl ' . $baseArgs . $tlsArgs . ' ' . escapeshellarg($url) . ' 2>&1';
    $output = [];
    $retval = 0;
    exec($cmd, $output, $retval);

    $raw    = trim(implode('', $output));
    $code   = (int) preg_replace('/\D.*/', '', $raw);
    $stderr = preg_replace('/^\d+/', '', $raw);

    if (stripos($stderr, 'does not support') !== false
        || stripos($stderr, 'not supported') !== false
        || stripos($stderr, 'unsupported') !== false
        || $retval === 35) {
        return ['status' => 'UNSUP', 'code' => 0];
    }

    if ($retval === 0 && $code >= 100 && $code < 600) {
        return ['status' => 'OK', 'code' => $code];
    }

    return ['status' => 'FAIL', 'code' => $code];
}

function zapret2_ping_check(string $host): array
{
    $output = [];
    $retval = 0;
    exec('ping -c 1 -W 2000 ' . escapeshellarg($host) . ' 2>/dev/null', $output, $retval);

    if ($retval !== 0) {
        return ['ok' => false, 'ms' => null];
    }

    foreach ($output as $line) {
        if (preg_match('/time[=<]([\d.]+)\s*ms/i', $line, $m)) {
            return ['ok' => true, 'ms' => (int) round((float) $m[1])];
        }
    }

    return ['ok' => false, 'ms' => null];
}

function zapret2_run_dpi_test(): array
{
    // Fetch test suite from hyperion-cs
    $suiteJson = @file_get_contents(ZAPRET2_DPI_SUITE_URL);
    if ($suiteJson === false) {
        return ['error' => 'Failed to fetch DPI suite from ' . ZAPRET2_DPI_SUITE_URL];
    }

    $suite = json_decode($suiteJson, true);
    if (!is_array($suite)) {
        return ['error' => 'Invalid DPI suite JSON'];
    }

    // Write 64KB random payload to temp file
    $payloadFile = tempnam('/tmp', 'zapret2_dpi_');
    $payload     = random_bytes(65536);
    file_put_contents($payloadFile, $payload);

    $results  = [];
    $timeout  = 5;

    foreach (array_slice($suite, 0, 20) as $target) {
        $host     = $target['host'] ?? '';
        $id       = $target['id']   ?? $host;
        $provider = ($target['country'] ?? '') . ' ' . ($target['provider'] ?? '');
        if ($host === '') {
            continue;
        }

        $row = ['host' => $host, 'label' => trim($provider . ' ' . $id), 'checks' => []];

        foreach (['http' => '--http1.1', 'tls12' => '--tlsv1.2 --tls-max 1.2', 'tls13' => '--tlsv1.3 --tls-max 1.3'] as $mode => $tlsArg) {
            $cmd    = 'curl -s -X POST --data-binary @' . escapeshellarg($payloadFile)
                    . ' -w "%{http_code} %{size_upload} %{size_download} %{time_total}"'
                    . ' -o /dev/null -m ' . $timeout . ' ' . $tlsArg
                    . ' ' . escapeshellarg('https://' . $host) . ' 2>/dev/null';
            $out    = [];
            $retval = 0;
            exec($cmd, $out, $retval);

            $parts    = preg_split('/\s+/', trim(implode('', $out)));
            $code     = (int) ($parts[0] ?? 0);
            $upBytes  = (int) ($parts[1] ?? 0);
            $downBytes = (int) ($parts[2] ?? 0);
            $timeSec  = (float) str_replace(',', '.', $parts[3] ?? '0');

            // DPI freeze pattern: sent data, got nothing, timed out
            $frozen = $upBytes > 1000 && $downBytes === 0 && $timeSec >= ($timeout - 0.5) && $retval !== 0;

            if ($retval === 35 || $retval === 4) {
                $status = 'UNSUP';
            } elseif ($frozen) {
                $status = 'BLOCKED';
            } elseif ($retval === 0 && $code >= 100) {
                $status = 'OK';
            } else {
                $status = 'FAIL';
            }

            $row['checks'][$mode] = [
                'status'    => $status,
                'code'      => $code,
                'up_kb'     => round($upBytes / 1024, 1),
                'down_kb'   => round($downBytes / 1024, 1),
                'time_ms'   => (int) round($timeSec * 1000),
            ];
        }

        $results[] = $row;
    }

    @unlink($payloadFile);

    return $results;
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
            echo 'backend: ' . $s['backend'] . "\n";
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
