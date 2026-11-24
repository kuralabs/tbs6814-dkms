/*
    TBS ECP3 FPGA based cards PCIe driver

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "tbsecp3.h"

static bool enable_msi = true;
module_param(enable_msi, bool, 0444);
MODULE_PARM_DESC(enable_msi, "use an msi interrupt if available");

void tbsecp3_gpio_set_pin(struct tbsecp3_dev *dev,
		struct tbsecp3_gpio_pin *pin, int state)
{
	u32 tmp, bank, bit;

	if (pin->lvl == TBSECP3_GPIODEF_NONE)
		return;

	if (pin->lvl == TBSECP3_GPIODEF_LOW)
		state = !state;

	bank = (pin->nr >> 3) & ~3;
	bit = pin->nr % 32;

	tmp = tbs_read(TBSECP3_GPIO_BASE, bank);
	if (state)
		tmp |= 1 << bit;
	else
		tmp &= ~(1 << bit);
	tbs_write(TBSECP3_GPIO_BASE, bank, tmp);
}

static irqreturn_t tbsecp3_irq_handler(int irq, void *dev_id)
{
	struct tbsecp3_dev *dev = (struct tbsecp3_dev *) dev_id;
	struct tbsecp3_i2c *i2c;
	int i, in;

	u32 stat = tbs_read(TBSECP3_INT_BASE, TBSECP3_INT_STAT);

	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_STAT, stat);


	if (stat & 0x00000ff0) {
		
		/* dma0~7 */
		for (i = 0; i < dev->info->adapters; i++) {
			in = dev->adapter[i].cfg->ts_in;
			if (stat & TBSECP3_DMA_IF(in)){
				tasklet_schedule(&dev->adapter[i].tasklet);
				}
		}
	
	}
	if (stat & 0x0000000f) {
		/* i2c */
		for (i = 0; i < 4; i++) {
			i2c = &dev->i2c_bus[i];
			if (stat & TBSECP3_I2C_IF(i)) {
				i2c->done = 1;
				wake_up(&i2c->wq);
			}
		}
	}
	if (stat & 0xf0000000) {
		/* i2c 4~7*/
		for (i = 4; i < 8; i++) {
			i2c = &dev->i2c_bus[i];
			if (stat & TBSECP3_I2C_IF1(i)) {
				i2c->done = 1;
				wake_up(&i2c->wq);
			}
		}
	}
	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_EN, 1);

	/* lets the core spot a stuck line when INTx is shared */
	return stat ? IRQ_HANDLED : IRQ_NONE;
}

static int tbsecp3_adapters_attach(struct tbsecp3_dev *dev)
{
	int i, ret = 0;
	for (i = 0; i < dev->info->adapters; i++) {
		ret = tbsecp3_dvb_init(&dev->adapter[i]);
		if (ret) {
			dev_err(&dev->pci_dev->dev,
				"adapter%d attach failed\n",
				dev->adapter[i].nr);
			dev->adapter[i].nr = -1;
		}
	}
	return 0;
}

static void tbsecp3_adapters_detach(struct tbsecp3_dev *dev)
{
	struct tbsecp3_adapter *adapter;
	int i;

	for (i = 0; i < dev->info->adapters; i++) {
		adapter = &dev->adapter[i];

		/* attach has failed, nothing to do */
		if (adapter->nr == -1)
			continue;

		tbsecp3_dvb_exit(adapter);
		tbsecp3_i2c_remove_clients(adapter);
	}
}

static void tbsecp3_adapters_init(struct tbsecp3_dev *dev)
{
	struct tbsecp3_adapter *adapter = dev->adapter;
	int i;

	for (i = 0; i < dev->info->adapters; i++) {
		adapter = &dev->adapter[i];
		adapter->nr = i;
		adapter->cfg = &dev->info->adap_config[i];
		adapter->dev = dev;
		adapter->i2c = &dev->i2c_bus[adapter->cfg->i2c_bus_nr];
	}
}

static void tbsecp3_adapters_release(struct tbsecp3_dev *dev)
{
	struct tbsecp3_adapter *adapter;
	int i;

	for (i = 0; i < dev->info->adapters; i++) {
		adapter = &dev->adapter[i];
		tasklet_kill(&adapter->tasklet);
	}
}


static int tbsecp3_request_irq(struct tbsecp3_dev *dev)
{
	struct pci_dev *pdev = dev->pci_dev;
	unsigned int types = PCI_IRQ_INTX;
	int ret;

	if (enable_msi)
		types |= PCI_IRQ_MSI;
	else
		dev_info(&pdev->dev,
			"MSI disabled by module parameter 'enable_msi'\n");

	/* prefers MSI when allowed and available, else legacy INTx */
	ret = pci_alloc_irq_vectors(pdev, 1, 1, types);
	if (ret < 0) {
		dev_err(&pdev->dev, "can't allocate an IRQ vector: %d\n", ret);
		return ret;
	}
	dev->msi = pdev->msi_enabled;

	ret = request_irq(pci_irq_vector(pdev, 0), tbsecp3_irq_handler,
			  dev->msi ? 0 : IRQF_SHARED, "tbs6814", dev);
	if (ret) {
		dev_err(&pdev->dev, "can't request IRQ %d: %d\n",
			pci_irq_vector(pdev, 0), ret);
		pci_free_irq_vectors(pdev);
		dev->msi = false;
	}
	return ret;
}

