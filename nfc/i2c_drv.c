/*
 * The original Work has been changed by NXP Semiconductors.
 * Copyright 2013-2020 NXP
 *
 * Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
/*
 * Copyright (C) 2010 Trusted Logic S.A.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "../nfc/cold_reset.h"
#include "nfc_drv.h"
#include "common.h"
#include "sn110.h"

extern nfc_dev_t* nfc_dev_platform;

/**
 * i2c_disable_irq()
 *
 * Check if interrupt is disabled or not
 * and disable interrupt
 *
 * Return: void
 */
void i2c_disable_irq(i2c_dev_t *i2c_dev)
{
    unsigned long flags;

    spin_lock_irqsave(&i2c_dev->irq_enabled_lock, flags);
    if (i2c_dev->irq_enabled) {
        disable_irq_nosync(i2c_dev->client->irq);
        i2c_dev->irq_enabled = false;
    }
    spin_unlock_irqrestore(&i2c_dev->irq_enabled_lock, flags);
}

/**
 * i2c_enable_irq()
 *
 * Check if interrupt is enabled or not
 * and enable interrupt
 *
 * Return: void
 */
void i2c_enable_irq(i2c_dev_t *i2c_dev)
{
    unsigned long flags;

    spin_lock_irqsave(&i2c_dev->irq_enabled_lock, flags);
    if (!i2c_dev->irq_enabled) {
        i2c_dev->irq_enabled = true;
        enable_irq(i2c_dev->client->irq);
    }
    spin_unlock_irqrestore(&i2c_dev->irq_enabled_lock, flags);
}

static irqreturn_t i2c_irq_handler(int irq, void *dev_id)
{
    nfc_dev_t* nfc_dev = dev_id;
    i2c_dev_t *i2c_dev = NULL;
    unsigned long flags;
    i2c_dev = &nfc_dev->i2c_dev;
    if (device_may_wakeup(&i2c_dev->client->dev))
        pm_wakeup_event(&i2c_dev->client->dev, WAKEUP_SRC_TIMEOUT);

    i2c_disable_irq(i2c_dev);
    spin_lock_irqsave(&i2c_dev->irq_enabled_lock, flags);
    i2c_dev->count_irq++;
    spin_unlock_irqrestore(&i2c_dev->irq_enabled_lock, flags);
    wake_up(&nfc_dev->read_wq);

    return IRQ_HANDLED;
}

int i2c_read(i2c_dev_t *i2c_dev, char *buf, size_t count)
{
    int ret;
    pr_debug("%s : reading %zu bytes.\n", __func__, count);
    /* Read data */
    ret = i2c_master_recv(i2c_dev->client, buf, count);
    if (ret < 0) {
        pr_err("%s: i2c_master_recv returned %d\n", __func__, ret);
    }
    if (ret > count) {
        pr_err("%s: received too many bytes from i2c (%d)\n",
                __func__, ret);
        ret = -EIO;
    }
    /* delay for the slow nfc devices between susequent read operation */
    udelay(1000);
    return ret;
}

int i2c_write(i2c_dev_t *i2c_dev, char *buf, size_t count)
{
    int ret;
    pr_debug("%s : writing %zu bytes.\n", __func__, count);
    /* Write data */
    ret = i2c_master_send(i2c_dev->client, buf, count);
    if (ret != count) {
        pr_err("%s : i2c_master_send returned %d\n", __func__, ret);
        ret = -EIO;
    }
    /* delay for the slow nfc devices between susequent write operation */
    udelay(1000);
    return ret;
}

