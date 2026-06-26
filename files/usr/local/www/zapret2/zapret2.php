<?php
/*
 * zapret2.php — pfSense GUI page for Zapret2 DPI bypass
 * Services → Zapret2
 */

##|+PRIV
##|*IDENT=page-services-zapret2
##|*NAME=Services: Zapret2
##|*DESCR=Allow access to the 'Services: Zapret2' page.
##|*MATCH=zapret2.php*
##|-PRIV

require_once('functions.inc');
require_once('guiconfig.inc');
require_once('/usr/local/pkg/zapret2/includes/zapret2.inc');

$pconfig = zapret2_get_config();

$input_errors = [];
$save_success = false;

// Build list of Host/Network/URL pfSense aliases for the dropdown
$available_aliases = ['' => gettext('— None (all TCP 80/443 traffic) —')];
global $config;
if (is_array($config['aliases']['alias'] ?? null)) {
    foreach ($config['aliases']['alias'] as $a) {
        $aname = $a['name'] ?? '';
        $atype = $a['type'] ?? '';
        $adesc = $a['descr'] ?? '';
        if (in_array($atype, ['host', 'network', 'urltable'], true) && $aname !== '') {
            $label = $aname . ' (' . $atype . ')';
            if ($adesc !== '') {
                $label .= ' — ' . htmlspecialchars($adesc);
            }
            $available_aliases[$aname] = $label;
        }
    }
}

// Available profiles
$available_profiles = [
    'safe'            => 'Safe — multisplit only',
    'default'         => 'Default — multisplit + fake TLS (recommended)',
    'multidisorder'   => 'Multidisorder + seqovl — Rostelecom, Beeline, MTS',
    'fake_disorder'   => 'Fake TLS + multidisorder — regional ISPs',
    'wssize'          => 'Window size reduction — when fragment methods fail',
    'fake_aggressive' => 'Fake Aggressive — fake TLS ×15 + multidisorder (stubborn DPI)',
    'syndata'         => 'SYN Data — alternative when fragmentation is blocked',
    'wssize_disorder' => 'WSize + Disorder — window size combo with multidisorder',
    'tls_clone'       => 'TLS Clone — mirrors Client Hello to confuse stateful DPI',
    'custom'          => 'Custom — specify arguments manually',
];

