#include <string.h>
#include <stdio.h>

#include "esp_console.h"
#include "esp_log.h"

#include "credential.h"
#include "console_cmds.h"

static const char *TAG = "console_cmds";

static int cmd_cred(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        char id[CREDENTIAL_ID_MAX_LEN];
        char secret[CREDENTIAL_SECRET_MAX_LEN];
        if (credential_load(id, sizeof(id), secret, sizeof(secret))) {
            printf("device_id=%s (secret configured)\n", id);
        } else {
            printf("credential not set\n");
        }
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        esp_err_t err = credential_save(argv[2], argv[3]);
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("OK\n");
        return 0;
    }
    printf("usage: cred show | cred set <device_id> <device_secret>\n");
    return 1;
}

esp_err_t console_cmds_start(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "hub-link> ";
    repl_config.max_cmdline_length = 256;

    // esp_console_new_repl_uart (実UART0 = GPIO37/38) ではなく USB Serial/JTAG
    // を使う。この基板は PC と native USB Serial/JTAG (esptool/espflash が
    // 使うのと同じ COM ポート) でしか繋がっておらず、GPIO37/38 には何も
    // 配線されていない。CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG により
    // ログ出力 (stdout) だけは USB 側にもミラーされるため COM ポートで見えて
    // しまうが、これは書き込み専用のミラーで stdin は複製されない —
    // 結果、REPL は誰も繋いでいない UART0 の RX を待ち続け、USB 経由でどんな
    // コマンドを送っても (改行コードを変えても) 一切応答が返らない事故と
    // なっていた (実機検証で特定)。
    esp_console_dev_usb_serial_jtag_config_t usb_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_usb_serial_jtag failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t cred_cmd = {
        .command = "cred",
        .help = "auth-worker device credential (role=device-gateway) の表示・設定。"
                "cred set <device_id> <device_secret> / cred show",
        .hint = NULL,
        .func = &cmd_cred,
    };
    err = esp_console_cmd_register(&cred_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_cmd_register failed: %s", esp_err_to_name(err));
        return err;
    }

    return esp_console_start_repl(repl);
}
