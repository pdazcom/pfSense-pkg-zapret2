<?php
// Verify failure reporting without loading or modifying a real pfSense service.
if (file_exists('/etc/inc/config.inc')) {
    echo "Runner bootstrap failure test skipped on pfSense\n";
    exit(0);
}
$id = bin2hex(random_bytes(8));
$file = '/tmp/zapret2_task_' . $id . '.json';
try {
    $proc = proc_open([PHP_BINARY, __DIR__ . '/../files/usr/local/www/zapret2/zapret2_test_runner.php', $id, 'http', '["safe"]'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) { throw new RuntimeException('Could not launch runner'); }
    proc_close($proc);
    $state = json_decode(file_get_contents($file), true, 512, JSON_THROW_ON_ERROR);
    if ($state['status'] !== 'error' || empty($state['error']) || $state['current'] !== null) {
        throw new RuntimeException('Bootstrap failure left the task running');
    }
    echo "Runner bootstrap failure reports a terminal error: passed\n";
} finally {
    @unlink($file);
}