// Handle POST actions
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $act = htmlspecialchars($_POST['act'] ?? '');

    if ($act === 'save' || $act === 'apply') {
        // Validate profile
        $profile = trim($_POST['profile'] ?? 'default');
        if (!array_key_exists($profile, $available_profiles)) {
            $input_errors[] = gettext('Invalid profile selected.');
        }

        // Sanitize custom args (advanced field)
        $custom_args = zapret2_sanitize_args($_POST['custom_args'] ?? '');

        // Validate and sanitize divert port
        $raw_port    = (int) ($_POST['divert_port'] ?? ZAPRET2_DIVERT_PORT);
        $divert_port = zapret2_sanitize_port($raw_port);
        if ($raw_port !== $divert_port) {
            $input_errors[] = gettext('Divert port must be an integer between 1 and 65535.');
        }

        // Sanitize alias name and traffic filtering options
        $alias_name           = preg_replace('/[^a-zA-Z0-9_]/', '', trim($_POST['alias_name'] ?? ''));
        $discord_enabled      = !empty($_POST['discord_enabled'])      ? 'on' : '';
        $discord_udp_enabled  = !empty($_POST['discord_udp_enabled'])  ? 'on' : '';
        $youtube_enabled      = !empty($_POST['youtube_enabled'])      ? 'on' : '';
        $youtube_quic_enabled = !empty($_POST['youtube_quic_enabled']) ? 'on' : '';
        if ($discord_enabled !== 'on') {
            $discord_udp_enabled = '';
        }
        if ($youtube_enabled !== 'on') {
            $youtube_quic_enabled = '';
        }

        if (empty($input_errors)) {
            $new_cfg = [
                'enabled'              => !empty($_POST['enabled']) ? 'on' : '',
                'profile'              => $profile,
                'divert_port'          => $divert_port,
                'custom_args'          => $custom_args,
                'debug'                => !empty($_POST['debug']) ? 'on' : '',
                'alias_name'           => $alias_name,
                'discord_enabled'      => $discord_enabled,
                'discord_udp_enabled'  => $discord_udp_enabled,
                'youtube_enabled'      => $youtube_enabled,
                'youtube_quic_enabled' => $youtube_quic_enabled,
            ];

            zapret2_save_config($new_cfg);
            $pconfig      = $new_cfg;
            $save_success = true;

            if ($act === 'apply') {
                zapret2_resync();
            }
        }
    } elseif ($act === 'start') {
        zapret2_start();
    } elseif ($act === 'stop') {
        zapret2_stop();
    } elseif ($act === 'restart') {
        zapret2_restart();
    } elseif ($act === 'healthcheck_ajax') {
        // Called via AJAX — run health check synchronously and return output as JSON
        header('Content-Type: application/json');
        $output = [];
        $retval = 0;
        exec('/bin/sh ' . escapeshellarg('/usr/local/share/zapret2/healthcheck.sh') . ' 2>&1', $output, $retval);
        echo json_encode([
            'output' => implode("\n", $output),
            'passed' => $retval === 0,
        ]);
        exit;
    } elseif ($act === 'start_test_ajax') {
        header('Content-Type: application/json');

        $validProfiles  = array_keys($available_profiles);
        $rawProfiles    = (array) ($_POST['profiles'] ?? []);
        $profilesToTest = array_values(array_filter(
            $rawProfiles,
            fn($p) => in_array($p, $validProfiles, true) && $p !== 'custom'
        ));
        $testMode = in_array($_POST['test_mode'] ?? '', ['http', 'dpi'], true)
            ? $_POST['test_mode']
            : 'http';

        if (empty($profilesToTest)) {
            echo json_encode(['error' => 'No valid profiles selected.']);
            exit;
        }

        $taskId   = bin2hex(random_bytes(8));
        $taskFile = '/tmp/zapret2_task_' . $taskId . '.json';

        $runner   = __DIR__ . '/zapret2_test_runner.php';
        $phpBin   = '/usr/local/bin/php';
        $cmd      = $phpBin . ' ' . escapeshellarg($runner)
                  . ' ' . escapeshellarg($taskId)
                  . ' ' . escapeshellarg($testMode)
                  . ' ' . escapeshellarg(json_encode($profilesToTest))
                  . ' > /dev/null 2>&1 &';

        // Write initial state so poll sees the task immediately
        file_put_contents($taskFile, json_encode([
            'task_id'    => $taskId,
            'status'     => 'running',
            'mode'       => $testMode,
            'profiles'   => $profilesToTest,
            'current'    => null,
            'results'    => [],
            'started_at' => time(),
            'updated_at' => time(),
        ]));
        chmod($taskFile, 0600);

        exec($cmd);

        echo json_encode(['task_id' => $taskId]);
        exit;

    } elseif ($act === 'stop_test_ajax') {
        header('Content-Type: application/json');

        $taskId   = preg_replace('/[^a-f0-9]/', '', $_POST['task_id'] ?? '');
        $taskFile = '/tmp/zapret2_task_' . $taskId . '.json';

        if ($taskId === '' || !file_exists($taskFile)) {
            echo json_encode(['error' => 'Task not found.']);
            exit;
        }

        $state = json_decode(file_get_contents($taskFile), true) ?? [];
        if (($state['status'] ?? '') === 'running') {
            $state['status']     = 'cancelled';
            $state['updated_at'] = time();
            file_put_contents($taskFile, json_encode($state));
        }

        echo json_encode(['ok' => true]);
        exit;

    } elseif ($act === 'poll_test_ajax') {
        header('Content-Type: application/json');

        $taskId   = preg_replace('/[^a-f0-9]/', '', $_POST['task_id'] ?? '');
        $taskFile = '/tmp/zapret2_task_' . $taskId . '.json';

        if ($taskId === '' || !file_exists($taskFile)) {
            echo json_encode(['error' => 'Task not found.']);
            exit;
        }

        $state = json_decode(file_get_contents($taskFile), true) ?? [];

        // Clean up finished tasks older than 30 min
        if (($state['status'] ?? '') === 'done'
            && (time() - ($state['updated_at'] ?? 0)) > 1800) {
            @unlink($taskFile);
        }

        echo json_encode($state);
        exit;
    }
}

// Re-read config after any write operations
$pconfig = zapret2_get_config();

$current_divert_port = (int) ($pconfig['divert_port'] ?? ZAPRET2_DIVERT_PORT);

$pgtitle = [gettext('Services'), gettext('Zapret2')];
$pglinks  = ['', '@self'];

include('head.inc');
?>

