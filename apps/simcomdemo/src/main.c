#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/cmux.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/logging/log.h>
#include <string.h>

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

static uint8_t cmux_rx_buf[512];
static uint8_t cmux_tx_buf[512];
static struct modem_cmux cmux;

static uint8_t dlci1_rx_buf[256];
static struct modem_cmux_dlci dlci1;

static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2));

static K_SEM_DEFINE(cmux_connected_sem, 0, 1);

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

static void uart_pipe_handler(struct modem_pipe *pipe,
                              enum modem_pipe_event event,
                              void *user_data)
{
    if (event == MODEM_PIPE_EVENT_RECEIVE_READY) {
        uint8_t buf[128];
        int len = modem_pipe_receive(pipe, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            LOG_INF("[UART Response] %s", buf);
        }
    }
}

static void dlci1_pipe_handler(struct modem_pipe *pipe,
                               enum modem_pipe_event event,
                               void *user_data)
{
    if (event == MODEM_PIPE_EVENT_RECEIVE_READY) {
        uint8_t buf[128];
        int len = modem_pipe_receive(pipe, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            LOG_INF("[DLCI1 Response] %s", buf);
        }
    }
}

int main(void)
{
    struct modem_pipe *uart_pipe;
    struct modem_pipe *dlci1_pipe;
    int ret;

    LOG_INF("Starting Application...");

    ret = modem_power_on();
    if (ret < 0) {
        LOG_ERR("Failed to power on modem: %d. Application halted.", ret);
        return ret;
    }

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART not ready");
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

    uart_pipe = modem_backend_uart_init(&uart_backend, &uart_config);
    if (!uart_pipe) {
        LOG_ERR("UART backend init failed");
        return -EIO;
    }
    LOG_INF("UART backend OK: %p", uart_pipe);

    modem_pipe_attach(uart_pipe, uart_pipe_handler, NULL);
    LOG_INF("UART callback attached");

    ret = modem_pipe_open(uart_pipe);
    if (ret < 0) {
        LOG_ERR("Failed to open UART pipe: %d", ret);
        return ret;
    }
    LOG_INF("UART pipe opened");

    k_sleep(K_SECONDS(1));

    const char *cmd_at = "AT\r\n";
    ret = modem_pipe_transmit(uart_pipe, (const uint8_t *)cmd_at, strlen(cmd_at));
    if (ret < 0) {
        LOG_ERR("Failed to send AT: %d", ret);
    } else {
        LOG_INF("Sent: AT");
    }

    k_sleep(K_SECONDS(1));

    const char *cmd_cmux = "AT+CMUX=0\r\n";
    ret = modem_pipe_transmit(uart_pipe, (const uint8_t *)cmd_cmux, strlen(cmd_cmux));
    if (ret < 0) {
        LOG_ERR("Failed to send AT+CMUX: %d", ret);
    } else {
        LOG_INF("Sent: AT+CMUX=0");
    }

    k_sleep(K_SECONDS(2));

    LOG_INF("UART pipe ready for CMUX attachment");

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

    modem_pipe_attach(uart_pipe, NULL, NULL);
    LOG_INF("UART handler detached before CMUX attach");

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

    ret = k_sem_take(&cmux_connected_sem, K_SECONDS(10));
    if (ret < 0) {
        LOG_ERR("CMUX connect timeout");
        return ret;
    }
    LOG_INF("CMUX ready");

    const struct modem_cmux_dlci_config dlci1_config = {
        .dlci_address = 1,
        .receive_buf = dlci1_rx_buf,
        .receive_buf_size = sizeof(dlci1_rx_buf),
    };

    dlci1_pipe = modem_cmux_dlci_init(&cmux, &dlci1, &dlci1_config);
    if (!dlci1_pipe) {
        LOG_ERR("DLCI 1 init failed");
        return -EIO;
    }
    LOG_INF("DLCI 1 pipe created: %p", dlci1_pipe);

    modem_pipe_attach(dlci1_pipe, dlci1_pipe_handler, NULL);
    LOG_INF("DLCI 1 callback attached");

    ret = modem_pipe_open(dlci1_pipe);
    if (ret < 0) {
        LOG_ERR("Failed to open DLCI 1 pipe: %d", ret);
        return ret;
    }
    LOG_INF("DLCI 1 pipe opened");

    k_sleep(K_SECONDS(1));

    const char *test_cmd = "AT+CSQ\r\n";
    ret = modem_pipe_transmit(dlci1_pipe, (const uint8_t *)test_cmd, strlen(test_cmd));
    if (ret < 0) {
        LOG_ERR("Failed to send AT+CSQ on DLCI 1: %d", ret);
    } else {
        LOG_INF("[DLCI1] Sent: AT+CSQ");
    }

    k_sleep(K_SECONDS(2));

    const char *test_cmd2 = "AT+CPIN?\r\n";
    ret = modem_pipe_transmit(dlci1_pipe, (const uint8_t *)test_cmd2, strlen(test_cmd2));
    if (ret < 0) {
        LOG_ERR("Failed to send AT+CPIN on DLCI 1: %d", ret);
    } else {
        LOG_INF("[DLCI1] Sent: AT+CPIN?");
    }

    k_sleep(K_SECONDS(2));

    LOG_INF("DLCI 1 test complete");

    while (1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}