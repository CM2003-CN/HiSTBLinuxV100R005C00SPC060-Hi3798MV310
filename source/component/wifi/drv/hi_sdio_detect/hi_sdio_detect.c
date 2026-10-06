/******************************************************************************
 *
 * hi_sdio_detect.c
 *
 * Purpose: HiSilicon Hi3798MV300H SDIO Wi-Fi (MT7668RSN) Power On & Card Detect Driver
 *
 * This module powers on the Wi-Fi module (via REG_ON GPIO) and triggers SDIO card
 * detection so that the kernel SDIO driver (e.g., wlan_mt7668_sdio.ko) can detect
 * and bind to the hardware.
 *
 * Author: CM2003
 * Platform: HiSilicon Hi3798MV300H
 * Board: ChinaMobile M301H_ZN
 * Wi-Fi Module: MT7668RSN (SDIO)
 *
 ******************************************************************************/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>      // for mdelay
#include <linux/kernel.h>     // for pr_info, pr_err
#include <linux/gpio.h>       // Linux 标准 GPIO 框架

#include "hi_sdio_detect.h"

// ======================
// 硬件相关宏定义
// ======================

// Wi-Fi 模块 REG_ON / WL_EN GPIO 编号 = GPIO4_3 = 4 * 8 + 3 = 35
#define GPIO_WL_EN             (4 * 8 + 3)  // 即 GPIO 35

// SDIO 控制寄存器地址
#define REG_BASE_CTRL          0xf8a20008

// 映射后的虚拟地址
static void __iomem *sdio_ctrl_reg = NULL;

// SDIO 卡检测控制：根据 CONFIG_HISI_SDIO_ID 决定是 SDIO0 还是 SDIO1
// 默认使用 SDIO1（mask = 1）。如使用 SDIO0，则改为 mask = 2
#if !defined(CONFIG_HISI_SDIO_ID)
  #define CONFIG_HISI_SDIO_ID 1  // 使用 SDIO1
#endif

// 根据 SDIO ID 决定 mask 值
#if (CONFIG_HISI_SDIO_ID == 0)
  #define SDIO_DETECT_MASK     0x2  // SDIO0
#else
  #define SDIO_DETECT_MASK     0x1  // SDIO1
#endif

// Wi-Fi 模块控制 GPIO
static int gpio_wlan_reg_on = GPIO_WL_EN;

// GPIO 申请状态标志
static bool gpio_requested = false;

// ======================
// 函数声明
// ======================

static int hi_gpio_set_value(unsigned int gpio, unsigned int value);
static int hisi_wlan_set_carddetect(bool present);
static int platform_wifi_power_on(void);
static void platform_wifi_power_off(void);

// ======================
// GPIO 操作封装（使用标准 Linux GPIO 框架）
// ======================

/**
 * @brief 设置 GPIO 的电平值（高/低）
 *
 * 使用 Linux 标准 GPIO 框架
 *
 * @param gpio     GPIO 编号，如 GPIO_WL_EN (35)
 * @param value    0 = 低电平，1 = 高电平
 * @return int     0 成功，负数失败
 */
static int hi_gpio_set_value(unsigned int gpio, unsigned int value)
{
    int ret;

    // 如果 GPIO 还没有申请，先申请
    if (!gpio_requested) {
        ret = gpio_request(gpio, "wifi_reg_on");
        if (ret) {
            pr_err("GPIO[%d] request failed: %d\n", gpio, ret);
            return ret;
        }
        gpio_requested = true;
        pr_info("GPIO[%d] requested successfully\n", gpio);
    }

    // 设置 GPIO 为输出方向
    ret = gpio_direction_output(gpio, value);
    if (ret) {
        pr_err("GPIO[%d] set output direction failed: %d\n", gpio, ret);
        return ret;
    }

    // 设置 GPIO 输出电平
    gpio_set_value(gpio, value);

    pr_info("GPIO[%d] set to %d\n", gpio, value);
    return 0;
}

// ======================
// SDIO 卡检测控制
// ======================

/**
 * 控制 SDIO 卡检测引脚（拉低表示插入，拉高表示拔出）
 *
 * 通过写 REG_BASE_CTRL 寄存器（由 mask 决定）来控制
 *
 * @param present true = 插入（拉低），false = 拔出（拉高）
 * @return int    0 成功，其它失败
 */