<script type="text/javascript">
//<![CDATA[
function toggleCustomProfile() {
    var profile = document.getElementById('profile').value;
    var el = document.getElementById('custom_args');
    if (el) {
        var row = el.closest('.form-group');
        if (row) { row.style.display = (profile === 'custom') ? '' : 'none'; }
    }
}

function toggleDiscordUdp() {
    var tcp = document.getElementById('discord_enabled');
    var udp = document.getElementById('discord_udp_enabled');
    if (!tcp || !udp) { return; }
    udp.disabled = !tcp.checked;
    var row = udp.closest('.form-group');
    if (row) { row.style.opacity = tcp.checked ? '1' : '0.5'; }
}

function toggleYoutubeQuic() {
    var tcp  = document.getElementById('youtube_enabled');
    var quic = document.getElementById('youtube_quic_enabled');
    if (!tcp || !quic) { return; }
    quic.disabled = !tcp.checked;
    var row = quic.closest('.form-group');
    if (row) { row.style.opacity = tcp.checked ? '1' : '0.5'; }
}

function submitAction(act) {
    document.getElementById('hidden_act').value = act;
    document.getElementById('zapret2-form').submit();
}

function runHealthCheck() {
    var btn = document.getElementById('btn-healthcheck');
    var out = document.getElementById('healthcheck-output');
    btn.disabled = true;
    btn.innerHTML = '<i class="fa fa-spinner fa-spin" style="margin-right:5px"></i>Running...';
    out.style.display = 'block';
    out.className = 'alert alert-info';
    out.textContent = 'Running health check...';

    var form = new FormData();
    form.append('act', 'healthcheck_ajax');
    // Include pfSense CSRF token so the POST is accepted
    if (typeof csrfMagicName !== 'undefined' && typeof csrfMagicToken !== 'undefined') {
        form.append(csrfMagicName, csrfMagicToken);
    }
    fetch(window.location.pathname, { method: 'POST', body: form })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            out.className = data.passed ? 'alert alert-success' : 'alert alert-danger';
            out.textContent = data.output;
        })
        .catch(function(e) {
            out.className = 'alert alert-danger';
            out.textContent = 'Error: ' + e;
        })
        .finally(function() {
            btn.disabled = false;
            btn.innerHTML = '<i class="fa fa-stethoscope icon-embed-btn"></i> <?= gettext('Health Check') ?>';
        });
}

var z2ActiveTaskId   = null;
var z2PollInterval   = null;
var z2PollTaskFile   = 'zapret2_active_task';

function statusBadge(s) {
    var colors = { OK: '#5cb85c', FAIL: '#d9534f', UNSUP: '#777', BLOCKED: '#e67e22' };
    return '<span class="badge" style="background:' + (colors[s] || '#777') + ';font-size:11px">' + s + '</span>';
}

function z2CsrfForm() {
    var form = new FormData();
    if (typeof csrfMagicName !== 'undefined' && typeof csrfMagicToken !== 'undefined') {
        form.append(csrfMagicName, csrfMagicToken);
    }
    return form;
}

function runProfileTest(mode) {
    var out      = document.getElementById('test-profiles-output');
    var checkboxes = document.querySelectorAll('.profile-test-check:checked');
    var profiles = [];
    checkboxes.forEach(function(cb) { profiles.push(cb.value); });

    if (profiles.length === 0) {
        out.innerHTML = '<div class="alert alert-warning">Select at least one profile to test.</div>';
        return;
    }

    var form = z2CsrfForm();
    form.append('act', 'start_test_ajax');
    form.append('test_mode', mode);
    profiles.forEach(function(p) { form.append('profiles[]', p); });

    z2SetBusy(mode);
    out.innerHTML = '<div class="alert alert-info">'
        + (mode === 'http' ? 'Starting HTTP/TLS test...' : 'Starting DPI test...')
        + ' <small>You can leave this page — test will continue in background.</small></div>';

    fetch(window.location.pathname, { method: 'POST', body: form })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.error) {
                out.innerHTML = '<div class="alert alert-danger">' + data.error + '</div>';
                z2SetIdle();
                return;
            }
            var sb = document.getElementById('btn-stop-test');
            if (sb) { sb.style.display = ''; }
            z2StartPolling(data.task_id, mode);
            localStorage.setItem(z2PollTaskFile, JSON.stringify({ task_id: data.task_id, mode: mode }));
        })
        .catch(function(e) {
            out.innerHTML = '<div class="alert alert-danger">Request error: ' + e + '</div>';
            z2SetIdle();
        });
}