ssize_t nfc_i2c_dev_read(struct file *filp, char __user *buf,
        size_t count, loff_t *offset)
{
    int ret;
    char tmp[MAX_BUFFER_SIZE];
    nfc_dev_t *nfc_dev = filp->private_data;
    i2c_dev_t *i2c_dev = &nfc_dev->i2c_dev;

    if (count > MAX_BUFFER_SIZE)
        count = MAX_BUFFER_SIZE;

    pr_debug("%s : reading   %zu bytes.\n", __func__, count);
    mutex_lock(&nfc_dev->read_mutex);
    if (!gpio_get_value(nfc_dev->gpio.irq)) {
        if (filp->f_flags & O_NONBLOCK) {
        pr_err(":f_falg has O_NONBLOCK. EAGAIN\n");
            ret = -EAGAIN;
            goto err;
        }
        while (1) {
            ret = 0;
            if (!i2c_dev->irq_enabled) {
                i2c_dev->irq_enabled = true;
                enable_irq(i2c_dev->client->irq);
            }
            ret = wait_event_interruptible(
                    nfc_dev->read_wq,
                    !i2c_dev->irq_enabled);
            i2c_disable_irq(i2c_dev);
            if (ret)
                goto err;
            if (!gpio_get_value(nfc_dev->gpio.ven)) {
                pr_info("%s: releasing read  \n", __func__);
                ret =  -EL3RST;
                goto err;
            }
            if (gpio_get_value(nfc_dev->gpio.irq))
                break;
            pr_warning("%s: spurious interrupt detected\n", __func__);
        }
    }
    /* Read data */
    ret = i2c_read(i2c_dev, tmp, count);
    if (ret < 0) {
        pr_err("%s: i2c_read returned %d\n", __func__, ret);
        goto err;
    }
    /* check if it's response of cold reset command
     * NFC HAL process shouldn't receive this data as
     * command was sent by driver
     */
    if (nfc_dev->cold_reset.rsp_pending && IS_COLD_RESET_RSP(tmp) && (ret > 0)) {
        read_cold_reset_rsp(nfc_dev, tmp);
        nfc_dev->cold_reset.rsp_pending = false;
        wake_up_interruptible(&nfc_dev->cold_reset.read_wq);
        mutex_unlock(&nfc_dev->read_mutex);
        return 0;
    }
    mutex_unlock(&nfc_dev->read_mutex);
    if (copy_to_user(buf, tmp, ret)) {
        pr_warning("%s : failed to copy to user space\n", __func__);
        return -EFAULT;
    }
    return ret;
err:
    mutex_unlock(&nfc_dev->read_mutex);
    return ret;
}

ssize_t nfc_i2c_dev_write(struct file *filp, const char __user *buf,
        size_t count, loff_t *offset)
{
    int ret;
    char tmp[MAX_BUFFER_SIZE];
    nfc_dev_t *nfc_dev = filp->private_data;
    i2c_dev_t *i2c_dev = &nfc_dev->i2c_dev;

    if (count > MAX_BUFFER_SIZE)
        count = MAX_BUFFER_SIZE;

    if (copy_from_user(tmp, buf, count)) {
        pr_err("%s : failed to copy from user space\n", __func__);
        return -EFAULT;
    }
    ret = i2c_write(i2c_dev, tmp, count);

    return ret;
}

static const struct file_operations nfc_i2c_dev_fops = {
    .owner = THIS_MODULE,
    .llseek = no_llseek,
    .read  = nfc_i2c_dev_read,
    .write = nfc_i2c_dev_write,
    .open = nfc_dev_open,
    .release = nfc_dev_close,
    .unlocked_ioctl = nfc_dev_ioctl,
};

