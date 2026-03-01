#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/modem/backend/uart.h>
#include <zephyr/modem/pipe.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem, LOG_LEVEL_INF);

#define MODEM_STABILIZATION_MS  100
#define MODEM_PWRKEY_PULSE_MS   100
#define MODEM_BOOT_DELAY_SEC    13

static const struct gpio_dt_spec pwr_en =
    GPIO_DT_SPEC_GET(DT_NODELABEL(power), gpios);
static const struct gpio_dt_spec pwrkey =
    GPIO_DT_SPEC_GET(DT_NODELABEL(pwrkey), gpios);

static uint8_t uart_backend_receive_buf[1024];
static uint8_t uart_backend_transmit_buf[256];
static struct modem_backend_uart uart_backend;
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2));

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

    const struct modem_backend_uart_config uart_backend_config = {
        .uart = uart_dev,
        .receive_buf = uart_backend_receive_buf,
        .receive_buf_size = sizeof(uart_backend_receive_buf),
        .transmit_buf = uart_backend_transmit_buf,
        .transmit_buf_size = sizeof(uart_backend_transmit_buf),
    };

    struct modem_pipe *uart_pipe = modem_backend_uart_init(
        &uart_backend,
        &uart_backend_config
    );

    if (uart_pipe == NULL) {
        LOG_ERR("Failed to initialize UART backend");
        return -EIO;
    }

    LOG_INF("UART backend initialized, got pipe: %p", uart_pipe);

    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}