function z2StartPolling(taskId, mode) {
    z2ActiveTaskId = taskId;
    if (z2PollInterval) { clearInterval(z2PollInterval); }
    z2PollInterval = setInterval(function() { z2Poll(taskId, mode); }, 3000);
    z2Poll(taskId, mode);
}

function z2Poll(taskId, mode) {
    var form = z2CsrfForm();
    form.append('act', 'poll_test_ajax');
    form.append('task_id', taskId);

    fetch(window.location.pathname, { method: 'POST', body: form })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.error) {
                z2StopPolling();
                document.getElementById('test-profiles-output').innerHTML =
                    '<div class="alert alert-danger">' + data.error + '</div>';
                return;
            }
            z2RenderProgress(data);
            if (data.status === 'done' || data.status === 'cancelled') {
                z2StopPolling();
                localStorage.removeItem(z2PollTaskFile);
            }
        })
        .catch(function() {});
}

function stopTest() {
    if (!z2ActiveTaskId) { return; }
    var btn  = document.getElementById('btn-stop-test');
    if (btn) { btn.disabled = true; btn.textContent = 'Stopping...'; }

    var taskId = z2ActiveTaskId;
    var form = z2CsrfForm();
    form.append('act', 'stop_test_ajax');
    form.append('task_id', taskId);

    fetch(window.location.pathname, { method: 'POST', body: form })
        .then(function(r) { return r.json(); })
        .then(function() {
            localStorage.removeItem(z2PollTaskFile);
            z2Poll(taskId, null);
        })
        .catch(function() {});
}

function z2StopPolling() {
    if (z2PollInterval) { clearInterval(z2PollInterval); z2PollInterval = null; }
    z2ActiveTaskId = null;
    z2SetIdle();
    var btn = document.getElementById('btn-stop-test');
    if (btn) { btn.style.display = 'none'; }
}

function z2RenderProgress(data) {
    var out  = document.getElementById('test-profiles-output');
    var done = data.status === 'done';
    var mode = data.mode || 'http';

    var profiles  = data.profiles  || [];
    var results   = data.results   || {};
    var current   = data.current;

    var elapsed = data.started_at ? Math.round((Date.now() / 1000) - data.started_at) : 0;

    var cancelled = data.status === 'cancelled';
    var html = '';

    // Progress bar
    var total    = profiles.length;
    var finished = Object.keys(results).length;
    if (current && !results[current]) { finished += 0.5; }
    var pct = total > 0 ? Math.min(100, Math.round(finished / total * 100)) : 0;

    html += '<div style="margin-bottom:12px;display:flex;align-items:center;gap:12px">';
    html += '<div style="flex:1">';
    if (!done && !cancelled) {
        html += '<div style="font-size:13px;margin-bottom:6px">'
             + '<i class="fa fa-spinner fa-spin" style="margin-right:5px"></i>'
             + 'Testing profile <strong>' + (current || '...') + '</strong>'
             + ' &nbsp;·&nbsp; ' + elapsed + 's elapsed</div>';
    } else if (cancelled) {
        html += '<div style="font-size:13px;margin-bottom:6px;color:#e67e22">'
             + '<i class="fa fa-stop" style="margin-right:5px"></i>Test stopped. '
             + Object.keys(results).length + ' of ' + total + ' profiles tested.</div>';
    } else {
        html += '<div style="font-size:13px;margin-bottom:6px;color:#27ae60">'
             + '<i class="fa fa-check" style="margin-right:5px"></i>All tests finished.</div>';
    }
    html += '<div class="progress" style="height:8px;margin-bottom:0">'
         + '<div class="progress-bar' + (cancelled ? ' progress-bar-warning' : '') + '" style="width:' + pct + '%"></div></div>';
    html += '</div>';

    // Show/hide stop button
    var stopBtn = document.getElementById('btn-stop-test');
    if (stopBtn) { stopBtn.style.display = (!done && !cancelled) ? '' : 'none'; }

    html += '</div>';

    // Results per profile
    if (mode === 'http') {
        html += renderHttpResults(results, profiles, current);
    } else {
        html += renderDpiResults(results, profiles, current);
    }

    out.innerHTML = html;
}

function z2SetBusy(mode) {
    document.querySelectorAll('.btn-run-test').forEach(function(b) { b.disabled = true; });
    var btn = document.getElementById('btn-test-' + mode);
    if (btn) {
        btn.innerHTML = '<i class="fa fa-spinner fa-spin" style="margin-right:5px"></i>'
                      + (mode === 'http' ? 'HTTP test running...' : 'DPI test running...');
    }
}