static int hisi_wlan_set_carddetect(bool present)
{
    unsigned int regval;

    pr_info("SDIO: %s SDIO card (present=%d)\n",
            present ? "Detecting" : "Removing", present);

    if (!sdio_ctrl_reg) {
        sdio_ctrl_reg = ioremap(REG_BASE_CTRL, 4);  // 只映射4字节
        if (!sdio_ctrl_reg) {
            pr_err("Failed to ioremap SDIO control register at 0x%lx\n", REG_BASE_CTRL);
            return -ENOMEM;
        }
        pr_info("SDIO: Mapped control register to virtual addr %px\n", sdio_ctrl_reg);
    }

    regval = readl(sdio_ctrl_reg);
    pr_info("SDIO: Current register value: 0x%x\n", regval);

    if (present) {
        // 拉低检测引脚 -> 通知内核有卡插入
        regval |= SDIO_DETECT_MASK;
    } else {
        // 拉高检测引脚 -> 通知内核卡已移除
        regval &= ~(SDIO_DETECT_MASK);
    }

    writel(regval, sdio_ctrl_reg);
    pr_info("SDIO: New register value set to 0x%x\n", regval);

    return 0;
}

// ======================
// Wi-Fi 模块电源与探测流程
// ======================

/**
 * 上电 Wi-Fi 模块并触发 SDIO 卡检测
 *
 * 1. 拉高 REG_ON GPIO（gpio_wlan_reg_on），给模块供电
 * 2. 延时
 * 3. 调用 hisi_wlan_set_carddetect(1) 通知内核检测 SDIO 卡
 * 4. 延时等待模块初始化
 */
static int platform_wifi_power_on(void)
{
    int ret = 0;
    
    pr_info("==-----------------------------==\n");
    pr_info("WiFi: Powering ON Wi-Fi module...\n");

    // Step 1: Enable WL_EN / REG_ON (GPIO 35)
    ret = hi_gpio_set_value(gpio_wlan_reg_on, 1);
    if (ret != 0) {
        pr_err("Failed to power on Wi-Fi (GPIO set failed)\n");
        return ret;
    }
    mdelay(1500);  // 延时等待电源稳定*****此处一般为100，但是在我的板上不稳定，所以加长了延时。

    // Step 2: 通知内核：插上了 SDIO 卡（拉低检测脚）
    pr_info("==------------\n");
    ret = hisi_wlan_set_carddetect(1);
    if (ret != 0) {
        pr_err("Failed to set card detect\n");
        // 即使卡检测失败，也要继续执行，因为电源已经打开
    }
    mdelay(2000);  // 等待模块初始化 & 驱动探测
    pr_info("==------mdelay(2000)------\n");
    pr_info("WiFi: Power ON & Card Detect DONE.\n");
    return 0;
}

/**
 * 关闭 Wi-Fi 模块电源并移除 SDIO 卡检测
 */
static void platform_wifi_power_off(void)
{
    pr_info("WiFi: Powering OFF Wi-Fi module...\n");

    // 1. 通知内核：移除 SDIO 卡（拉高检测脚）
    hisi_wlan_set_carddetect(0);
    mdelay(100);

    // 2. 关闭 REG_ON （GPIO 35）
    hi_gpio_set_value(gpio_wlan_reg_on, 0);

    // 3. 释放 GPIO 资源
    if (gpio_requested) {
        gpio_free(gpio_wlan_reg_on);
        gpio_requested = false;
        pr_info("GPIO[%d] freed\n", gpio_wlan_reg_on);
    }

    pr_info("WiFi: Powered OFF.\n");
}

// ======================
// 模块入口 / 出口
// ======================

// 模块初始化函数（加载 ko 时调用）
static int __init hi_sdio_detect_init(void)
{
    int ret;
    
    pr_info("hi_sdio_detect: Initializing...\n");
    
    ret = platform_wifi_power_on();
    if (ret) {
        pr_err("Failed to power on Wi-Fi module\n");
        platform_wifi_power_off();  // 清理资源
        return ret;
    }
    
    pr_info("hi_sdio_detect: Initialization completed successfully\n");
    return 0;
}

// 模块退出函数（rmmod 时调用）
static void __exit hi_sdio_detect_exit(void)
{
    pr_info("hi_sdio_detect: Exiting...\n");

    // 取消映射寄存器
    if (sdio_ctrl_reg) {
        iounmap(sdio_ctrl_reg);
        sdio_ctrl_reg = NULL;
        pr_info("SDIO: Unmapped control register\n");
    }

    platform_wifi_power_off();
    pr_info("hi_sdio_detect: Exit completed\n");
}

// 声明模块初始化 / 退出函数
module_init(hi_sdio_detect_init);
module_exit(hi_sdio_detect_exit);

// 模块元信息
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CM2003 / Community");
MODULE_DESCRIPTION("HiSilicon Hi3798MV300H MT7668RSN SDIO Wi-Fi Power & Detect Driver");
MODULE_VERSION("1.0");
