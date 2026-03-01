#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem, LOG_LEVEL_INF);

#define MODEM_STABILIZATION_MS  100
#define MODEM_PWRKEY_PULSE_MS   100
#define MODEM_BOOT_DELAY_SEC    13

static const struct gpio_dt_spec pwr_en =
    GPIO_DT_SPEC_GET(DT_NODELABEL(power), gpios);
static const struct gpio_dt_spec pwrkey =
    GPIO_DT_SPEC_GET(DT_NODELABEL(pwrkey), gpios);

static void uart_cb(const struct device *dev, void *user_data)
{
    static char buf[128];
    static int  pos;
    uint8_t c;

    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }

    while (uart_fifo_read(dev, &c, 1) == 1) {
        if (c == '\n') {
            buf[pos] = '\0';
            if (pos > 0) {
                LOG_INF("< %s", buf);
            }
            pos = 0;
        } else if (c != '\r' && pos < sizeof(buf) - 1) {
            buf[pos++] = c;
        }
    }
}

static int modem_power_on(void)
{
    int ret;

    if (!gpio_is_ready_dt(&pwr_en)) {
        LOG_ERR("PWR_EN GPIO not ready");
        return -ENODEV;
    }
    if (!gpio_is_ready_dt(&pwrkey)) {
        LOG_ERR("PWRKEY GPIO not ready");
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
    const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart2));

    if (!device_is_ready(uart)) {
        LOG_ERR("UART not ready");
        return -1;
    }

    uart_irq_callback_set(uart, uart_cb);
    uart_irq_rx_enable(uart);

    ret = modem_power_on();
    if (ret < 0) {
        LOG_ERR("Failed to power on modem: %d", ret);
        return ret;
    }

    LOG_INF("Sending AT...");
    uart_poll_out(uart, 'A');
    uart_poll_out(uart, 'T');
    uart_poll_out(uart, '\r');
    uart_poll_out(uart, '\n');

    k_sleep(K_FOREVER);
    return 0;
}