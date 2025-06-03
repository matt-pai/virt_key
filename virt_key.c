#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

//#define DEBUG
#ifdef DEBUG
	#define LOG_D(fmt, ...) pr_info("[%s] "fmt, __FUNCTION__, ##__VA_ARGS__)
#else
	#define LOG_D(fmt, ...) ((void)0)
#endif

#define KEY_RELEASE   0
#define KEY_PRESS     1

struct virt_key {
	u32 code;
	u32 state;
	struct input_dev *input;
	struct delayed_work delay_work;
};

struct virt_key_state {
	u32 num_keys;
	struct virt_key *map;
};

static ssize_t dev_attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct input_dev *input = platform_get_drvdata(to_platform_device(dev));
	struct virt_key_state *st = input_get_drvdata(input);
	char *p = buf;
	int i;
	
	p += sprintf(buf, "Support Keys:\n");
	
	for (i = 0; i < st->num_keys; i++) {
		p += sprintf(p, "%d\n", st->map[i].code);
	}
	
    return (p - buf);
}

static ssize_t dev_attr_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
	struct input_dev *input = platform_get_drvdata(to_platform_device(dev));
	struct virt_key_state *st = input_get_drvdata(input);
	struct virt_key *key;
	int keycode, i;

	if ( kstrtouint(buf, 10, &keycode) ) {
		dev_err(dev, "convert keycode fail\n");
		return -EINVAL;
	} 
	
	for (i = 0; i < st->num_keys; i++) {
	
		key = &st->map[i];
			
		if (keycode == key->code && 
			key->state == KEY_RELEASE) {
				
			LOG_D("Press key: %d\n", key->code);
				
			key->state = KEY_PRESS;
			input_report_key(key->input, keycode, 1);
			input_sync(key->input);
				
			queue_delayed_work(system_wq, &key->delay_work, msecs_to_jiffies(1000));
		}
	}
    return count;
}

static DEVICE_ATTR(key, 0664, dev_attr_show, dev_attr_store);

static int virt_key_create_file(struct device *dev)
{
	return device_create_file(dev, &dev_attr_key);
}

static void virt_key_work_handler(struct work_struct *work)
{
	struct delayed_work *delay_work  = container_of(work, struct delayed_work, work);
	struct virt_key *key = container_of(delay_work, struct virt_key, delay_work);
	
    LOG_D("Release key: %d\n", key->code);
	
	input_report_key(key->input, key->code, 0);
	input_sync(key->input);
	key->state = KEY_RELEASE;
}

static int virt_key_init_keys(struct input_dev *input, struct virt_key_state *st)
{
	int num_keys, i;
	struct fwnode_handle *child;
	struct virt_key *keys;
	struct device *dev = input->dev.parent;
	
	num_keys = device_get_child_node_count(dev);
	LOG_D("num keys: %d\n", num_keys);
	if (num_keys == 0) {
		dev_err(dev, "keymap is missing\n");
		return -EINVAL;
	}
	
	keys = devm_kmalloc_array(dev, num_keys, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;
	
	i = 0;
	device_for_each_child_node(dev, child) {
		if (fwnode_property_read_u32(child, "linux,code",
					     &keys[i].code)) {
			dev_err(dev, "button without keycode\n");
			fwnode_handle_put(child);
			return -EINVAL;
		}
		
		keys[i].input = input;
		keys[i].state = KEY_RELEASE;
		INIT_DELAYED_WORK( &keys[i].delay_work , virt_key_work_handler );
		i++;
	}
	
	st->num_keys = num_keys;
	st->map = keys;
	
	return 0;
}

static int virt_key_probe(struct platform_device *pdev)
{
    struct input_dev *input;
	struct device *dev = &pdev->dev;
	struct virt_key_state *st;
	int i, error;
	

	st = devm_kzalloc(dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	
    input = devm_input_allocate_device(dev);
    if (!input) {
        return -ENOMEM;
	}
	
	input_set_drvdata(input, st);
	platform_set_drvdata(pdev, input);

	input->name = pdev->name;
	input->phys = "virtkey/input0";
	input->id.bustype = BUS_HOST;
	
	error = virt_key_init_keys(input, st);
	if (error) {
		dev_err(dev, "Key init fail: %d\n", error);
		return error;
	}
	
	__set_bit(EV_KEY, input->evbit);
	for (i = 0; i < st->num_keys; i++)
		__set_bit(st->map[i].code, input->keybit);


	error = virt_key_create_file(dev);
	if (error) {
		dev_err(dev, "Unable to create device file: %d\n", error);
		return error;
	}
	
    error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device: %d\n", error);
		return error;
	}
	
	return 0;
}

static int virt_key_remove(struct platform_device *pdev)
{
	struct input_dev *input = platform_get_drvdata(pdev);
	struct virt_key_state *st = input_get_drvdata(input);
	struct virt_key *key;
	int i;
	
	for (i = 0; i < st->num_keys; i++) {
		key = &st->map[i];
		cancel_delayed_work_sync(&key->delay_work);
	}
	
	device_remove_file(&pdev->dev, &dev_attr_key);
	
    return 0;
}

static const struct of_device_id virtkey_of_match[] = {
    { .compatible = "matt,virt-keys" },
    {},
};
MODULE_DEVICE_TABLE(of, virtkey_of_match);

static struct platform_driver virtkey_driver = {
	.probe = virt_key_probe,
    .remove = virt_key_remove,
    .driver = {
        .name = "virt_keys",
        .of_match_table = virtkey_of_match,
    },
};

module_platform_driver(virtkey_driver);

MODULE_AUTHOR("matt");
MODULE_DESCRIPTION("test");
MODULE_LICENSE("GPL");