static void tbsecp3_free_irq(struct tbsecp3_dev *dev)
{
	free_irq(pci_irq_vector(dev->pci_dev, 0), dev);
	pci_free_irq_vectors(dev->pci_dev);
	dev->msi = false;
}

static int tbsecp3_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct tbsecp3_dev *dev;
	int ret = -ENODEV;

	if (pci_enable_device(pdev) < 0)
		return -ENODEV;

	/* tbsecp3_dma_reg_init() programs only DMA_ADDRL; DMA_ADDRH is 0 */
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
		dev_err(&pdev->dev, "32-bit PCI DMA not supported\n");
		goto err0;
	}

	pci_set_master(pdev);

	dev = kzalloc(sizeof(struct tbsecp3_dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto err0;
	}

	dev->pci_dev = pdev;
	pci_set_drvdata(pdev, dev);

	dev->info = (struct tbsecp3_board *) id->driver_data;
	dev_info(&pdev->dev, "%s\n", dev->info->name);

	dev->lmmio = ioremap(pci_resource_start(pdev, 0),
				pci_resource_len(pdev, 0));
	if (!dev->lmmio) {
		ret = -ENOMEM;
		goto err1;
	}

	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_EN, 0);
	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_STAT, 0xff);

	tbsecp3_adapters_init(dev);

	/* dma */
	ret = tbsecp3_dma_init(dev);
	if (ret < 0)
		goto err2;

	/* i2c */
	ret = tbsecp3_i2c_init(dev);
	if (ret < 0)
		goto err3;

	/* interrupts */
	ret = tbsecp3_request_irq(dev);
	if (ret < 0)
		goto err4;

	/* global interrupt enable */
	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_EN, 1);

	ret = tbsecp3_adapters_attach(dev);
	if (ret < 0)
		goto err5;

	dev_info(&pdev->dev, "%s: PCI %s, IRQ %d (%s), MMIO 0x%lx\n",
		dev->info->name, pci_name(pdev), pci_irq_vector(pdev, 0),
		dev->msi ? "MSI" : "INTx",
		(unsigned long) pci_resource_start(pdev, 0));

	//dev_info(&dev->pci_dev->dev, "%s ready\n", dev->info->name);
	return 0;

err5:
	tbsecp3_adapters_detach(dev);

	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_EN, 0);
	tbsecp3_free_irq(dev);
err4:
	tbsecp3_i2c_exit(dev);
err3:
	tbsecp3_dma_free(dev);
err2:
	tbsecp3_adapters_release(dev);
	iounmap(dev->lmmio);
err1:
	pci_set_drvdata(pdev, NULL);
	kfree(dev);
err0:
	pci_disable_device(pdev);
	dev_err(&pdev->dev, "probe error\n");
	return ret;
}

static void tbsecp3_remove(struct pci_dev *pdev)
{
	struct tbsecp3_dev *dev = pci_get_drvdata(pdev);

	/* disable interrupts */
	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_EN, 0);
	tbsecp3_free_irq(dev);
	tbsecp3_adapters_detach(dev);
	tbsecp3_adapters_release(dev);
	tbsecp3_dma_free(dev);
	tbsecp3_i2c_exit(dev);
	iounmap(dev->lmmio);
	pci_set_drvdata(pdev, NULL);
	pci_disable_device(pdev);
	kfree(dev);
}

static int tbsecp3_resume(struct pci_dev *pdev)
{
	struct tbsecp3_dev *dev = pci_get_drvdata(pdev);
	/* re-init registers */
	tbsecp3_i2c_reg_init(dev);
	tbsecp3_dma_reg_init(dev);
	tbs_write(TBSECP3_INT_BASE, TBSECP3_INT_EN, 1);
	return 0;
}

/* PCI IDs */
#define TBSECP3_ID(_board_id,_subvend,_subdev) { \
	.vendor = TBSECP3_VID, .device = TBSECP3_PID, \
	.subvendor = _subvend, .subdevice = _subdev, \
	.driver_data = (unsigned long)&tbsecp3_boards[_board_id] }

static const struct pci_device_id tbsecp3_id_table[] = {
	TBSECP3_ID(TBSECP3_BOARD_TBS6814,0x6814,PCI_ANY_ID),
	{0}
};
MODULE_DEVICE_TABLE(pci, tbsecp3_id_table);

static struct pci_driver tbsecp3_driver = {
	.name = "tbs6814",
	.id_table = tbsecp3_id_table,
	.probe    = tbsecp3_probe,
	.remove   = tbsecp3_remove,
	.resume   = tbsecp3_resume,
	.suspend  = NULL,
};

module_pci_driver(tbsecp3_driver);

MODULE_AUTHOR("Luis Alves <ljalvs@gmail.com>");
MODULE_DESCRIPTION("TBS6814 Quad ISDB-T PCIe card driver (cut down from TBS tbsecp3)");
MODULE_LICENSE("GPL");