int nfc_i2c_dev_probe(struct i2c_client *client,
        const struct i2c_device_id *id)
{
    int ret = 0;
    nfc_dev_t *nfc_dev = NULL;
    i2c_dev_t *i2c_dev = NULL;
    platform_gpio_t  nfc_gpio;
    pr_debug("%s: enter\n", __func__);
    /*retrive details of gpios from dt*/
    ret = nfc_parse_dt(&client->dev, &nfc_gpio, PLATFORM_IF_I2C);
    if (ret) {
        pr_err("%s : failed to parse\n", __func__);
        goto err;
    }

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        pr_err("%s : need I2C_FUNC_I2C\n", __func__);
        ret = -ENODEV;
        goto err;
    }
    nfc_dev = kzalloc(sizeof(nfc_dev_t), GFP_KERNEL);
    if (nfc_dev == NULL) {
        ret = -ENOMEM;
        goto err;
    }
    nfc_dev_platform = nfc_dev;
    nfc_dev->platform = PLATFORM_IF_I2C;
    nfc_dev->i2c_dev.client = client;
    i2c_dev = &nfc_dev->i2c_dev;

    ret = configure_gpio(nfc_gpio.ven, GPIO_OUTPUT);
    if (ret) {
        pr_err("%s: unable to request nfc reset gpio [%d]\n",
            __func__, nfc_gpio.ven);
        goto err;
    }
    ret = configure_gpio(nfc_gpio.irq, GPIO_IRQ);
    if (ret <= 0) {
        pr_err("%s: unable to request nfc irq gpio [%d]\n",
            __func__, nfc_gpio.irq);
        goto err;
    }
    client->irq = ret;
    ret = configure_gpio(nfc_gpio.dwl_req, GPIO_OUTPUT);
    if (ret) {
        pr_err("%s: unable to request nfc firm downl gpio [%d]\n",
            __func__, nfc_gpio.dwl_req);
        goto err;
    }
    ret = configure_gpio(nfc_gpio.ese_pwr, GPIO_OUTPUT);
    if (ret) {
        pr_err("%s: unable to request nfc ese pwr gpio [%d]\n",
            __func__, nfc_gpio.ese_pwr);
    }
    nfc_dev->gpio.ven = nfc_gpio.ven;
    nfc_dev->gpio.irq = nfc_gpio.irq;
    nfc_dev->gpio.dwl_req  = nfc_gpio.dwl_req;
    nfc_dev->gpio.ese_pwr  = nfc_gpio.ese_pwr;

    /* init mutex and queues */
    init_waitqueue_head(&nfc_dev->read_wq);
    init_waitqueue_head(&nfc_dev->cold_reset.read_wq);
    mutex_init(&nfc_dev->read_mutex);
    mutex_init(&nfc_dev->dev_ref_mutex);
    mutex_init(&nfc_dev->ese_access_mutex);
    mutex_init(&nfc_dev->cold_reset.sync_mutex);
    spin_lock_init(&i2c_dev->irq_enabled_lock);

    ret = nfc_misc_register(nfc_dev, &nfc_i2c_dev_fops, DEV_COUNT,
            NFC_I2C_DEVICE_NAME, CLASS_NAME);
    if (ret) {
        pr_err("%s: nfc_misc_register failed\n", __func__);
        goto err_mutex_destroy;
    }
    /* interrupt initializations */
    pr_info("%s : requesting IRQ %d\n", __func__, client->irq);
    i2c_dev->irq_enabled = true;
    ret = request_irq(client->irq, i2c_irq_handler,
              IRQF_TRIGGER_HIGH, client->name, nfc_dev);
    if (ret) {
        pr_err("%s: request_irq failed\n", __func__);
        goto err_nfc_misc_unregister;
    }
    i2c_disable_irq(i2c_dev);
    device_init_wakeup(&client->dev, true);
    device_set_wakeup_capable(&client->dev, true);
    i2c_set_clientdata(client, nfc_dev);
    i2c_dev->irq_wake_up = false;
    nfc_dev->cold_reset.rsp_pending = false;
    nfc_dev->cold_reset.nfc_enabled = false;

    /*call to platform specific probe*/
    ret = sn110_i2c_probe(nfc_dev);
    if (ret != 0) {
        pr_err("%s: probing platform failed\n", __func__);
        goto err_request_free_irq;
    };
    pr_info("%s probing nfc i2c successfully",__func__);
    return 0;
err_request_free_irq:
    free_irq(client->irq, nfc_dev);
err_nfc_misc_unregister:
    nfc_misc_unregister(nfc_dev, DEV_COUNT);
