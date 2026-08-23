// Host-native unit tests for cli_authz_policy.h (MQTT CLI command gate).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "cli_authz_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- cliAuthzFirstTokenIs ---------------------------------------------------

void test_first_token_exact_match(void) {
  TEST_ASSERT_TRUE(cliAuthzFirstTokenIs("mqtt.port 8884", "mqtt.port"));
  TEST_ASSERT_TRUE(cliAuthzFirstTokenIs("mqtt.port\t8884", "mqtt.port"));
  // Key with no value still matches - config.set decides what to do with it.
  TEST_ASSERT_TRUE(cliAuthzFirstTokenIs("mqtt.port", "mqtt.port"));
}

void test_first_token_skips_leading_space(void) {
  TEST_ASSERT_TRUE(cliAuthzFirstTokenIs("   mqtt.port 8884", "mqtt.port"));
  TEST_ASSERT_TRUE(cliAuthzFirstTokenIs("\t mqtt.port 8884", "mqtt.port"));
}

// The trap this function exists for: a longer key must not pass on its prefix.
void test_first_token_rejects_prefix_extension(void) {
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("mqtt.portx 1", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("mqtt.port.sub 1", "mqtt.port"));
}

void test_first_token_rejects_other_keys(void) {
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("mqtt.broker_url x", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("wifi.ssid x", "mqtt.port"));
  // Key must be the FIRST token, not any token.
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("wifi.ssid mqtt.port", "mqtt.port"));
}

void test_first_token_null_and_empty(void) {
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs(nullptr, "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("mqtt.port", nullptr));
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("mqtt.port", ""));
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzFirstTokenIs("   ", "mqtt.port"));
}

// --- cliAuthzPortValueOk ----------------------------------------------------

void test_port_value_accepts_real_ports(void) {
  TEST_ASSERT_TRUE(cliAuthzPortValueOk("mqtt.port 8884", "mqtt.port"));
  TEST_ASSERT_TRUE(cliAuthzPortValueOk("mqtt.port 8883", "mqtt.port"));
  TEST_ASSERT_TRUE(cliAuthzPortValueOk("mqtt.port 1", "mqtt.port"));
  TEST_ASSERT_TRUE(cliAuthzPortValueOk("mqtt.port 65535", "mqtt.port"));
  TEST_ASSERT_TRUE(cliAuthzPortValueOk("  mqtt.port\t8884\r\n", "mqtt.port"));
}

// The bench case: a stray brace rode in on the payload and was stored as a
// string, which drops the device off the broker at the next reload.
void test_port_value_rejects_trailing_junk(void) {
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port 8884}", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port 8884abc", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port 8884 8885", "mqtt.port"));
}

void test_port_value_rejects_out_of_range(void) {
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port 0", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port 65536", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port 999999", "mqtt.port"));
}

void test_port_value_rejects_non_numeric_and_missing(void) {
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port ", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port abc", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port -1", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.port +8884", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk(nullptr, "mqtt.port"));
}

// The key rule still applies on top of the value rule.
void test_port_value_rejects_other_keys(void) {
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.broker_url 8884", "mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueOk("mqtt.portx 8884", "mqtt.port"));
}

// --- cliAuthzPasswordCmdAllowed ---------------------------------------------

void test_password_allows_provisioning_set(void) {
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("cert.set", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("cert.apply", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("cert.info", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("secret.set", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("restart", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("version", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("chip.info", false));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("heap", false));
}

void test_password_denies_filesystem(void) {
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.cat", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.write", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.append", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.rm", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.mv", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.format", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("fs.ls", false));
}

void test_password_denies_code_and_config_read(void) {
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("lua.exec", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("lua.load", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("cell.at", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("config.dump", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("config.get", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("secret.info", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("secret.clear", false));
  // cert.clear moved off the flat deny list - see the recovery block below.
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("cert.clear", false));
  // identity.reset destroys the device keypair - never on a shared credential.
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("identity.reset", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("identity.info", false));
}

void test_password_denies_network_and_ota(void) {
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("net.http", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("net.ping", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("ota.check", false));
}

// config.set is not on the plain command list - it is handled by the payload
// rule in cliAuthzAllowed and must not pass on the name alone.
void test_password_cmd_list_excludes_config_set(void) {
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("config.set", false));
}

void test_password_cmd_null_and_unknown(void) {
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed(nullptr, false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("nosuchcommand", false));
  // Case folds with the dispatcher; prefixes and extensions never match.
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("Restart", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("restart2", false));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("cert.se", false));
}

// --- cliAuthzAllowed --------------------------------------------------------

// A device on its own cert keeps the surface it has today.
void test_mtls_allows_everything(void) {
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "fs.cat", "/config.json",
                                   false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "lua.exec", "os.exit()",
                                   false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                   "mqtt.broker_url mqtts://elsewhere", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.dump", nullptr,
                                   false));
}

// Every publish the pair and recovery flows make while the device is on the
// shared credential. If one of these ever goes false, pairing breaks.
void test_password_admits_the_pair_flow(void) {
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.set",
                                   "client_cert\n-----BEGIN CERTIFICATE-----",
                                   false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.set",
                                   "client_key\n-----BEGIN PRIVATE KEY-----",
                                   false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                   "mqtt.port 8884", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "secret.set",
                                   "mqtt.password hunter2", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "secret.set",
                                   "wifi.password:MySSID hunter2", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "restart", "{}", false));
}

// The takeover chain, blocked at dispatch rather than at the broker.
void test_password_blocks_the_takeover_chain(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                    "mqtt.broker_url mqtts://attacker", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "fs.write",
                                    "/main.lua\nos.execute()", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "fs.cat",
                                    "/config.json", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.dump", "",
                                    false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "lua.exec", "x", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "identity.reset",
                                    "--yes", false));
}