function z2SetIdle() {
    document.querySelectorAll('.btn-run-test').forEach(function(b) { b.disabled = false; });
    var bhttp = document.getElementById('btn-test-http');
    var bdpi  = document.getElementById('btn-test-dpi');
    if (bhttp) bhttp.innerHTML = '<i class="fa fa-globe icon-embed-btn"></i> <?= gettext('HTTP / Ping Test') ?>';
    if (bdpi)  bdpi.innerHTML  = '<i class="fa fa-shield icon-embed-btn"></i> <?= gettext('DPI Test') ?>';
}

function renderHttpResults(results, allProfiles, currentProfile) {
    var bestProfile = null, bestOk = -1;
    var html = '';

    (allProfiles || Object.keys(results)).forEach(function(profile) {
        var r = results[profile];
        html += '<h5 style="margin:16px 0 6px;font-weight:bold">Profile: ' + profile;
        if (!r && profile === currentProfile) {
            html += ' <i class="fa fa-spinner fa-spin" style="font-size:12px;color:#888"></i>';
        }
        html += '</h5>';

        if (!r) { return; }

        if (!r.started) {
            html += '<div class="alert alert-danger" style="padding:6px 12px">Failed to start dvtws2.</div>';
            return;
        }

        var targets = r.http || {};
        var okCount = 0, total = 0;

        html += '<table class="table table-condensed table-bordered" style="font-size:12px;margin-bottom:4px">';
        html += '<thead style="background:#f5f5f5"><tr>'
             + '<th style="width:170px">Target</th>'
             + '<th>HTTP</th><th>TLS 1.2</th><th>TLS 1.3</th><th>Ping</th>'
             + '</tr></thead><tbody>';

        Object.keys(targets).forEach(function(name) {
            var t = targets[name];
            html += '<tr><td style="font-weight:500">' + name + '</td>';
            if (t.http) {
                [['http','HTTP'],['tls12','TLS1.2'],['tls13','TLS1.3']].forEach(function(pair) {
                    var c = t[pair[0]];
                    if (!c) { html += '<td>—</td>'; return; }
                    if (c.status === 'OK') { okCount++; }
                    total++;
                    var code = c.code ? ' <small style="color:#999">'+c.code+'</small>' : '';
                    html += '<td>' + statusBadge(c.status) + code + '</td>';
                });
            } else {
                html += '<td colspan="3" style="color:#999;font-style:italic">ping only</td>';
            }
            var p = t.ping;
            html += p && p.ok
                ? '<td><span style="color:#27ae60">&#9679;</span> ' + p.ms + ' ms</td>'
                : (p ? '<td><span style="color:#e74c3c">&#9679;</span> Timeout</td>' : '<td>—</td>');
            html += '</tr>';
        });

        html += '</tbody></table>';
        html += '<div style="font-size:12px;color:#555;margin-bottom:4px">HTTP OK: ' + okCount + ' / ' + total + '</div>';
        if (okCount > bestOk) { bestOk = okCount; bestProfile = profile; }
    });

    if (bestProfile && !currentProfile) {
        html += '<div class="alert alert-success" style="margin-top:12px">'
             + 'Best profile: <strong>' + bestProfile + '</strong> (' + bestOk + ' OK). '
             + '<a href="#" onclick="document.getElementById(\'profile\').value=\''
             + bestProfile + '\';toggleCustomProfile();return false;">Apply</a></div>';
    }
    return html;
}

