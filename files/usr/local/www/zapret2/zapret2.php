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

// Available profiles
$available_profiles = [
    'safe'          => 'Safe — multisplit only',
    'default'       => 'Default — multisplit + fake TLS (recommended)',
    'multidisorder' => 'Multidisorder + seqovl — Rostelecom, Beeline, MTS',
    'fake_disorder' => 'Fake TLS + multidisorder — regional ISPs',
    'wssize'        => 'Window size reduction — when fragment methods fail',
    'custom'        => 'Custom — specify arguments manually',
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

        if (empty($input_errors)) {
            $new_cfg = [
                'enabled'     => !empty($_POST['enabled']) ? 'on' : '',
                'profile'     => $profile,
                'divert_port' => $divert_port,
                'custom_args' => $custom_args,
                'debug'       => !empty($_POST['debug']) ? 'on' : '',
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

function submitAction(act) {
    document.getElementById('hidden_act').value = act;
    document.getElementById('zapret2-form').submit();
}

function runHealthCheck() {
    var btn = document.getElementById('btn-healthcheck');
    var out = document.getElementById('healthcheck-output');
    btn.disabled = true;
    btn.innerHTML = '<i class="fa fa-spinner fa-spin icon-embed-btn"></i> Running...';
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

document.addEventListener('DOMContentLoaded', function() {
    toggleCustomProfile();
    var profileSel = document.getElementById('profile');
    if (profileSel) {
        profileSel.addEventListener('change', toggleCustomProfile);
    }
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