// config.set with no payload has no key to check, so it must not pass.
void test_password_config_set_needs_a_key(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set", nullptr,
                                    false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set", "",
                                    false));
}

// A permitted key with an unusable value must not reach config.set - it would
// be stored verbatim and strand the device.
void test_password_config_set_needs_a_real_port(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                    "mqtt.port 8884}", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                    "mqtt.port garbage", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                   "mqtt.port 8883", false));
}

// --- cliAuthzPortValueBad ---------------------------------------------------

void test_port_value_bad_flags_only_unusable_writes(void) {
  TEST_ASSERT_TRUE(cliAuthzPortValueBad("mqtt.port 8884}"));
  TEST_ASSERT_TRUE(cliAuthzPortValueBad("mqtt.port garbage"));
  TEST_ASSERT_TRUE(cliAuthzPortValueBad("mqtt.port 0"));
  TEST_ASSERT_TRUE(cliAuthzPortValueBad("  mqtt.port\t70000\r\n"));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("mqtt.port 8883"));
}

// Another key is not this rule's business, and a bare key writes nothing:
// config.set answers with its usage line.
void test_port_value_bad_ignores_other_writes(void) {
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("mqtt.broker_url mqtts://elsewhere"));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("mqtt.portx 8884}"));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("wifi.ssid junk}"));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad(nullptr));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad(""));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("mqtt.port"));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("mqtt.port   "));
  TEST_ASSERT_FALSE(cliAuthzPortValueBad("mqtt.port\r\n"));
}

// The value rule is universal. mTLS is the path paired devices actually use,
// so gating it only on the password path never ran where the strand happens.
void test_mtls_still_cannot_strand_the_port(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                    "mqtt.port 8884}", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                    "mqtt.port garbage", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                    "mqtt.port 65536", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                   "mqtt.port 8884", false));
}

// Authorization stays asymmetric: only the value check crossed over.
void test_mtls_keeps_its_wider_write_surface(void) {
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                   "mqtt.broker_url mqtts://elsewhere", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set",
                                   "wifi.ssid whatever}", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "config.set", nullptr,
                                   false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                    "mqtt.broker_url mqtts://elsewhere",
                                    false));
}

void test_gate_rejects_empty_command(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, nullptr, "", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "", "", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_MTLS, nullptr, "", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_MTLS, "", "", false));
}

// --- cert.clear recovery (storedCertBroken) ---------------------------------

// A cert that loads and validates keeps cert.clear on the mTLS row. Otherwise
// anyone holding the shared credential wipes certs fleet-wide.
void test_cert_clear_denied_while_cert_is_healthy(void) {
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("cert.clear", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.clear", "{}",
                                    false));
}

// The stranded device: the stored cert will not load or validate, so it is
// worth nothing and clearing it is the only way back onto password auth.
void test_cert_clear_allowed_when_stored_cert_is_broken(void) {
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("cert.clear", true));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.clear", "{}",
                                   true));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.clear", nullptr,
                                   true));
}

// A broken cert buys exactly one command, not the rest of the mTLS surface.
void test_broken_cert_widens_nothing_else(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "fs.cat",
                                    "/config.json", true));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "lua.exec", "x", true));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.dump", "",
                                    true));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "secret.clear", "",
                                    true));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "identity.reset",
                                    "--yes", true));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                    "mqtt.broker_url mqtts://attacker", true));
  // Extensions never match; case folds with the dispatcher.
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed("cert.clear2", true));
  TEST_ASSERT_TRUE(cliAuthzPasswordCmdAllowed("Cert.clear", true));
  TEST_ASSERT_FALSE(cliAuthzPasswordCmdAllowed(nullptr, true));
}

// No cert stored is NOT broken. Nothing to recover, nothing to clear, so the
// caller passes false and cert.clear stays mTLS-only.
void test_no_cert_stored_is_not_broken(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.clear", "{}",
                                    false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.set",
                                   "client_cert x", false));
}

