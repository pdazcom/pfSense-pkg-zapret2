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

$taskId  = preg_replace('/[^a-f0-9]/', '', $argv[1] ?? '');
$mode    = in_array($argv[2] ?? '', ['http', 'dpi'], true) ? $argv[2] : 'http';
$profiles = json_decode($argv[3] ?? '[]', true);

if ($taskId === '' || empty($profiles)) {
    exit(1);
}

$taskFile = '/tmp/zapret2_task_' . $taskId . '.json';

function task_write(string $taskFile, array $state): void
{
    $tmp = $taskFile . '.' . getmypid() . '.tmp';
    if (file_put_contents($tmp, json_encode($state, JSON_THROW_ON_ERROR), LOCK_EX) === false) {
        throw new RuntimeException('Cannot write test progress');
    }
    chmod($tmp, 0600);
    rename($tmp, $taskFile);
}

function task_read(string $taskFile): array
{
    if (!file_exists($taskFile)) {
        return [];
    }
    $state = json_decode(file_get_contents($taskFile), true) ?? [];
    if (file_exists($taskFile . '.cancel')) {
        $state['status'] = 'cancelled';
    }
    if (time() - ($state['started_at'] ?? time()) > 1800) {
        throw new RuntimeException('Profile test exceeded the 30 minute limit');
    }
    return $state;
}

$state = [
    'task_id'   => $taskId,
    'status'    => 'running',
    'pid'       => getmypid(),
    'mode'      => $mode,
    'profiles'  => $profiles,
    'current'   => null,
    'results'   => [],
    'started_at' => time(),
    'updated_at' => time(),
];
task_write($taskFile, $state);

// Report even bootstrap/fatal errors, and restore the service on every exit path.
$cfg = null;
$wasRunning = false;
$restoreCfg = null;
$serviceChanged = false;
$finished = false;
$runnerLock = null;
register_shutdown_function(function () use (&$state, $taskFile, &$restoreCfg, &$wasRunning, &$serviceChanged, &$finished, &$runnerLock) {
    global $config;
    if (!$finished) {
        $state['status'] = 'error';
        $state['error'] = $state['error'] ?? 'Profile runner terminated unexpectedly; see /var/log/zapret2-test.log';
    }
    try {
        if ($serviceChanged) {
            $config['installedpackages']['zapret2']['config'][0] = $restoreCfg;
            if (!zapret2_stop() || ($wasRunning && !zapret2_start())) {
                throw new RuntimeException('Failed to restore the original service state');
            }
        }
    } catch (Throwable $e) {
        $state['status'] = 'error';
        $state['error'] = $e->getMessage();
    }
    $state['current'] = null;
    $state['updated_at'] = time();
    task_write($taskFile, $state);
    @unlink($taskFile . '.cancel');
    if (is_resource($runnerLock)) {
        flock($runnerLock, LOCK_UN);
        fclose($runnerLock);
    }
});

try {
    require_once('/etc/inc/globals.inc');
    require_once('/etc/inc/config.inc');
    require_once('/etc/inc/functions.inc');
    require_once('/usr/local/pkg/zapret2/includes/zapret2.inc');

    $runnerLock = fopen('/var/run/zapret2-test.lock', 'c');
    if (!$runnerLock || !flock($runnerLock, LOCK_EX | LOCK_NB)) {
        throw new RuntimeException('Another profile test is already running');
    }
    $cfg = zapret2_get_config();
    $wasRunning = zapret2_is_running();
    $restoreCfg = $wasRunning ? (zapret2_active_config() ?: $cfg) : $cfg;
    if (zapret2_backend($cfg) === 'pf' || ($wasRunning && zapret2_backend($restoreCfg) === 'pf')) {
        throw new RuntimeException('PF divert must be tested from a LAN client, not from the router.');
    }

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

        $serviceChanged = true;
        if (!zapret2_stop()) {
            throw new RuntimeException('Could not stop the service before testing');
        }
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

        $wasCancelled = !empty($result['cancelled']);
        unset($result['cancelled']);

        $state['results'][$profile] = array_merge(['started' => true], $result);
        $state['updated_at'] = time();
        task_write($taskFile, $state);

        if ($wasCancelled) {
            break;
        }
    }


    $state['status'] = (task_read($taskFile)['status'] ?? '') === 'cancelled' ? 'cancelled' : 'done';
    $finished = true;
} catch (Throwable $e) {
    $state['error'] = $e->getMessage();
    error_log('Zapret2 profile test: ' . $e->getMessage());
}

// ---------------------------------------------------------------------------
// Incremental test runners — write partial results after each target
// ---------------------------------------------------------------------------

function zapret2_run_http_test_incremental(string $taskFile, array &$state, string $profile): array
{
    $httpTargets = unserialize(ZAPRET2_HTTP_TARGETS);
    $pingTargets = unserialize(ZAPRET2_PING_TARGETS);
    $targets     = [];

    foreach ($httpTargets as $name => $url) {
        if ((task_read($taskFile)['status'] ?? '') === 'cancelled') {
            return ['http' => $targets, 'cancelled' => true];
        }

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
        if ((task_read($taskFile)['status'] ?? '') === 'cancelled') {
            return ['http' => $targets, 'cancelled' => true];
        }

        $targets[$name] = ['url' => $ip, 'ping' => zapret2_ping_check($ip)];

        $state['results'][$profile] = ['started' => true, 'http' => $targets];
        $state['updated_at'] = time();
        task_write($taskFile, $state);
    }

    return ['http' => $targets];
}

function zapret2_run_dpi_test_incremental(string $taskFile, array &$state, string $profile): array
{
    $suiteJson = @file_get_contents(ZAPRET2_DPI_SUITE_URL, false, stream_context_create(['http' => ['timeout' => 10]]));
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
        if ((task_read($taskFile)['status'] ?? '') === 'cancelled') {
            @unlink($payloadFile);
            return ['dpi' => $targets, 'cancelled' => true];
        }

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
