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
#include "mtv23x.h"

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static int set_mac_address(struct tbsecp3_adapter *adap)
{
	struct tbsecp3_dev *dev = adap->dev;
	u8 eeprom_bus_nr = dev->info->eeprom_i2c;
	struct i2c_adapter *i2c = &dev->i2c_bus[eeprom_bus_nr].i2c_adap;
	u8 eep_addr = 0xa0;
	int ret;

	struct i2c_msg msg[] = {
		{ .addr = 0x50, .flags = 0,
		  .buf = &eep_addr, .len = 1 },
		{ .addr = 0x50, .flags = I2C_M_RD,
		  .buf = adap->dvb_adapter.proposed_mac, .len = 6 }
	};

	if (dev->info->eeprom_addr)
		eep_addr = dev->info->eeprom_addr;

	eep_addr += 0x10 * adap->nr;
	
	ret = i2c_transfer(i2c, msg, 2);
	ret = i2c_transfer(i2c, msg, 2);
	if (ret != 2) {
		dev_warn(&dev->pci_dev->dev,
			"error reading MAC address for adapter %d\n",
			adap->nr);
	} else {
		dev_info(&dev->pci_dev->dev,
			"MAC address %pM\n", adap->dvb_adapter.proposed_mac);
	}
	return 0;
};

static int start_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct tbsecp3_adapter *adapter = dvbdmx->priv;

	if (!adapter->feeds)
		tbsecp3_dma_enable(adapter);

	return ++adapter->feeds;
}

static int stop_feed(struct dvb_demux_feed *dvbdmxfeed)
{
	struct dvb_demux *dvbdmx = dvbdmxfeed->demux;
	struct tbsecp3_adapter *adapter = dvbdmx->priv;

	if (--adapter->feeds)
		return adapter->feeds;

	tbsecp3_dma_disable(adapter);
	return 0;
}

static void reset_demod(struct tbsecp3_adapter *adapter)
{
	struct tbsecp3_dev *dev = adapter->dev;
	struct tbsecp3_gpio_pin *reset = &adapter->cfg->gpio.demod_reset;

	tbsecp3_gpio_set_pin(dev, reset, 1);
	usleep_range(10000, 20000);

	tbsecp3_gpio_set_pin(dev, reset, 0);
	usleep_range(50000, 100000);
}

static int tbsecp3_frontend_attach(struct tbsecp3_adapter *adapter)
{
	struct tbsecp3_dev *dev = adapter->dev;
	struct mtv23x_config mtv23x_config;
	struct i2c_board_info info;
	struct i2c_adapter *i2c = &adapter->i2c->i2c_adap;
	struct i2c_client *client_demod;

	adapter->fe = NULL;
	adapter->i2c_client_demod = NULL;
	adapter->i2c_client_tuner = NULL;

	reset_demod(adapter);
	set_mac_address(adapter);

	switch (dev->info->board_id) {
	case TBSECP3_BOARD_TBS6814:
		memset(&mtv23x_config, 0, sizeof(mtv23x_config));
		mtv23x_config.fe = &adapter->fe;
		mtv23x_config.clk_freq = 32000;
		mtv23x_config.ts_mode  = 6;
		mtv23x_config.i2c_wr_max = 32;

		memset(&info, 0, sizeof(struct i2c_board_info));
		strscpy(info.type, "mtv23x", I2C_NAME_SIZE);
		info.addr = (adapter->nr%2)? 0x44 : 0x43;
		info.platform_data = &mtv23x_config;
		request_module(info.type);
		client_demod = i2c_new_client_device(i2c, &info);
		if (!i2c_client_has_driver(client_demod))
		    goto frontend_atach_fail;
		if (!try_module_get(client_demod->dev.driver->owner)) {
		    i2c_unregister_device(client_demod);
		    goto frontend_atach_fail;
		}
		adapter->i2c_client_demod = client_demod;
		break;

	default:
		dev_warn(&dev->pci_dev->dev, "unknonw card\n");
		return -ENODEV;
		break;
	}
	strscpy(adapter->fe->ops.info.name,dev->info->name,52);
	return 0;

frontend_atach_fail:
	tbsecp3_i2c_remove_clients(adapter);
	if (adapter->fe != NULL)
	    dvb_frontend_detach(adapter->fe);
	adapter->fe = NULL;
	dev_err(&dev->pci_dev->dev, "TBSECP3 frontend %d attach failed\n",
		adapter->nr);

	return -ENODEV;
}

