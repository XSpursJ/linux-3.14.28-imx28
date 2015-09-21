#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/i2c.h>
#include <linux/i2c/tsgpio.h>


struct gpio_ts_priv {
	struct i2c_client *client;
	struct gpio_chip gpio_chip;
	struct mutex mutex;
};

static inline struct gpio_ts_priv *to_gpio_ts(struct gpio_chip *chip)
{
	return container_of(chip, struct gpio_ts_priv, gpio_chip);
}

/*
 * To configure ts GPIO module registers
 */
static inline int gpio_ts_write(struct i2c_client *client, u16 addr, u8 data)
{
	u8 out[3];
	int ret;
	struct i2c_msg msg;

	out[0] = ((addr >> 8) & 0xff);
	out[1] = (addr & 0xff);
	out[2] = data;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = 3;
	msg.buf = out;

	dev_dbg(&client->dev, "%s Writing 0x%X to 0x%X\n", __func__, data, addr);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(&client->dev, "%s: write error, ret=%d\n",
			__func__, ret);
		return -EIO;
	}

	return ret;
}

/*
 * To read a ts GPIO module register
 */
static inline int gpio_ts_read(struct i2c_client *client, u16 addr)
{
	u8 data[3];
	int ret;
	struct i2c_msg msgs[2];

	data[0] = ((addr >> 8) & 0xff);
	data[1] = (addr & 0xff);
	data[2] = 0;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len	= 2;
	msgs[0].buf	= data;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len	= 1;
	msgs[1].buf	= data;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_err(&client->dev, "%s: read error, ret=%d\n",
			__func__, ret);
		return -EIO;
	}
	dev_dbg(&client->dev, "%s read 0x%X from 0x%X\n", __func__, data[0], addr);

	return data[0];
}

static int ts_set_gpio_direction(struct i2c_client *client,
	int gpio, int is_input)
{
	struct tsgpio_platform_data *pdata = client->dev.platform_data;
	u8 reg;

	dev_dbg(&client->dev, "%s setting gpio %d to is_input=%d\n", 
		__func__, gpio, is_input);
	
	reg = gpio_ts_read(client, gpio);

	if(pdata->twobit) {
		if(is_input) {
			reg = 0x0; /* can safely clear the whole reg in this case */
		} else {
			/* Since this board doesn't have seperate input/output
			* data regs, initialize to the last output value if
			* it has beenset before, or start with 0 on first
			* use */
			int oldval = (pdata->bank >> gpio) & 0x1;
			reg |= (TSGPIO_OE | (oldval << 1));
		}
	} else {
		if(is_input) {
			reg &= TSGPIO_OD;
		} else {
			reg |= TSGPIO_OE;
			/* clear other bits because a write of 0 to the upper bits
			 * keeps the output in its current state. If we read then 
			 * write it back we have a race condition on our hands */
			reg &= (TSGPIO_OD | TSGPIO_OE);
		}
	}

	gpio_ts_write(client, gpio, reg);

	return 0;
}

static int ts_set_gpio_dataout(struct i2c_client *client, int gpio, int enable)
{
	struct tsgpio_platform_data *pdata = client->dev.platform_data;
	u8 reg;

	dev_dbg(&client->dev, "%s setting gpio %d to output=%d\n", 
		__func__, gpio, enable);

	reg = gpio_ts_read(client, gpio);

	WARN_ON(pdata == NULL);

	if(enable) {
		pdata->bank |= (1 << gpio);
		reg |= TSGPIO_OD; /* set data output bit */
		/* clear other bits because a write of 0 to the upper bits
		 * keeps the output in its current state. If we read then 
		 * write it back we have a race condition on our hands */
		reg &= (TSGPIO_OD | TSGPIO_OE); 
	} else {
		pdata->bank &= ~(1 << gpio);
		reg &= TSGPIO_OE;
	}

	return gpio_ts_write(client, gpio, reg);
}

static int ts_get_gpio_datain(struct i2c_client *client, int gpio)
{
	struct tsgpio_platform_data *pdata = client->dev.platform_data;
	u8 reg, addr;
	int ret;
	
	dev_dbg(&client->dev, "%s Getting GPIO %d Input\n", __func__, gpio);

	addr = gpio;

	reg = gpio_ts_read(client, addr);

	if(pdata->twobit) {
		ret = (reg & TSGPIO_OD) ? 1 : 0;
	} else {
		ret = (reg & TSGPIO_ID) ? 1 : 0;
	}

	return ret;
}

static int ts_direction_in(struct gpio_chip *chip, unsigned offset)
{
	struct gpio_ts_priv *priv = to_gpio_ts(chip);
	int ret;

	mutex_lock(&priv->mutex);
	ret = ts_set_gpio_direction(priv->client, offset, 1);
	mutex_unlock(&priv->mutex);

	return ret;
}

