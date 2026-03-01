#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/modem/chat.h>
#include <zephyr/modem/cmux.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem, LOG_LEVEL_INF);

#define MODEM_STABILIZATION_MS  100
#define MODEM_PWRKEY_PULSE_MS   100
#define MODEM_BOOT_DELAY_SEC    13

static const struct gpio_dt_spec pwr_en =
    GPIO_DT_SPEC_GET(DT_NODELABEL(power), gpios);
static const struct gpio_dt_spec pwrkey =
    GPIO_DT_SPEC_GET(DT_NODELABEL(pwrkey), gpios);

static uint8_t uart_rx_buf[1024];
static uint8_t uart_tx_buf[256];
static struct modem_backend_uart uart_backend;
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2));

static struct modem_chat chat;
static uint8_t chat_rx_buf[512];
static uint8_t *chat_argv[12];
static K_SEM_DEFINE(script_done, 0, 1);

static struct modem_cmux cmux;
static uint8_t cmux_rx_buf[512];
static uint8_t cmux_tx_buf[512];
static K_SEM_DEFINE(cmux_connected_sem, 0, 1);

static struct modem_cmux_dlci dlci_at;
static struct modem_pipe *at_pipe;
static uint8_t at_rx_buf[512];

static struct modem_cmux_dlci dlci_data;
static struct modem_pipe *data_pipe;
static uint8_t data_rx_buf[1500];

static void dump_cb(struct modem_chat *chat,
                    char **argv, uint16_t argc, void *user_data)
{
    for (uint16_t i = 0; i < argc; i++) {
        if (argv[i]) {
            LOG_INF("MODEM RX [%u]: \"%s\"", i, argv[i]);
        }
    }
}

static void script_cb(struct modem_chat *chat,
                      enum modem_chat_script_result result,
                      void *user_data)
{
    LOG_INF("AT script finished (%d)", result);
    k_sem_give(&script_done);
}

static void cmux_event_handler(struct modem_cmux *cmux,
                               enum modem_cmux_event event,
                               void *user_data)
{
    switch (event) {
    case MODEM_CMUX_EVENT_CONNECTED:
        LOG_INF("CMUX connected!");
        k_sem_give(&cmux_connected_sem);
        break;
    case MODEM_CMUX_EVENT_DISCONNECTED:
        LOG_WRN("CMUX disconnected");
        break;
    }
}

static const struct modem_chat_match ok_match =
    MODEM_CHAT_MATCH("OK", "", dump_cb);

static const struct modem_chat_match abort_matches[] = {
    MODEM_CHAT_MATCH("ERROR", "", dump_cb),
};

static const struct modem_chat_script_chat device_cmds[] = {
    MODEM_CHAT_SCRIPT_CMD_RESP("ATE0\r\n", ok_match),
    MODEM_CHAT_SCRIPT_CMD_RESP("AT\r\n", ok_match),
};

MODEM_CHAT_SCRIPT_DEFINE(device_script,
    device_cmds, abort_matches, script_cb, 5);

static const struct modem_chat_script_chat network_cmds[] = {
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CPIN?\r\n", ok_match),
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CSQ\r\n", ok_match),
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CREG?\r\n", ok_match),
};

MODEM_CHAT_SCRIPT_DEFINE(network_script,
    network_cmds, abort_matches, script_cb, 5);

static const struct modem_chat_script_chat cmux_cmds[] = {
    MODEM_CHAT_SCRIPT_CMD_RESP("AT+CMUX=0\r\n", ok_match),
};

MODEM_CHAT_SCRIPT_DEFINE(cmux_script,
    cmux_cmds, abort_matches, script_cb, 5);

