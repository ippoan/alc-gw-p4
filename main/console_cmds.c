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

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed: %s", esp_err_to_name(err));
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