function renderDpiResults(results, allProfiles, currentProfile) {
    var bestProfile = null, bestOk = -1;
    var html = '';

    (allProfiles || Object.keys(results)).forEach(function(profile) {
        var r = results[profile];
        html += '<h5 style="margin:16px 0 6px;font-weight:bold">Profile: ' + profile;
        if (!r && profile === currentProfile) {
            html += ' <i class="fa fa-spinner fa-spin" style="font-size:12px;color:#888"></i>';
        }
        html += '</h5>';

        if (!r) { return; }

        if (!r.started) {
            html += '<div class="alert alert-danger" style="padding:6px 12px">Failed to start dvtws2.</div>';
            return;
        }

        var targets = r.dpi || [];
        if (targets.error) {
            html += '<div class="alert alert-danger" style="padding:6px 12px">' + targets.error + '</div>';
            return;
        }

        var okCount = 0;
        html += '<table class="table table-condensed table-bordered" style="font-size:12px;margin-bottom:4px">';
        html += '<thead style="background:#f5f5f5"><tr>'
             + '<th>Server</th><th>HTTP</th><th>TLS 1.2</th><th>TLS 1.3</th>'
             + '</tr></thead><tbody>';

        targets.forEach(function(t) {
            html += '<tr><td style="font-size:11px">' + t.label + '</td>';
            ['http','tls12','tls13'].forEach(function(m) {
                var c = t.checks[m] || {};
                if (c.status === 'OK') { okCount++; }
                var detail = (c.status === 'OK' || c.status === 'FAIL') && c.time_ms
                    ? ' <small style="color:#999">' + c.up_kb + '↑ ' + c.down_kb + '↓ ' + c.time_ms + 'ms</small>'
                    : '';
                html += '<td>' + statusBadge(c.status || '?') + detail + '</td>';
            });
            html += '</tr>';
        });

        html += '</tbody></table>';
        html += '<div style="font-size:12px;color:#555;margin-bottom:4px">DPI OK: ' + okCount + '</div>';
        if (okCount > bestOk) { bestOk = okCount; bestProfile = profile; }
    });

    if (bestProfile && !currentProfile) {
        html += '<div class="alert alert-success" style="margin-top:12px">'
             + 'Best profile: <strong>' + bestProfile + '</strong> (' + bestOk + ' servers passed). '
             + '<a href="#" onclick="document.getElementById(\'profile\').value=\''
             + bestProfile + '\';toggleCustomProfile();return false;">Apply</a></div>';
    }
    return html;
}

document.addEventListener('DOMContentLoaded', function() {
    toggleCustomProfile();
    var profileSel = document.getElementById('profile');
    if (profileSel) { profileSel.addEventListener('change', toggleCustomProfile); }
    toggleDiscordUdp();
    var dc = document.getElementById('discord_enabled');
    if (dc) { dc.addEventListener('change', toggleDiscordUdp); }
    toggleYoutubeQuic();
    var yt = document.getElementById('youtube_enabled');
    if (yt) { yt.addEventListener('change', toggleYoutubeQuic); }

    // Resume polling if a test was running before page reload
    try {
        var saved = localStorage.getItem(z2PollTaskFile);
        if (saved) {
            var t = JSON.parse(saved);
            if (t && t.task_id && t.mode) {
                var out = document.getElementById('test-profiles-output');
                out.innerHTML = '<div class="alert alert-info">'
                    + '<i class="fa fa-spinner fa-spin" style="margin-right:5px"></i>'
                    + 'Test is running in background (task ' + t.task_id + '). Resuming updates...</div>';
                z2SetBusy(t.mode);
                var sb = document.getElementById('btn-stop-test');
                if (sb) { sb.style.display = ''; }
                z2StartPolling(t.task_id, t.mode);
            }
        }
    } catch(e) {}
});
//]]>
</script>

<?php

if (!empty($input_errors)) {
    print_input_errors($input_errors);
}

if ($save_success) {
    print_info_box(gettext('Configuration saved successfully.'), 'success');
}

$form = new Form(false);
$form->addGlobal(new Form_Input('act', null, 'hidden', 'save', ['id' => 'hidden_act']));

// ---- General Settings ----
$section = new Form_Section(gettext('General Settings'));

$section->addInput(new Form_Checkbox(
    'enabled',
    gettext('Enable Zapret2'),
    gettext('Enable Zapret2 DPI bypass service'),
    ($pconfig['enabled'] ?? '') === 'on'
))->setHelp(gettext('When enabled, the service will start automatically on boot.'));

$section->addInput(new Form_Select(
    'profile',
    gettext('Profile'),
    $pconfig['profile'] ?? 'default',
    $available_profiles
))->setHelp(
    'Select a bypass strategy. If none work, use <b>Custom</b> and refer to the ' .
    '<a href="https://github.com/bol-van/zapret2/blob/main/docs/manual.md" target="_blank">zapret2 manual</a> ' .
    'for available --lua-desync parameters.'
);

$form->add($section);

// ---- Traffic Filtering ----
$section = new Form_Section(gettext('Traffic Filtering'));