static int modem_power_on(void)
{
    int ret;

    if (!gpio_is_ready_dt(&pwr_en)) {
        LOG_ERR("PWR_EN GPIO device not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&pwrkey)) {
        LOG_ERR("PWRKEY GPIO device not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&pwr_en, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) return ret;
    ret = gpio_pin_configure_dt(&pwrkey, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) return ret;

    LOG_INF("Step 1: Enabling PWR_EN");
    ret = gpio_pin_set_dt(&pwr_en, 1);
    if (ret < 0) return ret;
    k_sleep(K_MSEC(MODEM_STABILIZATION_MS));

    LOG_INF("Step 2: PWRKEY pulse (%d ms)", MODEM_PWRKEY_PULSE_MS);
    ret = gpio_pin_set_dt(&pwrkey, 1);
    if (ret < 0) return ret;
    k_sleep(K_MSEC(MODEM_PWRKEY_PULSE_MS));
    ret = gpio_pin_set_dt(&pwrkey, 0);
    if (ret < 0) return ret;

    LOG_INF("Step 3: Waiting for modem boot (%d s)...", MODEM_BOOT_DELAY_SEC);
    k_sleep(K_SECONDS(MODEM_BOOT_DELAY_SEC));

    LOG_INF("Modem power-on sequence completed");
    return 0;
}

int main(void)
{
    int ret;

    LOG_INF("Starting Application...");

    ret = modem_power_on();
    if (ret < 0) {
        LOG_ERR("Failed to power on modem: %d. Application halted.", ret);
        return ret;
    }

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }
    LOG_INF("UART device ready: %s", uart_dev->name);

    const struct modem_backend_uart_config uart_config = {
        .uart = uart_dev,
        .receive_buf = uart_rx_buf,
        .receive_buf_size = sizeof(uart_rx_buf),
        .transmit_buf = uart_tx_buf,
        .transmit_buf_size = sizeof(uart_tx_buf),
    };

    struct modem_pipe *uart_pipe = modem_backend_uart_init(
        &uart_backend, &uart_config);
    if (uart_pipe == NULL) {
        LOG_ERR("Failed to initialize UART backend");
        return -EIO;
    }
    LOG_INF("UART backend initialized: %p", uart_pipe);

    ret = modem_pipe_open(uart_pipe);
    if (ret < 0) {
        LOG_ERR("Failed to open UART pipe: %d", ret);
        return ret;
    }
    LOG_INF("UART pipe opened");

    modem_chat_init(&chat,
        &(struct modem_chat_config){
            .receive_buf = chat_rx_buf,
            .receive_buf_size = sizeof(chat_rx_buf),
            .delimiter = (uint8_t *)"\r",
            .delimiter_size = 1,
            .filter = (uint8_t *)"\n",
            .filter_size = 1,
            .argv = chat_argv,
            .argv_size = ARRAY_SIZE(chat_argv),
            .unsol_matches = NULL,
            .unsol_matches_size = 0,
        });

    modem_chat_attach(&chat, uart_pipe);
    LOG_INF("Chat attached to UART pipe");

    ret = modem_chat_run_script(&chat, &device_script);
    if (ret < 0) {
        LOG_ERR("device_script failed: %d", ret);
        return ret;
    }
    if (k_sem_take(&script_done, K_SECONDS(10)) != 0) {
        LOG_ERR("device_script timeout");
        return -ETIMEDOUT;
    }

    ret = modem_chat_run_script(&chat, &network_script);
    if (ret < 0) {
        LOG_ERR("network_script failed: %d", ret);
        return ret;
    }
    if (k_sem_take(&script_done, K_SECONDS(10)) != 0) {
        LOG_ERR("network_script timeout");
        return -ETIMEDOUT;
    }

    ret = modem_chat_run_script(&chat, &cmux_script);
    if (ret < 0) {
        LOG_ERR("cmux_script failed: %d", ret);
        return ret;
    }
    if (k_sem_take(&script_done, K_SECONDS(10)) != 0) {
        LOG_ERR("cmux_script timeout");
        return -ETIMEDOUT;
    }
    LOG_INF("AT+CMUX=0 OK");

    /* Détacher le chat avant que CMUX prenne le contrôle */
    modem_chat_release(&chat);
    LOG_INF("Chat detached from UART pipe");

    /* CMUX */
    const struct modem_cmux_config cmux_config = {
        .callback = cmux_event_handler,
        .user_data = NULL,
        .receive_buf = cmux_rx_buf,
        .receive_buf_size = sizeof(cmux_rx_buf),
        .transmit_buf = cmux_tx_buf,
        .transmit_buf_size = sizeof(cmux_tx_buf),
    };

    modem_cmux_init(&cmux, &cmux_config);
    LOG_INF("CMUX initialized");

    ret = modem_cmux_attach(&cmux, uart_pipe);
    if (ret < 0) {
        LOG_ERR("CMUX attach failed: %d", ret);
        return ret;
    }
    LOG_INF("CMUX attached to UART pipe");

    ret = modem_cmux_connect(&cmux);
    if (ret < 0) {
        LOG_ERR("CMUX connect failed: %d", ret);
        return ret;
    }
    LOG_INF("CMUX connection initiated...");

    if (k_sem_take(&cmux_connected_sem, K_SECONDS(10)) != 0) {
        LOG_ERR("CMUX connect timeout");
        return -ETIMEDOUT;
    }
    LOG_INF("CMUX ready");

    /* DLCI 1 — AT */
    at_pipe = modem_cmux_dlci_init(&cmux, &dlci_at,
        &(struct modem_cmux_dlci_config){
            .dlci_address = 1,
            .receive_buf = at_rx_buf,
            .receive_buf_size = sizeof(at_rx_buf),
        });
    if (!at_pipe) {
        LOG_ERR("DLCI 1 init failed");
        return -EIO;
    }

    ret = modem_pipe_open(at_pipe);
    if (ret < 0) {
        LOG_ERR("Failed to open AT pipe: %d", ret);
        return ret;
    }
    LOG_INF("DLCI 1 (AT) pipe opened: %p", at_pipe);

    /* DLCI 2 — DATA */
    data_pipe = modem_cmux_dlci_init(&cmux, &dlci_data,
        &(struct modem_cmux_dlci_config){
            .dlci_address = 2,
            .receive_buf = data_rx_buf,
            .receive_buf_size = sizeof(data_rx_buf),
        });
    if (!data_pipe) {
        LOG_ERR("DLCI 2 init failed");
        return -EIO;
    }

    ret = modem_pipe_open(data_pipe);
    if (ret < 0) {
        LOG_ERR("Failed to open data pipe: %d", ret);
        return ret;
    }
    LOG_INF("DLCI 2 (DATA) pipe opened: %p", data_pipe);

    /* Rattacher le chat sur DLCI 1 */
    modem_chat_attach(&chat, at_pipe);
    LOG_INF("Chat attached to DLCI 1");

    LOG_INF("Step 3 complete — CMUX ready, chat on DLCI1");

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}