int tbsecp3_dvb_init(struct tbsecp3_adapter *adapter)
{
    struct tbsecp3_dev *dev = adapter->dev;
    struct dvb_adapter *adap = &adapter->dvb_adapter;
    struct dvb_demux *dvbdemux = &adapter->demux;
    struct dmxdev *dmxdev;
    struct dmx_frontend *fe_hw;
    struct dmx_frontend *fe_mem;
    int ret;

    ret = dvb_register_adapter(adap, "TBS6814 DVB Adapter",
            THIS_MODULE,
            &adapter->dev->pci_dev->dev,
            adapter_nr);
    if (ret < 0) {
        dev_err(&dev->pci_dev->dev, "error registering adapter\n");
        if (ret == -ENFILE)
            dev_err(&dev->pci_dev->dev,
                    "increase DVB_MAX_ADAPTERS (%d)\n",
                    DVB_MAX_ADAPTERS);
        return ret;
    }

    adap->priv = adapter;
    dvbdemux->priv = adapter;
    dvbdemux->filternum = 256;
    dvbdemux->feednum = 256;
    dvbdemux->start_feed = start_feed;
    dvbdemux->stop_feed = stop_feed;
    dvbdemux->write_to_decoder = NULL;
    dvbdemux->dmx.capabilities = (DMX_TS_FILTERING |
            DMX_SECTION_FILTERING |
            DMX_MEMORY_BASED_FILTERING);

    ret = dvb_dmx_init(dvbdemux);
    if (ret < 0) {
        dev_err(&dev->pci_dev->dev, "dvb_dmx_init failed\n");
        goto err0;
    }

    dmxdev = &adapter->dmxdev;

    dmxdev->filternum = 256;
    dmxdev->demux = &dvbdemux->dmx;
    dmxdev->capabilities = 0;

    ret = dvb_dmxdev_init(dmxdev, adap);
    if (ret < 0) {
        dev_err(&dev->pci_dev->dev, "dvb_dmxdev_init failed\n");
        goto err1;
    }

    fe_hw = &adapter->fe_hw;
    fe_mem = &adapter->fe_mem;

    fe_hw->source = DMX_FRONTEND_0;
    ret = dvbdemux->dmx.add_frontend(&dvbdemux->dmx, fe_hw);
    if ( ret < 0) {
        dev_err(&dev->pci_dev->dev, "dvb_dmx_init failed");
        goto err2;
    }

    fe_mem->source = DMX_MEMORY_FE;
    ret = dvbdemux->dmx.add_frontend(&dvbdemux->dmx, fe_mem);
    if (ret  < 0) {
        dev_err(&dev->pci_dev->dev, "dvb_dmx_init failed");
        goto err3;
    }

    ret = dvbdemux->dmx.connect_frontend(&dvbdemux->dmx, fe_hw);
    if (ret < 0) {
        dev_err(&dev->pci_dev->dev, "dvb_dmx_init failed");
        goto err4;
    }

    ret = dvb_net_init(adap, &adapter->dvbnet, adapter->dmxdev.demux);
    if (ret < 0) {
        dev_err(&dev->pci_dev->dev, "dvb_net_init failed");
        goto err5;
    }

    tbsecp3_frontend_attach(adapter);
    if (adapter->fe == NULL) {
        dev_err(&dev->pci_dev->dev, "frontend attach failed\n");
        ret = -ENODEV;
        goto err6;
    }

    ret = dvb_register_frontend(adap, adapter->fe);
    if (ret < 0) {
        dev_err(&dev->pci_dev->dev, "frontend register failed\n");
        goto err7;
    }

    return ret;

err7:
    dvb_frontend_detach(adapter->fe);
err6:
    dvb_net_release(&adapter->dvbnet);
err5:
    dvbdemux->dmx.close(&dvbdemux->dmx);
err4:
    dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, fe_mem);
err3:
    dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, fe_hw);
err2:
    dvb_dmxdev_release(dmxdev);
err1:
    dvb_dmx_release(dvbdemux);
err0:
    dvb_unregister_adapter(adap);
    return ret;
}

void tbsecp3_dvb_exit(struct tbsecp3_adapter *adapter)
{
    struct dvb_adapter *adap = &adapter->dvb_adapter;
    struct dvb_demux *dvbdemux = &adapter->demux;

    if (adapter->fe) {
        dvb_unregister_frontend(adapter->fe);
        dvb_frontend_detach(adapter->fe);
        adapter->fe = NULL;
    }
    dvb_net_release(&adapter->dvbnet);
    dvbdemux->dmx.close(&dvbdemux->dmx);
    dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, &adapter->fe_mem);
    dvbdemux->dmx.remove_frontend(&dvbdemux->dmx, &adapter->fe_hw);
    dvb_dmxdev_release(&adapter->dmxdev);
    dvb_dmx_release(&adapter->demux);
    dvb_unregister_adapter(adap);
}