err_mutex_destroy:
    mutex_destroy(&nfc_dev->dev_ref_mutex);
    mutex_destroy(&nfc_dev->read_mutex);
    mutex_destroy(&nfc_dev->ese_access_mutex);
    mutex_destroy(&nfc_dev->cold_reset.sync_mutex);
err:
    gpio_free_all(nfc_dev);
    if (nfc_dev)
        kfree(nfc_dev);
    pr_err("%s: probing not successful, check hardware\n", __func__);
    return ret;
}

int nfc_i2c_dev_remove(struct i2c_client *client)
{
    int ret = 0;
    nfc_dev_t *nfc_dev = NULL;
    pr_info("%s: remove device\n", __func__);
    nfc_dev = i2c_get_clientdata(client);
    if (!nfc_dev) {
        pr_err("%s: device doesn't exist anymore\n", __func__);
        ret = -ENODEV;
        return ret;
    }
    free_irq(client->irq, nfc_dev);
    nfc_misc_unregister(nfc_dev, DEV_COUNT);
    mutex_destroy(&nfc_dev->read_mutex);
    mutex_destroy(&nfc_dev->ese_access_mutex);
    mutex_destroy(&nfc_dev->cold_reset.sync_mutex);
    gpio_free_all(nfc_dev);
    if (nfc_dev)
        kfree(nfc_dev);
    return ret;
}

int nfc_i2c_dev_suspend(struct device *device)
{
    struct i2c_client *client = to_i2c_client(device);
    nfc_dev_t *nfc_dev = i2c_get_clientdata(client);
    i2c_dev_t *i2c_dev = &nfc_dev->i2c_dev;

    if (device_may_wakeup(&client->dev) && i2c_dev->irq_enabled) {
        if (!enable_irq_wake(client->irq))
            i2c_dev->irq_wake_up = true;
    }
    return 0;
}

int nfc_i2c_dev_resume(struct device *device)
{
    struct i2c_client *client = to_i2c_client(device);
    nfc_dev_t *nfc_dev = i2c_get_clientdata(client);
    i2c_dev_t *i2c_dev = &nfc_dev->i2c_dev;

    if (device_may_wakeup(&client->dev) && i2c_dev->irq_wake_up) {
        if (!disable_irq_wake(client->irq))
            i2c_dev->irq_wake_up = false;
    }
    return 0;
}

static const struct i2c_device_id nfc_i2c_dev_id[] = {
        { NFC_I2C_DEVICE_NAME, 0 },
        { }
};

static struct of_device_id nfc_i2c_dev_match_table[] = {
    {.compatible = NFC_I2C_DEVICE_ID,},
    {}
};

static const struct dev_pm_ops nfc_i2c_dev_pm_ops = {
    SET_SYSTEM_SLEEP_PM_OPS(nfc_i2c_dev_suspend, nfc_i2c_dev_resume)
};

static struct i2c_driver nfc_i2c_dev_driver = {
        .id_table   = nfc_i2c_dev_id,
        .probe      = nfc_i2c_dev_probe,
        .remove     = nfc_i2c_dev_remove,
        .driver     = {
                .owner = THIS_MODULE,
                .name  = NFC_I2C_DEVICE_NAME,
                .pm = &nfc_i2c_dev_pm_ops,
                .of_match_table = nfc_i2c_dev_match_table,
        },
};
MODULE_DEVICE_TABLE(of, nfc_i2c_dev_match_table);

static int __init nfc_i2c_dev_init(void)
{
    pr_info("Loading NXP NFC I2C driver\n");
    return i2c_add_driver(&nfc_i2c_dev_driver);
}
module_init(nfc_i2c_dev_init);

static void __exit nfc_i2c_dev_exit(void)
{
    pr_info("Unloading NXP NFC I2C driver\n");
    i2c_del_driver(&nfc_i2c_dev_driver);
}
module_exit(nfc_i2c_dev_exit);

MODULE_DESCRIPTION("NXP NFC I2C driver");
MODULE_LICENSE("GPL");