$section->addInput(new Form_Select(
    'alias_name',
    gettext('Restrict to Alias (Include Mode)'),
    $pconfig['alias_name'] ?? '',
    $available_aliases
))->setHelp(gettext(
    'Select a Firewall Alias containing IPs/subnets. When set, ONLY traffic to those ' .
    'destinations will be processed by dvtws2 — everything else bypasses it entirely. ' .
    'Leave blank to process all TCP 80/443 traffic (default behavior). ' .
    'Ignored when Discord Bypass is enabled (dvtws2 filters by domain itself). ' .
    'Hostname entries in the alias are skipped (ipfw tables require IPs/CIDRs).'
));

$section->addInput(new Form_Checkbox(
    'discord_enabled',
    gettext('Discord DPI Bypass (TCP 443)'),
    gettext('Enable DPI bypass for Discord domains'),
    ($pconfig['discord_enabled'] ?? '') === 'on'
))->setHelp(gettext(
    'Adds a hostlist filter to dvtws2 for Discord domains (discord.com, discordapp.com, ' .
    'discord.gg, discord.media, etc.). When enabled, overrides Alias mode: all TCP 80/443 ' .
    'traffic goes to dvtws2 and it filters by domain name.'
));

$section->addInput(new Form_Checkbox(
    'discord_udp_enabled',
    gettext('Discord Voice/Video (UDP)'),
    gettext('Enable UDP divert for Discord voice and video'),
    ($pconfig['discord_udp_enabled'] ?? '') === 'on'
))->setHelp(gettext(
    'Adds IPFW rules to divert Discord voice/video UDP traffic (port 443 QUIC and ' .
    'ports 50000–65535) through dvtws2. Requires Discord DPI Bypass (TCP) to be enabled.'
))->setAttribute('id', 'discord_udp_enabled');

$section->addInput(new Form_Checkbox(
    'youtube_enabled',
    gettext('YouTube DPI Bypass (TCP 443)'),
    gettext('Enable DPI bypass for YouTube and Google Video domains'),
    ($pconfig['youtube_enabled'] ?? '') === 'on'
))->setHelp(gettext(
    'Adds a hostlist filter to dvtws2 for YouTube domains (youtube.com, googlevideo.com, ' .
    'ytimg.com, youtu.be, etc.). dvtws2 applies the selected profile strategy only to matching ' .
    'traffic. Compatible with Discord bypass — both hostlists are active simultaneously.'
));

$section->addInput(new Form_Checkbox(
    'youtube_quic_enabled',
    gettext('YouTube QUIC / HTTP3 (UDP 443)'),
    gettext('Enable UDP divert for YouTube QUIC traffic'),
    ($pconfig['youtube_quic_enabled'] ?? '') === 'on'
))->setHelp(gettext(
    'Adds an IPFW rule to divert outbound UDP port 443 (QUIC / HTTP3) through dvtws2. ' .
    'Required to bypass YouTube when the browser uses HTTP/3 instead of TCP. ' .
    'Requires YouTube DPI Bypass (TCP) to be enabled.'
))->setAttribute('id', 'youtube_quic_enabled');

$form->add($section);

// ---- Advanced ----
$section = new Form_Section(gettext('Advanced'));
$section->addClass('advanced');

$section->addInput(new Form_Input(
    'divert_port',
    gettext('Divert Port'),
    'number',
    $current_divert_port
))->setHelp(gettext(
    'IPFW divert socket port number used by dvtws2. Default: 990. ' .
    'Must match the port in the IPFW divert rule: ' .
    'ipfw add 100 divert PORT tcp from any to any 80,443 out not diverted not sockarg. ' .
    'Valid range: 1–65535.'
))->setAttribute('min', '1')
  ->setAttribute('max', '65535');

$section->addInput(new Form_Textarea(
    'custom_args',
    gettext('Custom dvtws2 Arguments'),
    $pconfig['custom_args'] ?? ''
))->setHelp(gettext(
    'ADVANCED / DANGEROUS: Raw arguments passed directly to dvtws2. ' .
    'Only used when profile is set to "Custom arguments". ' .
    'Incorrect values may break DPI bypass or cause service failure. ' .
    'See dvtws2 --help for supported flags.'
))->setAttribute('rows', '4')
  ->setAttribute('id', 'custom_args')
  ->setAttribute('placeholder', '--lua-desync=multisplit --lua-desync=fake:blob=fake_default_tls:tcp_md5');

$section->addInput(new Form_Checkbox(
    'debug',
    gettext('Debug Logging'),
    gettext('Enable verbose debug logging to ' . ZAPRET2_LOG_FILE),
    ($pconfig['debug'] ?? '') === 'on'
));

$form->add($section);


print($form);
?>