static int ts_get(struct gpio_chip *chip, unsigned offset)
{
	struct gpio_ts_priv *priv = to_gpio_ts(chip);
	int status;

	mutex_lock(&priv->mutex);
	status = ts_get_gpio_datain(priv->client, offset);
	mutex_unlock(&priv->mutex);
	return status;
}

static void ts_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct gpio_ts_priv *priv = to_gpio_ts(chip);

	mutex_lock(&priv->mutex);
	ts_set_gpio_dataout(priv->client, offset, value);
	mutex_unlock(&priv->mutex);
}

static int ts_direction_out(struct gpio_chip *chip, unsigned offset, int value)
{
	struct gpio_ts_priv *priv = to_gpio_ts(chip);

	mutex_lock(&priv->mutex);
	ts_set_gpio_dataout(priv->client, offset, value);
	ts_set_gpio_direction(priv->client, offset, 0);
	mutex_unlock(&priv->mutex);

	return 0;
}

static struct gpio_chip template_chip = {
	.label			= "tsgpio",
	.owner			= THIS_MODULE,
	.request		= NULL,
	.free			= NULL,
	.direction_input	= ts_direction_in,
	.get			= ts_get,
	.direction_output	= ts_direction_out,
	.set			= ts_set,
	.to_irq			= NULL,
	.can_sleep		= 1,
};

static int gpio_ts_remove(struct i2c_client *client)
{
	struct gpio_ts_priv *priv = 
		(struct gpio_ts_priv *)i2c_get_clientdata(client);
	int status;

	status = gpiochip_remove(&priv->gpio_chip);
	if (status < 0)
		return status;

	return 0;
}


#ifdef CONFIG_OF
static const struct of_device_id tsgpio_ids[] = {
        { .compatible = "tsgpio", .data = (void *) 0},
        { .compatible = "tsgpio-2bitio", .data = (void *) 1},
        {},
};

MODULE_DEVICE_TABLE(of, tsgpio_ids);

static struct tsgpio_platform_data *tsgpio_probe_dt(struct device *dev)
{
	struct tsgpio_platform_data *pdata;
	struct device_node *node = dev->of_node;
	const struct of_device_id *match;

	if (!node) {
		dev_err(dev, "Device does not have associated DT data\n");
		return ERR_PTR(-EINVAL);
	}

	match = of_match_device(tsgpio_ids, dev);
	if (!match) {
		dev_err(dev, "Unknown device model\n");
		return ERR_PTR(-EINVAL);
	}

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->twobit = (unsigned long)match->data;		
	
	return pdata;
}
#else
static struct tsgpio_platform_data *tsgpio_probe_dt(struct device *dev)
{
	dev_err(dev, "no platform data defined\n");
	return ERR_PTR(-EINVAL);
}
#endif

static int gpio_ts_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
	struct gpio_ts_priv *priv;
	struct tsgpio_platform_data *pdata;
	
	int ret;

	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	pdata = dev_get_platdata(&client->dev);
	if (!pdata) {
		pdata = tsgpio_probe_dt(&client->dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}		
	client->dev.platform_data = pdata;
		
	priv = devm_kzalloc(&client->dev, sizeof(struct gpio_ts_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	i2c_set_clientdata(client, priv);
	priv->client = client;
	priv->gpio_chip = template_chip;
	priv->gpio_chip.base = -1;
	priv->gpio_chip.ngpio = 64;
	priv->gpio_chip.label = "tsgpio";
	priv->gpio_chip.dev = &client->dev;

	mutex_init(&priv->mutex);

	ts_set_gpio_dataout(client, 14, 1);

	ret = gpiochip_add(&priv->gpio_chip);
	if (ret < 0) {
		dev_err(&client->dev, "could not register gpiochip, %d\n", ret);
		priv->gpio_chip.ngpio = 0;
	}
	
	return ret;
}

static const struct i2c_device_id tsgpio_id[] = {
	{ "tsgpio", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tsgpio_id);

MODULE_ALIAS("platform:tsgpio");

static struct i2c_driver gpio_ts_driver = {
	.driver = {
		.name	= "tsgpio",
		.owner	= THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(tsgpio_ids),
#endif
	},
	.probe		= gpio_ts_probe,
	.remove		= gpio_ts_remove,
	.id_table 	= tsgpio_id,
};

static int __init gpio_ts_init(void)
{
	return i2c_add_driver(&gpio_ts_driver);
}
subsys_initcall(gpio_ts_init);

static void __exit gpio_ts_exit(void)
{
	i2c_del_driver(&gpio_ts_driver);
}
module_exit(gpio_ts_exit);

MODULE_AUTHOR("Technologic Systems");
MODULE_DESCRIPTION("GPIO interface for Technologic Systems I2C-FPGA core");
MODULE_LICENSE("GPL");