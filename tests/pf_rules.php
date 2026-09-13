<?php
// Configuration-scope regression checks. Run with PHP on the development host.
require_once __DIR__ . '/../files/usr/local/pkg/zapret2/includes/zapret2.inc';
$config = ['interfaces' => [
    'wan' => ['enable' => '', 'if' => 'vtnet0'],
    'lan' => ['enable' => '', 'if' => 'vtnet1'],
    'opt1' => ['enable' => '', 'if' => 'vtnet2'],
    'opt2' => ['if' => 'vtnet3'],
], 'aliases' => ['alias' => [['name' => 'Targets', 'type' => 'network']]]];
$cfg = ['firewall_backend' => 'pf', 'pf_allow' => 'on', 'pf_interfaces' => 'lan'];
function check(bool $condition, string $message): void {
    if (!$condition) { throw new RuntimeException($message); }
}
function rejects(array $cfg): void {
    try { zapret2_pf_rules($cfg); } catch (RuntimeException $e) { return; }
    throw new RuntimeException('Unsafe PF configuration was accepted');
}
check(zapret2_backend([]) === 'ipfw', 'Existing configurations must keep IPFW');
check(array_keys(zapret2_pf_interfaces()) === ['lan', 'opt1'], 'WAN and disabled interfaces must be excluded');
rejects(array_replace($cfg, ['pf_allow' => '']));
rejects(array_replace($cfg, ['pf_interfaces' => '']));
rejects(array_replace($cfg, ['pf_interfaces' => 'wan']));
rejects(array_replace($cfg, ['pf_interfaces' => 'opt2']));
rejects(array_replace($cfg, ['alias_name' => 'missing']));
rejects(array_replace($cfg, ['alias_name' => 'Targets> from any']));
$rules = zapret2_pf_rules($cfg);
check(str_contains($rules, 'from (vtnet1:network) to ! (self)'), 'Scope must exclude router addresses and off-subnet sources');
check(!str_contains($rules, 'proto udp'), 'UDP must be opt-in');
check(!str_contains($rules, 'inet6'), 'PF backend is IPv4-only');
$rules = zapret2_pf_rules(array_replace($cfg, ['alias_name' => 'Targets']));
check(str_contains($rules, 'to <Targets>'), 'Destination alias must be honored');
check(str_contains($rules, 'to ! (self) {'), 'Alias mode must still exclude router addresses');
$rules = zapret2_pf_rules(array_replace($cfg, ['alias_name' => 'Targets', 'youtube_enabled' => 'on']));
check(!str_contains($rules, '<Targets>'), 'Hostlist mode must override destination alias');
$rules = zapret2_pf_rules(array_replace($cfg, ['pf_interfaces' => 'lan,opt1', 'youtube_quic_enabled' => 'on', 'discord_udp_enabled' => 'on', 'divert_port' => 991]));
check(substr_count($rules, 'label "zapret2"') === 6, 'Each interface needs TCP, QUIC and voice rules');
check(str_contains($rules, 'port 50000:65535') && str_contains($rules, 'port 991'), 'Voice range and custom divert port must be respected');
echo "PF rule scope and validation tests passed\n";
