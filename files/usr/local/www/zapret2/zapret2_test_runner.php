<?php
/*
 * zapret2_test_runner.php — background profile test runner
 *
 * Launched by zapret2.php via: nohup php zapret2_test_runner.php <task_id> <mode> <profiles_json> &
 * Writes progress to /tmp/zapret2_task_<task_id>.json as results arrive.
 * Never called directly via HTTP — no web route exists for it.
 */

if (php_sapi_name() !== 'cli') {
    http_response_code(403);
    exit(1);
}

require_once('/etc/inc/globals.inc');
require_once('/etc/inc/config.inc');
require_once('/etc/inc/functions.inc');
require_once('/usr/local/pkg/zapret2/includes/zapret2.inc');

$taskId  = preg_replace('/[^a-f0-9]/', '', $argv[1] ?? '');
$mode    = in_array($argv[2] ?? '', ['http', 'dpi'], true) ? $argv[2] : 'http';
$profiles = json_decode($argv[3] ?? '[]', true);

if ($taskId === '' || empty($profiles)) {
    exit(1);
}

$taskFile = '/tmp/zapret2_task_' . $taskId . '.json';

function task_write(string $taskFile, array $state): void
{
    file_put_contents($taskFile, json_encode($state));
    chmod($taskFile, 0600);
}

function task_read(string $taskFile): array
{
    if (!file_exists($taskFile)) {
        return [];
    }
    return json_decode(file_get_contents($taskFile), true) ?? [];
}

$state = [
    'task_id'   => $taskId,
    'status'    => 'running',
    'mode'      => $mode,
    'profiles'  => $profiles,
    'current'   => null,
    'results'   => [],
    'started_at' => time(),
    'updated_at' => time(),
];
task_write($taskFile, $state);

$cfg = zapret2_get_config();

foreach ($profiles as $profile) {
    // Check if cancelled between profiles
    $fresh = task_read($taskFile);
    if (($fresh['status'] ?? '') === 'cancelled') {
        break;
    }

    $state['current']    = $profile;
    $state['updated_at'] = time();
    task_write($taskFile, $state);

    global $config;
    $testCfg            = $cfg;
    $testCfg['profile'] = $profile;
    $config['installedpackages']['zapret2']['config'][0] = $testCfg;

    zapret2_stop();
    sleep(1);
    $started = zapret2_start();

    if (!$started) {
        $state['results'][$profile] = ['started' => false];
        $state['updated_at'] = time();
        task_write($taskFile, $state);
        continue;
    }

    sleep(2);

    if ($mode === 'http') {
        $result = zapret2_run_http_test_incremental($taskFile, $state, $profile);
    } else {
        $result = zapret2_run_dpi_test_incremental($taskFile, $state, $profile);
    }

    $state['results'][$profile] = array_merge(['started' => true], $result);
    $state['updated_at'] = time();
    task_write($taskFile, $state);
}

// Restore original service state
$config['installedpackages']['zapret2']['config'][0] = $cfg;
zapret2_stop();
sleep(1);
if (($cfg['enabled'] ?? '') === 'on') {
    zapret2_start();
}

$fresh = task_read($taskFile);
$state['status']     = ($fresh['status'] ?? '') === 'cancelled' ? 'cancelled' : 'done';
$state['current']    = null;
$state['updated_at'] = time();
task_write($taskFile, $state);

// ---------------------------------------------------------------------------
// Incremental test runners — write partial results after each target
// ---------------------------------------------------------------------------

function zapret2_run_http_test_incremental(string $taskFile, array &$state, string $profile): array
{
    $httpTargets = unserialize(ZAPRET2_HTTP_TARGETS);
    $pingTargets = unserialize(ZAPRET2_PING_TARGETS);
    $targets     = [];

    foreach ($httpTargets as $name => $url) {
        $row = [
            'url'   => $url,
            'http'  => zapret2_curl_check($url, ''),
            'tls12' => zapret2_curl_check($url, 'tls12'),
            'tls13' => zapret2_curl_check($url, 'tls13'),
            'ping'  => zapret2_ping_check(parse_url($url, PHP_URL_HOST)),
        ];
        $targets[$name] = $row;

        $state['results'][$profile] = ['started' => true, 'http' => $targets];
        $state['updated_at'] = time();
        task_write($taskFile, $state);
    }

    foreach ($pingTargets as $name => $ip) {
        $targets[$name] = ['url' => $ip, 'ping' => zapret2_ping_check($ip)];

        $state['results'][$profile] = ['started' => true, 'http' => $targets];
        $state['updated_at'] = time();
        task_write($taskFile, $state);
    }

    return ['http' => $targets];
}

function zapret2_run_dpi_test_incremental(string $taskFile, array &$state, string $profile): array
{
    $suiteJson = @file_get_contents(ZAPRET2_DPI_SUITE_URL);
    if ($suiteJson === false) {
        return ['dpi' => ['error' => 'Failed to fetch DPI suite']];
    }

    $suite = json_decode($suiteJson, true);
    if (!is_array($suite)) {
        return ['dpi' => ['error' => 'Invalid DPI suite JSON']];
    }

    $payloadFile = tempnam('/tmp', 'zapret2_dpi_');
    file_put_contents($payloadFile, random_bytes(65536));

    $targets = [];
    $timeout = 5;

    foreach (array_slice($suite, 0, 20) as $target) {
        $host = $target['host'] ?? '';
        if ($host === '') {
            continue;
        }

        $id       = $target['id']       ?? $host;
        $provider = ($target['country'] ?? '') . ' ' . ($target['provider'] ?? '');
        $row      = ['host' => $host, 'label' => trim($provider . ' ' . $id), 'checks' => []];

        foreach (['http' => '--http1.1', 'tls12' => '--tlsv1.2 --tls-max 1.2', 'tls13' => '--tlsv1.3 --tls-max 1.3'] as $m => $tlsArg) {
            $cmd = 'curl -s -X POST --data-binary @' . escapeshellarg($payloadFile)
                 . ' -w "%{http_code} %{size_upload} %{size_download} %{time_total}"'
                 . ' -o /dev/null -m ' . $timeout . ' ' . $tlsArg
                 . ' ' . escapeshellarg('https://' . $host) . ' 2>/dev/null';
            $out    = [];
            $retval = 0;
            exec($cmd, $out, $retval);

            $parts     = preg_split('/\s+/', trim(implode('', $out)));
            $code      = (int) ($parts[0] ?? 0);
            $upBytes   = (int) ($parts[1] ?? 0);
            $downBytes = (int) ($parts[2] ?? 0);
            $timeSec   = (float) str_replace(',', '.', $parts[3] ?? '0');
            $frozen    = $upBytes > 1000 && $downBytes === 0 && $timeSec >= ($timeout - 0.5) && $retval !== 0;

            $row['checks'][$m] = [
                'status'  => match (true) {
                    $retval === 35 || $retval === 4       => 'UNSUP',
                    $frozen                               => 'BLOCKED',
                    $retval === 0 && $code >= 100         => 'OK',
                    default                               => 'FAIL',
                },
                'code'    => $code,
                'up_kb'   => round($upBytes / 1024, 1),
                'down_kb' => round($downBytes / 1024, 1),
                'time_ms' => (int) round($timeSec * 1000),
            ];
        }

        $targets[] = $row;

        $state['results'][$profile] = ['started' => true, 'dpi' => $targets];
        $state['updated_at'] = time();
        task_write($taskFile, $state);
    }

    @unlink($payloadFile);

    return ['dpi' => $targets];
}