<nav class="action-buttons">
    <button type="button" class="btn btn-primary" onclick="submitAction('save')">
        <i class="fa fa-save icon-embed-btn"></i>
        <?= gettext('Save') ?>
    </button>
    <button type="button" class="btn btn-success" onclick="submitAction('apply')">
        <i class="fa fa-check icon-embed-btn"></i>
        <?= gettext('Apply') ?>
    </button>
    <button type="button" class="btn btn-info" onclick="submitAction('start')">
        <i class="fa fa-play icon-embed-btn"></i>
        <?= gettext('Start') ?>
    </button>
    <button type="button" class="btn btn-warning" onclick="submitAction('stop')">
        <i class="fa fa-stop icon-embed-btn"></i>
        <?= gettext('Stop') ?>
    </button>
    <button type="button" class="btn btn-info" onclick="submitAction('restart')">
        <i class="fa fa-refresh icon-embed-btn"></i>
        <?= gettext('Restart') ?>
    </button>
    <button type="button" id="btn-healthcheck" class="btn btn-warning" onclick="runHealthCheck()">
        <i class="fa fa-stethoscope icon-embed-btn"></i>
        <?= gettext('Health Check') ?>
    </button>
</nav>

<div id="healthcheck-output" class="alert" style="display:none;white-space:pre-wrap;margin-top:10px;font-family:monospace;font-size:12px"></div>

<div class="panel panel-default" style="margin-top:15px">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Profile Tester') ?></h2>
    </div>
    <div class="panel-body" style="padding:15px 20px">
        <p class="text-muted" style="margin-bottom:14px">
            <?= gettext('Select profiles and a test mode. Each profile is activated briefly, tested, then your original profile is restored.') ?>
        </p>
        <div class="row">
            <div class="col-sm-6">
                <strong><?= gettext('Profiles to test:') ?></strong>
                <div style="margin-top:8px;column-count:2;column-gap:10px">
                <?php foreach ($available_profiles as $pkey => $plabel):
                    if ($pkey === 'custom') continue; ?>
                    <div style="margin-bottom:4px"><label style="font-weight:normal;margin:0">
                        <input type="checkbox" class="profile-test-check" value="<?= htmlspecialchars($pkey) ?>" checked>
                        <?= htmlspecialchars($pkey) ?>
                    </label></div>
                <?php endforeach; ?>
                </div>
            </div>
            <div class="col-sm-6" style="border-left:1px solid #e5e5e5;padding-left:20px">
                <strong><?= gettext('Test mode:') ?></strong>
                <div style="margin-top:10px;display:flex;flex-direction:column;gap:8px">
                    <button type="button" id="btn-test-http" class="btn btn-primary btn-run-test" onclick="runProfileTest('http')">
                        <i class="fa fa-globe icon-embed-btn"></i>
                        <?= gettext('HTTP / Ping Test') ?>
                    </button>
                    <p class="text-muted" style="font-size:12px;margin:0 0 4px">
                        <?= gettext('Checks 12 hosts (Discord, YouTube, Google, Cloudflare) via HTTP, TLS 1.2, TLS 1.3 + ping to 5 DNS servers.') ?>
                    </p>
                    <button type="button" id="btn-test-dpi" class="btn btn-warning btn-run-test" onclick="runProfileTest('dpi')">
                        <i class="fa fa-shield icon-embed-btn"></i>
                        <?= gettext('DPI Test') ?>
                    </button>
                    <p class="text-muted" style="font-size:12px;margin:0">
                        <?= gettext('Sends 64 KB payloads to international test servers. Detects TCP 16-20 KB DPI freeze pattern. Slower (~2 min).') ?>
                    </p>
                </div>
            </div>
        </div>
        <div style="margin-top:12px">
            <button type="button" id="btn-stop-test" class="btn btn-danger" style="display:none" onclick="stopTest()">
                <i class="fa fa-stop icon-embed-btn"></i>
                <?= gettext('Stop Test') ?>
            </button>
        </div>
        <div id="test-profiles-output" style="margin-top:15px"></div>
    </div>
</div>

<div class="row">
    <div class="col-sm-12">
        <?= zapret2_status_html() ?>
    </div>
</div>

<script>
// Bind form id after pfSense renders it
document.addEventListener('DOMContentLoaded', function() {
    var frm = document.querySelector('form.form-horizontal');
    if (frm) { frm.id = 'zapret2-form'; }
});
</script>

<?php
include('foot.inc');