// mTLS already reaches everything; the flag must not narrow it.
void test_mtls_cert_clear_regardless_of_cert_health(void) {
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "cert.clear", "{}", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "cert.clear", "{}", true));
}

// What the app publishes to recover a stranded device: port back to the
// password listener, clear the dead cert, reboot. All three on one session.
void test_password_admits_the_recovery_flow(void) {
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "config.set",
                                   "mqtt.port 8883", true));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "cert.clear", "{}",
                                   true));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "restart", "{}", true));
}

// Shell::execute dispatches case-insensitively, so the gate must judge the
// same spelling the dispatcher accepts - "CONFIG.SET" is config.set.
void test_gate_matches_commands_case_insensitively(void) {
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_MTLS, "CONFIG.SET",
                                    "mqtt.port 8884}", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "CONFIG.SET",
                                    "mqtt.broker_url evil", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "RESTART", "{}", false));
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "Cert.Clear", "{}", true));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "SECRET.SET",
                                    "ota.url evil", false));
}

// secret.set is field-gated to what pairing provisions - the keymap set.
// Junk fields die at the gate instead of reaching the handler.
void test_secret_set_holds_the_field_to_the_provisioning_set(void) {
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed("mqtt.password hunter2"));
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed("telegram.bot_token t"));
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed("web.password w"));
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed("wifi.ap_password a"));
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed("wifi.password:Barn x"));
  TEST_ASSERT_FALSE(cliAuthzSecretFieldAllowed("wifi.password x"));
  TEST_ASSERT_FALSE(cliAuthzSecretFieldAllowed("mqtt.username x"));
  TEST_ASSERT_FALSE(cliAuthzSecretFieldAllowed("ota.url x"));
  TEST_ASSERT_FALSE(cliAuthzSecretFieldAllowed("anything.else x"));
  // No args: the handler answers its usage line and writes nothing.
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed(nullptr));
  TEST_ASSERT_TRUE(cliAuthzSecretFieldAllowed(""));
}

void test_gate_wires_the_secret_field_rule(void) {
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "secret.set",
                                   "web.password w", false));
  TEST_ASSERT_FALSE(cliAuthzAllowed(CLI_AUTH_PASSWORD, "secret.set",
                                    "not.a_field w", false));
  // mTLS sessions are not field-gated - full surface by design.
  TEST_ASSERT_TRUE(cliAuthzAllowed(CLI_AUTH_MTLS, "secret.set",
                                   "not.a_field w", false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_token_exact_match);
  RUN_TEST(test_first_token_skips_leading_space);
  RUN_TEST(test_first_token_rejects_prefix_extension);
  RUN_TEST(test_first_token_rejects_other_keys);
  RUN_TEST(test_first_token_null_and_empty);
  RUN_TEST(test_password_allows_provisioning_set);
  RUN_TEST(test_password_denies_filesystem);
  RUN_TEST(test_password_denies_code_and_config_read);
  RUN_TEST(test_password_denies_network_and_ota);
  RUN_TEST(test_port_value_accepts_real_ports);
  RUN_TEST(test_port_value_rejects_trailing_junk);
  RUN_TEST(test_port_value_rejects_out_of_range);
  RUN_TEST(test_port_value_rejects_non_numeric_and_missing);
  RUN_TEST(test_port_value_rejects_other_keys);
  RUN_TEST(test_password_cmd_list_excludes_config_set);
  RUN_TEST(test_password_cmd_null_and_unknown);
  RUN_TEST(test_mtls_allows_everything);
  RUN_TEST(test_password_admits_the_pair_flow);
  RUN_TEST(test_password_blocks_the_takeover_chain);
  RUN_TEST(test_password_config_set_needs_a_key);
  RUN_TEST(test_password_config_set_needs_a_real_port);
  RUN_TEST(test_port_value_bad_flags_only_unusable_writes);
  RUN_TEST(test_port_value_bad_ignores_other_writes);
  RUN_TEST(test_mtls_still_cannot_strand_the_port);
  RUN_TEST(test_mtls_keeps_its_wider_write_surface);
  RUN_TEST(test_gate_rejects_empty_command);
  RUN_TEST(test_cert_clear_denied_while_cert_is_healthy);
  RUN_TEST(test_cert_clear_allowed_when_stored_cert_is_broken);
  RUN_TEST(test_broken_cert_widens_nothing_else);
  RUN_TEST(test_no_cert_stored_is_not_broken);
  RUN_TEST(test_mtls_cert_clear_regardless_of_cert_health);
  RUN_TEST(test_password_admits_the_recovery_flow);
  RUN_TEST(test_gate_matches_commands_case_insensitively);
  RUN_TEST(test_secret_set_holds_the_field_to_the_provisioning_set);
  RUN_TEST(test_gate_wires_the_secret_field_rule);
  return UNITY_END();
}
