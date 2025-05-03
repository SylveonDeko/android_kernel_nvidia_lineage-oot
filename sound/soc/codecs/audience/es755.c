/*
 * es755.c  --  Audience eS755 ALSA SoC Audio driver
 *
 * Copyright 2011 Audience, Inc.
 *
 * Author: Greg Clemson <gclemson@audience.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "es755.h"
#include "escore.h"
#include "escore-i2s.h"
#include "escore-spi.h"
#include "es755-access.h"
#include "es-d300.h"
#include "es-a300-reg.h"
#include <linux/sort.h>
#include <linux/clk.h>

union es755_accdet_reg {
	u16 value;
	struct {
		u16 res1:1;
		u16 plug_det_fsm:1;
		u16 debounce_timer:2;
		u16 res2:1;
		u16 mic_det_fsm:1;
		u16 mg_sel_force:1;
		u16 mg_select:2;
	} fields;
};

static const u16 es755_vs_presets[] = {
	[ES755_DMIC0] = ES755_DMIC0_VS_ROUTE_PRESET,
	[ES755_MIC0]  = ES755_MIC0_VS_ROUTE_PRESET,
	[ES755_MICHS] = ES755_MICHS_VS_ROUTE_PRESET,
};

static const u16 es755_cvs_presets[] = {
	[ES755_DMIC0] = ES755_DMIC0_CVS_PRESET,
	[ES755_MIC0]  = ES755_MIC0_CVS_PRESET,
	[ES755_MICHS] = ES755_MICHS_CVS_PRESET,
};

/* codec private data TODO: move to runtime init */
struct escore_priv escore_priv = {
	.probe = es755_core_probe,
	.set_streaming = es755_set_streaming,
	.set_datalogging = es755_set_datalogging,
	.streamdev.no_more_bit = 0,
	.default_cvq_mic = ES755_DMIC0,
	.es_vs_route_preset = ES755_DMIC0_VS_ROUTE_PRESET,
	.es_cvs_preset = ES755_DMIC0_CVS_PRESET,
	.es_wdb_max_size = ES755_WDB_MAX_SIZE,
	.delay.wakeup_to_normal = ES755_DELAY_WAKEUP_TO_NORMAL,
	.delay.wakeup_to_vs = ES755_DELAY_WAKEUP_TO_VS,
	.delay.vs_to_normal = ES755_DELAY_VS_TO_NORMAL,
	.delay.mpsleep_to_normal = ES755_DELAY_MPSLEEP_TO_NORMAL,
	.delay.vs_streaming_to_vs = ES755_DELAY_VS_STREAMING_TO_VS,
};

struct snd_soc_dai_driver es755_dai[];

/*Slimbus channel map for APQ8060*/
static int es755_slim_rx_port_to_ch[ES_SLIM_RX_PORTS] = {
	152, 153, 154, 155, 134, 135
};
static int es755_slim_tx_port_to_ch[ES_SLIM_TX_PORTS] = {
	156, 157, 138, 139, 143, 144
};

static struct clk *codec_clk;

static struct escore_i2s_dai_data i2s_dai_data[ES_NUM_CODEC_I2S_DAIS];
static struct escore_slim_dai_data slim_dai_data[ES_NUM_CODEC_SLIM_DAIS];
static struct escore_slim_ch slim_rx[ES_SLIM_RX_PORTS];
static struct escore_slim_ch slim_tx[ES_SLIM_TX_PORTS];
static const u32 es755_streaming_cmds[] = {
	[ES_INVAL_INTF] = 0x00000000,	/* ES_NULL_INTF */
	[ES_SLIM_INTF]  = 0x90250200,	/* ES_SLIM_INTF */
	[ES_I2C_INTF]   = 0x90250000,	/* ES_I2C_INTF  */
	[ES_SPI_INTF]   = 0x90250300,	/* ES_SPI_INTF  */
	[ES_UART_INTF]  = 0x90250100,	/* ES_UART_INTF */
};

static int es755_clk_ctl(int enable)
{
	int ret = 0;
	bool clk_en;

	clk_en = enable ? true : false;

	if (clk_en == escore_priv.codec_clk_en)
		return 0;

	if (clk_en)
		ret = clk_enable(codec_clk);
	else
		clk_disable(codec_clk);

	if (-EINVAL != ret) {
		escore_priv.codec_clk_en = clk_en;
		ret = 0;
	}
	return ret;
}
static int es755_channel_dir(int dai_id)
{
	int dir = ES_SLIM_CH_UND;

	if (dai_id == ES_SLIM_1_PB ||
			dai_id == ES_SLIM_2_PB ||
			dai_id == ES_SLIM_3_PB) {
		dir = ES_SLIM_CH_RX;
	} else if (dai_id == ES_SLIM_1_CAP ||
			dai_id == ES_SLIM_2_CAP ||
			dai_id == ES_SLIM_3_CAP)  {
		dir = ES_SLIM_CH_TX;
	}

	return dir;
}

static int es755_get_route_status(struct escore_priv *escore, int algo_id)
{
	int ret = 0, value = 0;
	u32 api_word = 0;
	unsigned int msg_len;

	ret = escore_prepare_msg(escore, ES_CHANGE_STATUS, value,
			(char *)&api_word, &msg_len, ES_MSG_READ);
	if (ret) {
		pr_err("%s: Prepare message fail %d\n", __func__, ret);
		goto out;
	}

	/* Set the algo type in message */
	api_word |= algo_id;
	ret = escore_cmd_locked(escore, api_word, &value);
	if (ret < 0) {
		pr_err("%s(): escore_cmd() ret:%d", __func__, ret);
		goto out;
	}
	ret = (value & 0xFF);

out:
	return ret;
}

static ssize_t es755_route_status_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	int ret = 0;
	int value = 0;
	char *status_name = "Route Status";
	struct escore_priv *escore = &escore_priv;
	const char *algo_text[ALGO_MAX] = {
		[VP] = "VP",
		[MM] = "MM",
		[AUDIOFOCUS] = "AudioFocus",
		[PASSTHRU] = "Passthru",
		[VP_MM] = "VP_MM",
		[PASSTHRU_VP] = "Passthru VP",
		[PASSTHRU_MM] = "Passthru MM",
		[PASSTHRU_VP_MM] = "Passthru VP MM",
		[PASSTHRU_AF] = "Passthru AF",
		[VOICEQ] = "VoiceQ",
		[KALA_OK] = "KalaOK",
	};

	const char *status_text[] = {
		"Active",
		"Muting",
		"Switching",
		"Unmuting",
		"Inactive",
	};

	const u8 algo_id[ALGO_MAX] = {
		[VP] = 0,
		[MM] = 1,
		[AUDIOFOCUS] = 2,
		[PASSTHRU] = 3,
		[VOICEQ] = 4,
		[KALA_OK] = 3, /* KalaOK is same as PASSTHRU */
	};

	if (escore->algo_type == ALGO_NONE) {
		pr_err("Algo type not set\n");
		ret = -EINVAL;
		goto out;
	}

	switch (escore->algo_type) {
	case VP:
	case MM:
	case AUDIOFOCUS:
	case PASSTHRU:
	case VOICEQ:
	case KALA_OK:
		value = es755_get_route_status(escore,
				algo_id[escore->algo_type]);
		if (value < 0) {
			pr_err("%s(): GetRouteChangeStatus Failed: %d\n",
					__func__, value);
			ret = value;
			goto out;
		}

		ret = snprintf(buf, PAGE_SIZE, "%s for %s Algo (%d) is %s\n",
				status_name, algo_text[escore->algo_type],
				escore->algo_type, status_text[value]);
		break;
	case PASSTHRU_VP:
	case BARGE_IN:
		{
		int ret1, ret2;
		value = es755_get_route_status(escore, algo_id[VP]);
		if (value < 0) {
			pr_err("%s(): GetRouteChangeStatus Failed: %d\n",
					__func__, value);
			ret = value;
			goto out;
		}

		ret1 = snprintf(buf, PAGE_SIZE, "%s for %s Algo (%d) is %s\n",
				status_name, algo_text[VP],
				VP, status_text[value]);

		value = es755_get_route_status(escore, algo_id[PASSTHRU]);
		if (value < 0) {
			pr_err("%s(): GetRouteChangeStatus Failed: %d\n",
					__func__, value);
			ret = value;
			goto out;
		}

		ret2 = snprintf(buf+ret1, PAGE_SIZE,
				"%s for %s Algo (%d) is %s\n",
				status_name, algo_text[PASSTHRU],
				PASSTHRU, status_text[value]);
		ret = ret1 + ret2;
		break;
		}
	}
out:
	return ret;
}

static DEVICE_ATTR(route_status, 0444, es755_route_status_show, NULL);
/* /sys/devices/platform/msm_slim_ctrl.1/es755-codec-gen0/route_status */

static ssize_t es755_get_pm_enable(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", escore_priv.pm_status ?
			"on" : "off");
}
static ssize_t es755_set_pm_enable(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	pr_info("%s(): requested - %s\n", __func__, buf);
	if (!strncmp(buf, "on", 2))
		escore_pm_status_update(ES_PM_ON);
	else if (!strncmp(buf, "off", 3))
		escore_pm_status_update(ES_PM_OFF);
	return count;

}
static DEVICE_ATTR(pm_enable, 0664, es755_get_pm_enable, es755_set_pm_enable);

#define SIZE_OF_VERBUF 256
/* TODO: fix for new read/write. use es755_read() instead of BUS ops */
static ssize_t es755_fw_version_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int idx = 0;
	unsigned int value;
	char versionbuffer[SIZE_OF_VERBUF];
	char *verbuf = versionbuffer;
	struct escore_priv *escore = &escore_priv;

	memset(verbuf, 0, SIZE_OF_VERBUF);

	mutex_lock(&escore->access_lock);
	value = escore_read(NULL, ES_FW_FIRST_CHAR);
	*verbuf++ = (value & 0x00ff);
	for (idx = 0; idx < (SIZE_OF_VERBUF-2); idx++) {
		value = escore_read(NULL, ES_FW_NEXT_CHAR);
		*verbuf++ = (value & 0x00ff);
		if (!value)
			break;
	}
	mutex_unlock(&escore->access_lock);
	/* Null terminate the string*/
	*verbuf = '\0';
	pr_debug("Audience fw ver %s\n", versionbuffer);
	return snprintf(buf, PAGE_SIZE, "FW Version = %s\n", versionbuffer);
}

static DEVICE_ATTR(fw_version, 0444, es755_fw_version_show, NULL);
/* /sys/devices/platform/msm_slim_ctrl.1/es755-codec-gen0/fw_version */

static ssize_t es755_clock_on_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	int ret = 0;

	return ret;
}

static DEVICE_ATTR(clock_on, 0444, es755_clock_on_show, NULL);
/* /sys/devices/platform/msm_slim_ctrl.1/es755-codec-gen0/clock_on */

static ssize_t es755_reset_control_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	es_d300_reset_cmdcache();
	return 0;
}

static DEVICE_ATTR(reset_control, 0444, es755_reset_control_show, NULL);

static ssize_t es755_ping_status_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct escore_priv *es755 = &escore_priv;
	int rc = 0;
	u32 sync_cmd = (ES_SYNC_CMD << 16) | ES_SYNC_POLLING;
	u32 sync_ack;
	char *status_name = "Ping";

	rc = escore_cmd_locked(es755, sync_cmd, &sync_ack);
	if (rc < 0) {
		pr_err("%s(): firmware load failed sync write %d\n",
		       __func__, rc);
		goto cmd_err;
	}
	pr_debug("%s(): sync_ack = 0x%08x\n", __func__, sync_ack);

	rc = snprintf(buf, PAGE_SIZE,
		       "%s=0x%08x\n",
		       status_name, sync_ack);
cmd_err:
	return rc;
}

static DEVICE_ATTR(ping_status, 0444, es755_ping_status_show, NULL);

#define ESCORE_VERSION "ESCORE_ES75X_1_5_0"
static ssize_t escore_version_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", ESCORE_VERSION);
}
static DEVICE_ATTR(escore_version, 0444, escore_version_show, NULL);

static const char * const es755_power_state_texts[] = {
	"None", "Sleep", "MP_Sleep", "MP_Cmd", "Normal", "Overlay", "Low_Power",
	"Codec Deep Sleep"
};

static ssize_t es755_power_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct escore_priv *es755 = &escore_priv;

	dev_dbg(es755->dev, "%s(): power state = %s\n", __func__,
			es755_power_state_texts[es755->escore_power_state]);
	return snprintf(buf, PAGE_SIZE, "power state = %s\n",
			es755_power_state_texts[es755->escore_power_state]);
}

static DEVICE_ATTR(power_state, 0444, es755_power_state_show, NULL);


/* Display Data format of Codec Control Registers dump */
#define ES_CODEC_REG_DISP_FMT		"Reg = 0x%02X , Value = 0x%02X\n"

/* Store register addresses of 1 byte */
static u8 reg_addr[ES_MAX_CODEC_CONTROL_REG];
/* Store read count of registers to be displayed */
static u8 reg_read_cnt = ES_MAX_CODEC_CONTROL_REG;

/* Device Attribute to dump Codec Control Registers */
static ssize_t codec_reg_dump_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct escore_priv *es755 = &escore_priv;
	int rc = 0;
	int j = 0;
	u32 cmd, resp;
	u8 i, val;
	u8 addr;
	int reg_max = reg_read_cnt;
	u32 max_size_disp_op;

	/* Dump all the registers */
	if (reg_read_cnt == ES_MAX_CODEC_CONTROL_REG)
		reg_max += 1;

	pr_debug("%s(): reg_max = 0x%02x\n", __func__, reg_max);

	/* Error if Display Data exceeds PAGE_SIZE */
	max_size_disp_op  = reg_max * sizeof(ES_CODEC_REG_DISP_FMT);
	if (max_size_disp_op > PAGE_SIZE) {
		pr_err("%s(): Display Data exceeds PAGE_SIZE", __func__);
		return -EINVAL;
	}

	mutex_lock(&es755->access_lock);
	for (i = 0; i < reg_max; i++) {
		if (reg_read_cnt == ES_MAX_CODEC_CONTROL_REG)
			addr = i;
		else
			addr = reg_addr[i];

		cmd = (ES_GET_CODEC_VAL << 16) | (addr << 8);
		rc = escore_cmd(es755, cmd, &resp);
		if (rc < 0) {
			pr_err("%s(): escore_cmd() rc:%d", __func__, rc);
			goto cmd_err;
		}
		pr_debug("%s(): cmd = 0x%08x, resp = 0x%08x\n",
				__func__, cmd, resp);

		val = resp & ES_CODEC_VAL_MASK;

		j += snprintf(buf + j, PAGE_SIZE,
			       ES_CODEC_REG_DISP_FMT,
			       addr, val);
		rc = j;
	}

cmd_err:
	mutex_unlock(&es755->access_lock);
	/* Assign max value for next read operation */
	reg_read_cnt = ES_MAX_CODEC_CONTROL_REG;

	return rc;
}

/* Device Attribute to store address of Codec Control Registers into array */
static ssize_t codec_reg_dump_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int arg_read = 0;
	int pos = 0;
	int bytes_read = 0;
	int in_addr;

	pr_debug("%s(): requested - %s, count=%zu\n", __func__, buf, count);

	/* Return Error for Invalid argument */
	if (strncmp(buf, "0x", 2) || !count) {
		pr_err("%s(): Invalid Argument\n", __func__);
		return -EINVAL;
	}

	/* Reset total for new operation */
	reg_read_cnt = 0;

	while ((bytes_read <= count) &&
			(reg_read_cnt <= ES_MAX_CODEC_CONTROL_REG)) {
		arg_read = sscanf(buf + bytes_read, "%x%n", &in_addr, &pos);
		bytes_read += pos;

		pr_debug("%s(): in_addr = %02x, arg_read=%d, bytes_read=%d\n",
				__func__, in_addr, arg_read, bytes_read);

		/* Skip Invalid characters and register address */
		if (!arg_read || (in_addr > ES_MAX_CODEC_CONTROL_REG))
			continue;

		reg_addr[reg_read_cnt] = in_addr;
		reg_read_cnt++;
	};

	return count;
}
static DEVICE_ATTR(codec_reg_dump, 0664, codec_reg_dump_show,
			codec_reg_dump_store);

static struct attribute *core_sysfs_attrs[] = {
	&dev_attr_route_status.attr,
	&dev_attr_reset_control.attr,
	&dev_attr_clock_on.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_ping_status.attr,
	&dev_attr_pm_enable.attr,
	&dev_attr_escore_version.attr,
	&dev_attr_power_state.attr,
	&dev_attr_codec_reg_dump.attr,
	NULL
};

static struct attribute_group core_sysfs = {
	.attrs = core_sysfs_attrs
};

static int es755_fw_download(struct escore_priv *es755)
{
	int rc;

	pr_debug("%s()\n", __func__);

	/* Reset Mode to Polling */
	es755->cmd_compl_mode = ES_CMD_COMP_POLL;

	if (es755->bus.ops.high_bw_open) {
		rc = es755->bus.ops.high_bw_open(es755);
		if (rc) {
			dev_err(es755->dev, "%s(): high_bw_open failed %d\n",
			__func__, rc);
			goto es755_high_bw_open_failed;
		}
	}

	if (es755->boot_ops.setup) {
		pr_debug("%s(): calling bus specific boot setup\n", __func__);
		rc = es755->boot_ops.setup(es755);
		if (rc != 0) {
			pr_err("%s() bus specific boot setup error %d\n",
			       __func__, rc);
			goto es755_bootup_failed;
		}
	}
	es755->mode = SBL;

	/* Set flag to Wait for API Interrupt */
	escore_set_api_intr_wait(es755);

	rc = es755->bus.ops.high_bw_write(es755, (char *)es755->standard->data,
			      es755->standard->size);
	if (rc < 0) {
		pr_err("%s(): firmware download failed %d\n",
		       __func__, rc);
		rc = -EIO;
		goto es755_bootup_failed;
	}

	if (es755->boot_ops.finish) {
		/* Give the chip some time to become ready after firmware
		 * download. */
		msleep(20);
		pr_debug("%s(): calling bus specific boot finish\n", __func__);
		rc = es755->boot_ops.finish(es755);
		if (rc != 0) {
			pr_err("%s() bus specific boot finish error %d\n",
			       __func__, rc);
			goto es755_bootup_failed;
		}
	}
	es755->mode = STANDARD;

	/* Reconfig API Interrupt mode */
	rc = escore_reconfig_api_intr(es755);
	if (rc)
		goto es755_bootup_failed;

es755_bootup_failed:
	if (es755->bus.ops.high_bw_close) {
		int ret = 0;
		ret = es755->bus.ops.high_bw_close(es755);
		if (ret) {
			dev_err(es755->dev, "%s(): high_bw_close failed %d\n",
				__func__, ret);
			rc = ret;
		}
	}

es755_high_bw_open_failed:
	return rc;
}

/* This function must be called with access_lock acquired */
int es755_bootup(struct escore_priv *es755)
{
	u8 retry = ES755_FW_DOWNLOAD_MAX_RETRY;
	int rc;

	pr_debug("%s()\n", __func__);

	BUG_ON(es755->standard->size == 0);

	do {
		if (retry < ES755_FW_DOWNLOAD_MAX_RETRY)
			escore_gpio_reset(es755);
		rc = es755_fw_download(es755);
	} while (rc && retry--);

	if (rc) {
		dev_err(es755->dev, "%s(): STANDARD fw download error %d\n",
								__func__, rc);
	}

	return rc;
}


static int es755_slim_set_channel_map(struct snd_soc_dai *dai,
				      unsigned int tx_num,
				      const unsigned int *tx_slot,
				      unsigned int rx_num,
				      const unsigned int *rx_slot)
{
	struct snd_soc_component *component = dai->component;
	struct escore_priv *escore = &escore_priv;
	int id = dai->id;
	int i;
	int rc = 0;

	dev_dbg(component->dev, "%s(): dai->name = %s, dai->id = %d\n", __func__,
		dai->name, dai->id);

	if (id == ES_SLIM_1_PB ||
	    id == ES_SLIM_2_PB ||
	    id == ES_SLIM_3_PB) {
		escore->slim_dai_data[DAI_INDEX(id)].ch_tot = rx_num;
		escore->slim_dai_data[DAI_INDEX(id)].ch_act = 0;
		for (i = 0; i < rx_num; i++)
			escore->slim_dai_data[DAI_INDEX(id)].ch_num[i] =
				rx_slot[i];
	} else if (id == ES_SLIM_1_CAP ||
		 id == ES_SLIM_2_CAP ||
		 id == ES_SLIM_3_CAP) {
		escore->slim_dai_data[DAI_INDEX(id)].ch_tot = tx_num;
		escore->slim_dai_data[DAI_INDEX(id)].ch_act = 0;
		for (i = 0; i < tx_num; i++)
			escore->slim_dai_data[DAI_INDEX(id)].ch_num[i] =
				tx_slot[i];
	}

	return rc;
}

static void es755_shutdown(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct escore_priv *escore = snd_soc_component_get_drvdata(component);
	int id = DAI_INDEX(dai->id);

	dev_dbg(component->dev, "%s(): dai->name = %s, dai->id = %d\n", __func__,
		dai->name, dai->id);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		escore->i2s_dai_data[id].rx_ch_tot = 0;
	else
		escore->i2s_dai_data[id].tx_ch_tot = 0;

	if (escore->can_mpsleep)
		escore->can_mpsleep = 0;
}

static int es755_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	int id = DAI_INDEX(dai->id);

	dev_dbg(component->dev, "%s(): dai->name = %s, dai->id = %d, fmt = %x\n",
		__func__, dai->name, dai->id, fmt);

	/* set master/slave audio interface */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		/* es755 as master */
		i2s_dai_data[id].port_mode = ES_PCM_PORT_MASTER;
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		/* es755 as slave */
		i2s_dai_data[id].port_mode = ES_PCM_PORT_SLAVE;
		break;
	default:
		dev_err(component->dev, "%s(): unsupported DAI clk mode\n",
			__func__);
		return -EINVAL;
	}
	dev_dbg(component->dev, "%s(): clk mode = %d\n", __func__,
			i2s_dai_data[id].port_mode);

	return 0;
}

static int es755_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct escore_priv *escore = snd_soc_component_get_drvdata(component);
	struct escore_api_access *api_access;
	int rc = 0;
	int channels;
	int bps = 0;
	int rate = 0;
	int id = DAI_INDEX(dai->id);
	u16 clock_control = 0;
	u16 codec_output_rate = 0;
	int port_map = 0;
	u8 pcm_port[] = { ES755_PCM_PORT_A,
		ES755_PCM_PORT_B,
		ES755_PCM_PORT_C };

	dev_dbg(component->dev, "%s(): dai->name = %s, dai->id = %d\n", __func__,
		dai->name, dai->id);

	mutex_lock(&escore->access_lock);
	rc =  es_change_power_state_normal(escore);
	mutex_unlock(&escore->access_lock);

	if (rc) {
		dev_err(component->dev, "%s(): Power state transition failed %d\n",
				__func__, rc);
		return rc;
	}

	channels = params_channels(params);
	switch (channels) {
	case 1:
	case 2:
	case 3:
	case 4:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			escore->i2s_dai_data[id].rx_ch_tot = channels;
			escore->i2s_dai_data[id].rx_ch_act = 0;
		} else {
			escore->i2s_dai_data[id].tx_ch_tot = channels;
			escore->i2s_dai_data[id].tx_ch_act = 0;
		}
		break;
	default:
		dev_err(component->dev,
			"%s(): unsupported number of channels, %d\n",
			__func__, channels);
		return -EINVAL;
	}


	escore->pcm_format = params_format(params);
	/* ######## HACK : Set PCM format as 32bit ####### */
	if (escore->algo_type == DHWPT || escore->dhwpt_enabled)
		escore->pcm_format = SNDRV_PCM_FORMAT_S32_LE;

	switch (escore->pcm_format) {
	case SNDRV_PCM_FORMAT_A_LAW:
		bps = 0x0207;
		break;
	case SNDRV_PCM_FORMAT_MU_LAW:
		bps = 0x0107;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S16_BE:
		bps = 0xF;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
	case SNDRV_PCM_FORMAT_S20_3BE:
		bps = 0x13;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_BE:
		bps = 0x17;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S32_BE:
		bps = 0x1F;
		break;
	default:
		dev_err(component->dev, "%s(): Unsupported format :%d\n",
				__func__, params_format(params));
		break;
	}

	if (bps) {
		api_access = &escore->api_access[ES_PORT_WORD_LEN];

		/* Update the Port info in write command */
		api_access->write_msg[0] |=  ES_API_WORD(ES_SET_DEV_PARAM_ID,
				(pcm_port[id] << 8));

		rc = escore_write_locked(component, ES_PORT_WORD_LEN, bps);
		if (rc) {
			pr_err("%s(): Preparing write message failed %d\n",
			       __func__, rc);
			return rc;
		}
		/* Clear the Port info in write command */
		api_access->write_msg[0] &= ES_API_WORD(ES_SET_DEV_PARAM_ID,
				0x00ff);
	}

	switch (params_rate(params)) {
	case 8000:
		rate = 8;
		break;
	case 11025:
		rate = 11;
		break;
	case 12000:
		rate = 12;
		break;
	case 16000:
		rate = 16;
		break;
	case 22050:
		rate = 22;
		break;
	case 24000:
		rate = 24;
		break;
	case 32000:
		rate = 32;
		break;
	case 44100:
		rate = 44;
		break;
	case 48000:
		rate = 48;
		break;
	case 96000:
		rate = 96;
		break;
	case 192000:
		rate = 192;
		break;
	default:
		pr_err("%s: Unsupported sampling rate %d\n", __func__,
				params_rate(params));
		return -EINVAL;
	}
	dev_dbg(component->dev, "%s(): params_rate(params) = %d\n",
			__func__, params_rate(params));

	api_access = &escore->api_access[ES_PORT_CLOCK_CONTROL];

	/* Update the Port info in write command */
	api_access->write_msg[0] |=  ES_API_WORD(ES_SET_DEV_PARAM_ID,
			(pcm_port[id] << 8));

	clock_control  = rate | (i2s_dai_data[id].port_mode << 8);

	rc = escore_write_locked(component, ES_PORT_CLOCK_CONTROL, clock_control);
	if (rc) {
		pr_err("%s(): Preparing write message failed %d\n",
		       __func__, rc);
		return rc;
	}

	/* Clear the Port info in write command */
	api_access->write_msg[0] &= ES_API_WORD(ES_SET_DEV_PARAM_ID, 0x00ff);

	/* Set codec output rate  */
	if (escore->dhwpt_enabled || escore->algo_type == DHWPT) {
		switch (params_rate(params)) {
		case 44100:
			codec_output_rate = 0x7;
			break;
		case 48000:
			codec_output_rate = 0x6;
			break;
		case 96000:
		case 192000:
			codec_output_rate = 0x2;
			break;
		default:
			pr_err("%s: Unsupported sampling rate %d for DHWPT\n",
				__func__, params_rate(params));
			return -EINVAL;
		}
	}
	rc = escore_write_locked(component, ES_CODEC_OUTPUT_RATE,
						codec_output_rate);
	if (rc) {
		pr_err("%s(): Preparing write message failed %d\n",
				__func__, rc);
		return rc;
	}

	/*
	 * To enter into MP_SLEEP mode during playback, a minimum 3Mhz clock
	 * is required. For that, minimum 48Khz sample rate, 32 bit word length
	 * and 2 channels are required.
	 */
	escore->can_mpsleep = (rate >= 48) && (bps >= 0x1F) && (channels >= 2);

	dev_dbg(component->dev, "%s(): # Rate %d BPS %d channels %d #\n", __func__,
		rate, bps, channels);

	switch (dai->id) {
	case ES_I2S_PORTA:
		port_map = PORT_A_TO_D;
		break;
	case ES_I2S_PORTB:
		port_map = PORT_B_TO_D;
		break;
	case ES_I2S_PORTC:
		port_map = PORT_C_TO_D;
		break;
	}

	if (escore->algo_type == DHWPT || escore->dhwpt_enabled)
		escore->dhwpt_cmd = (ES_DHWPT_CMD << 16) | port_map;

	/* BAS-3205, if frame size is 64 bits and word length is 16 bits, set
	 * TDM Timeslots Per Frame of PortA to 3  */
	if (escore->algo_type != DHWPT && !escore->dhwpt_enabled) {
		if (32 == (bps + 1)*channels && 0xF == bps) {
			api_access =
			     &escore->api_access[ES_PORT_TDM_SLOTS_PER_FRAME];
			api_access->write_msg[0] |=
				ES_API_WORD(ES_SET_DEV_PARAM_ID,
				(ES755_PCM_PORT_A << 8));

			rc = escore_write_locked(component,
					ES_PORT_TDM_SLOTS_PER_FRAME,
					3);
			if (rc) {
				pr_err("%s(): Set TDM slots per frame fail %d\n",
						__func__, rc);
				return rc;
			}

			api_access->write_msg[0] &=
				ES_API_WORD(ES_SET_DEV_PARAM_ID, 0x00ff);
		}
	}

	dev_dbg(component->dev, "%s(): params_channels(params) = %d\n", __func__,
		channels);

	return rc;
}

struct snd_soc_dai_driver es755_dai[] = {
	{
		.name = "earSmart-porta",
		.id = ES_I2S_PORTA,
		.playback = {
			.stream_name = "PORTA Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = ES_RATES,
			.formats = ES_FORMATS,
		},
		.capture = {
			.stream_name = "PORTA Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = ES_RATES,
			.formats = ES_FORMATS,
		},
		.ops = &escore_i2s_port_dai_ops,
	},
	{
		.name = "earSmart-portb",
		.id = ES_I2S_PORTB,
		.playback = {
			.stream_name = "PORTB Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = ES_RATES,
			.formats = ES_FORMATS,
		},
		.capture = {
			.stream_name = "PORTB Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = ES_RATES,
			.formats = ES_FORMATS,
		},
		.ops = &escore_i2s_port_dai_ops,
	},
	{
		.name = "earSmart-portc",
		.id = ES_I2S_PORTC,
		.playback = {
			.stream_name = "PORTC Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = ES_RATES,
			.formats = ES_FORMATS,
		},
		.capture = {
			.stream_name = "PORTC Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = ES_RATES,
			.formats = ES_FORMATS,
		},
		.ops = &escore_i2s_port_dai_ops,
	},
};

static void es755_mp_sleep_work(struct work_struct *work)
{
	struct escore_priv *escore = &escore_priv;

	dev_dbg(escore->dev, "%s() noop\n", __func__);
}

int es755_enter_mp_sleep(void)
{
	struct escore_priv *escore = &escore_priv;
	int ret = 0;
	int rc  = 0;

	ret = queue_delayed_work(escore->power_transition_wq,
				 &(escore->mp_sleep_work),
				 msecs_to_jiffies(ES755_MP_SLEEP_DELAY));
		dev_dbg(escore->dev, "%s() queue_delayed_work() ret = %d\n",
			__func__, ret);

	dev_dbg(escore->dev, "%s() queue_delayed_work() ret = %d\n",
		__func__, ret);
	return rc;
}

int es755_exit_mp_sleep(void)
{
	struct escore_priv *escore = &escore_priv;
	int rc = 0;
	int ret = 0;

	rc = cancel_delayed_work_sync(&escore->mp_sleep_work);
	dev_dbg(escore->dev, "%s() cancel_delayed_work_sync() rc = %d\n",
		__func__, rc);

	if (escore->escore_power_state == ES_SET_POWER_STATE_MP_SLEEP) {
		dev_dbg(escore->dev, "%s(): noop\n", __func__);
	}

	return ret;
}

static int es755_set_bias_level(struct snd_soc_component *component,
				      enum snd_soc_bias_level level)
{
	pr_debug("%s(): Setting bias level to :%d\n", __func__, level);
	//codec->dapm.bias_level = level;
	return 0;
}


static void es755_init_regs(struct escore_priv *es755)
{
	int reg_index;

	for (reg_index = 0; reg_index < ES_MAX_REGISTER; reg_index++) {
		/* Initialize values with invalid values */
		es755->reg_cache[reg_index].value = ES_INVALID_REG_VALUE;
	}
}

static int es755_component_probe(struct snd_soc_component *component)
{
	int ret;
	struct escore_priv *es755 = snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "%s()\n", __func__);
	es755->component = component;

	es755_init_regs(es755);

	ret = es_d300_add_snd_soc_controls(component);
	if (ret) {
		dev_err(component->dev,
			"%s(): es_d300_snd_controls fail %d\n",
			__func__, ret);
		return ret;
	}
	ret = es_analog_add_snd_soc_controls(component);
	if (ret) {
		dev_err(component->dev,
			"%s(): es_analog_snd_controls fail %d\n",
			__func__, ret);
		return ret;
	}
	ret = es_d300_add_snd_soc_dapm_controls(component);
	if (ret) {
		dev_err(component->dev,
			"%s(): es_d300_dapm_widgets fail %d\n",
			__func__, ret);
		return ret;
	}
	ret = es_analog_add_snd_soc_dapm_controls(component);
	if (ret) {
		dev_err(component->dev,
			"%s(): es_analog_dapm_widgets fail %d\n",
			__func__, ret);
		return ret;
	}
	ret = es_d300_add_snd_soc_route_map(component);
	if (ret) {
		dev_err(component->dev,
			"%s(): es_d300_add_routes fail %d\n",
			__func__, ret);
		return ret;
	}
	ret = es_analog_add_snd_soc_route_map(component);
	if (ret) {
		dev_err(component->dev,
			"%s(): es_analog_add_routes fail %d\n",
			__func__, ret);
		return ret;
	}

	ret = es_d300_fill_cmdcache(escore_priv.component);
	if (ret) {
		dev_err(component->dev,
			"%s(): Cache initialization fail %d\n",
			__func__, ret);
		return ret;
	}

	es755_set_bias_level(component, SND_SOC_BIAS_STANDBY);
	return ret;
}

static void es755_component_remove(struct snd_soc_component *component)
{

	pr_debug("%s(): Codec removed\n", __func__);
}

static unsigned int es755_component_read(struct snd_soc_component *component,
				unsigned int reg)
{
	struct escore_priv *escore = &escore_priv;
	u32 value = 0;
	int rc;
	int ret = 0;

	if (reg > ES_MAX_REGISTER) {
		/*dev_err(component->dev, "read out of range reg %d", reg);*/
		return 0;
	}

	if (escore->reg_cache[reg].value != ES_INVALID_REG_VALUE) {
		value = escore->reg_cache[reg].value;
		return value;
	}

	mutex_lock(&escore->access_lock);

	if (escore->dhwpt_mode) {
		/* Bring the chip from MP Sleep to normal state before
		 * sending any analog commands*/
		rc = es755_exit_mp_sleep();
		if (rc) {
			dev_err(component->dev,
			"%s(): es755_exit_mp_sleep() fail %d\n",
			__func__, rc);
			goto out;
		}
	}

	ret = escore_write(component, ES_CODEC_ADDR, reg);
	if (ret < 0) {
		dev_err(component->dev, "component reg %x write err %d\n",
			reg, ret);
		goto out;
	}

	value = escore_read(NULL, ES_CODEC_VALUE);
	rc = value;
	if (value < 0) {
		pr_err("%s(): ES_CODEC_VALUE read fail %d\n", __func__, rc);
		goto out;
	}
	escore->reg_cache[reg].value = value;

	if (escore->mp_sleep_requested && escore->dhwpt_mode &&
			!escore->dhwpt_capture_on
			&& escore->acc_det_status != ES_ACC_BUTTON_PRESSED) {
		pr_debug("%s(): Put chip back into mp_sleep\n", __func__);
		es755_enter_mp_sleep();
	}
out:
	mutex_unlock(&escore->access_lock);
	return (rc < 0) ? rc : value;

}

static int es755_component_write(struct snd_soc_component *component, unsigned int reg,
	unsigned int value)
{
	struct escore_priv *escore = &escore_priv;
	int ret = 0;

	if (reg > ES_MAX_REGISTER) {
		/*dev_err(component->dev, "write out of range reg %d", reg);*/
		return 0;
	}

	mutex_lock(&escore->access_lock);

	if (escore->dhwpt_mode) {
		/* Bring the chip from MP Sleep to normal state before
		 * sending any analog commands*/
		ret = es755_exit_mp_sleep();
		if (ret) {
			dev_err(component->dev,
				"%s(): es755_exit_mp_sleep() fail %d\n",
				__func__, ret);
			goto out;
		}
	}

	if (atomic_read(&escore->active_streams) > 0)
		usleep_range(1000, 1005);

	ret = escore_write(component, ES_CODEC_ADDR, reg);
	if (ret < 0) {
		dev_err(component->dev, "codec ES_CODEC_ADDR, %x write err %d\n",
			reg, ret);
		goto out;
	}

	ret = escore_write(component, ES_CODEC_VALUE, value);
	if (ret < 0) {
		dev_err(component->dev, "codec ES_CODEC_VALUE, %x write err %d\n",
			reg, ret);
		goto out;
	}
	escore->reg_cache[reg].value = value;

	if (escore->mp_sleep_requested && escore->dhwpt_mode &&
			!escore->dhwpt_capture_on &&
			escore->acc_det_status != ES_ACC_BUTTON_PRESSED) {
		pr_debug("%s(): Put chip back into mp_sleep\n", __func__);
		es755_enter_mp_sleep();
	}
out:
	mutex_unlock(&escore->access_lock);
	return ret;
}

struct snd_soc_component_driver soc_component_dev_es755 = {
	.probe =	es755_component_probe,
	.remove =	es755_component_remove,
	.read =		es755_component_read,
	.write =	es755_component_write,
	.set_bias_level =	es755_set_bias_level,
};

int es755_set_streaming(struct escore_priv *escore, int value)
{
	u32 resp;
	return escore_cmd_locked(escore,
		es755_streaming_cmds[escore->streamdev.intf] | value, &resp);
}

int es755_set_datalogging(struct escore_priv *escore, int value)
{
	u32 resp;
	return escore_cmd_locked(escore, value, &resp);
}

void es755_slim_setup(struct escore_priv *escore_priv)
{
	int i;
	int ch_cnt;

	escore_priv->init_slim_slave(escore_priv);

	/* allocate ch_num array for each DAI */
	for (i = 0; i < ARRAY_SIZE(es755_dai); i++) {
		switch (es755_dai[i].id) {
		case ES_SLIM_1_PB:
		case ES_SLIM_2_PB:
		case ES_SLIM_3_PB:
			ch_cnt = es755_dai[i].playback.channels_max;
			break;
		case ES_SLIM_1_CAP:
		case ES_SLIM_2_CAP:
		case ES_SLIM_3_CAP:
			ch_cnt = es755_dai[i].capture.channels_max;
			break;
		default:
				continue;
		}
		escore_priv->slim_dai_data[i].ch_num =
			kzalloc((ch_cnt * sizeof(unsigned int)), GFP_KERNEL);
	}
	/* front end for RX1 */
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_1_PB)].ch_num[0] = 152;
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_1_PB)].ch_num[1] = 153;
	/* back end for RX1 */
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_2_CAP)].ch_num[0] = 138;
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_2_CAP)].ch_num[1] = 139;
	/* front end for TX1 */
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_1_CAP)].ch_num[0] = 156;
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_1_CAP)].ch_num[1] = 157;
	/* back end for TX1 */
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_3_PB)].ch_num[0] = 134;
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_3_PB)].ch_num[1] = 135;
	/* front end for RX2 */
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_2_PB)].ch_num[0] = 154;
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_2_PB)].ch_num[1] = 155;
	/* back end for RX2 */
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_3_CAP)].ch_num[0] = 143;
	escore_priv->slim_dai_data[DAI_INDEX(ES_SLIM_3_CAP)].ch_num[1] = 144;
}

static int es755_config_jack(struct escore_priv *escore)
{
	struct esxxx_accdet_config *accdet_cfg = &escore->pdata->accdet_cfg;
	union es755_accdet_reg accdet_reg;
	u32 cmd;
	u32 resp;
	int rc;

	/* Setup the Event response */
	cmd = (ES_SET_EVENT_RESP << 16) | IRQF_TRIGGER_HIGH;
	rc = escore_cmd_nopm(escore, cmd, &resp);
	if (rc < 0) {
		pr_err("%s(): Error %d in setting event response\n",
				__func__, rc);
		goto out;
	}

	accdet_reg.value = 0;
	accdet_reg.fields.plug_det_fsm = accdet_cfg->plug_det_enabled & (0x1);
	accdet_reg.fields.debounce_timer = accdet_cfg->debounce_timer & (0x3);

	/* Setup the debounce timer for plug event */
	cmd = (ES_ACCDET_CONFIG_CMD << 16) | (accdet_reg.value);
	rc = escore_cmd_nopm(escore, cmd, &resp);
	if (rc < 0)
		pr_err("%s(): Error %d in setting debounce timer\n",
				__func__, rc);

out:
	return rc;
}

int es755_detect(struct snd_soc_component *component, struct snd_soc_jack *jack)
{
	struct escore_priv *escore = snd_soc_component_get_drvdata(component);
	int rc;

	mutex_lock(&escore->access_lock);
	rc = escore_pm_get_sync();
	if (rc > -1) {
		rc = es755_config_jack(escore);
		escore_pm_put_autosuspend();
	}
	mutex_unlock(&escore->access_lock);

	if (rc >= 0)
		escore->jack = jack;

	return rc;
}
EXPORT_SYMBOL_GPL(es755_detect);


static struct esxxx_platform_data *es755_populate_dt_pdata(struct device *dev)
{
	struct esxxx_platform_data *pdata;
	struct property *prop;
	u8 *temp;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "could not allocate memory for platform data\n");
		return NULL;
	}

	pdata->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata->reset_gpio))
		dev_err(dev, "Looking up reset-gpios property in node %s failed %ld\n",
				dev->of_node->full_name, PTR_ERR(pdata->reset_gpio));

	pdata->wakeup_gpio = devm_gpiod_get(dev, "device-wakeup", GPIOD_OUT_LOW);
	if (IS_ERR(pdata->wakeup_gpio))
		dev_err(dev, "Looking up device-wakup-gpios property in node %s failed %ld\n",
				dev->of_node->full_name, PTR_ERR(pdata->wakeup_gpio));

	prop = of_find_property(dev->of_node, "adnc,enable_hs_uart_intf", NULL);
	if (prop != NULL) {
		temp =	(u8 *)prop->value;
		if (temp[3] == 1)
			pdata->enable_hs_uart_intf = true;
		else
			pdata->enable_hs_uart_intf = false;
	}

	prop = of_find_property(dev->of_node, "adnc,ext_clk_rate", NULL);
	if (prop != NULL) {
		temp = (u8 *)prop->value;
		if (temp[3] != 0)
			pdata->ext_clk_rate = temp[3];
	}

	prop = of_find_property(dev->of_node, "adnc,debounce_timer", NULL);
	if (prop != NULL) {
		temp = (u8 *)prop->value;
		if (temp[3] != 0)
			pdata->accdet_cfg.debounce_timer = temp[3];
	}

	prop = of_find_property(dev->of_node, "adnc,plug_det_enabled", NULL);
	if (prop != NULL) {
		temp = (u8 *)prop->value;
		if (temp[3] != 0)
			pdata->accdet_cfg.plug_det_enabled = temp[3];
	}

	prop = of_find_property(dev->of_node, "adnc,mic_det_enabled", NULL);
	if (prop != NULL) {
		temp = (u8 *)prop->value;
		if (temp[3] != 0)
			pdata->accdet_cfg.mic_det_enabled = temp[3];
	}

/*
	prop = of_find_property(dev->of_node, "adnc,hs_mg_select", NULL);
	if (prop != NULL) {
		temp = (u8 *)prop->value;
		if (temp[3] != 0)
			pdata->accdet_cfg.hs_mg_select = temp[3];
	}
*/
	pdata->accdet_cfg.hs_mg_select = 2;


	prop = of_find_property(dev->of_node, "adnc,cmd_comp_mode", NULL);
	if (prop != NULL) {
		temp = (u8 *)prop->value;
		if (temp[3] != 0)
			pdata->cmd_comp_mode = temp[3];
	}

	codec_clk = clk_get(dev, "es755-mclk");
	if (IS_ERR(codec_clk)) {
		dev_err(dev, "%s: Failed to request es755 mclk from pmic %ld\n",
				__func__, PTR_ERR(codec_clk));
		pdata->esxxx_clk_cb = NULL;
	} else {
		pdata->esxxx_clk_cb = es755_clk_ctl;
		pdata->esxxx_clk_cb(1);
	}

	return pdata;
}


static int es755_mic_config(struct escore_priv *escore)
{
	struct snd_soc_component *component = escore->component;
	union es755_accdet_reg accdet_reg;
	struct esxxx_accdet_config accdet_cfg = escore->pdata->accdet_cfg;

	accdet_reg.value = 0;

	accdet_reg.fields.mic_det_fsm = 1;
	accdet_reg.fields.plug_det_fsm = accdet_cfg.plug_det_enabled & (0x1);
	accdet_reg.fields.debounce_timer = accdet_cfg.debounce_timer & (0x3);
	/*
	 * BAS-3584: As per updated accessory detection logic, always set
	 * mg_sel_force bit to 1 and mg_select bits as per required
	 * configuration - LRGM: 0, LRMG: 1, Both: 2
	 * */
	accdet_reg.fields.mg_sel_force = 1;
	accdet_reg.fields.mg_select = accdet_cfg.hs_mg_select;

	pr_debug("%s()\n", __func__);

	return escore_write_locked(component, ES_ACCDET_CONFIG, accdet_reg.value);
}

static int es755_button_config(struct escore_priv *escore)
{
	union es755_btn_ctl1 btn_ctl1;
	int rc = 0;

	btn_ctl1.value = 0;
	/* Enable Button Press Detection */
	btn_ctl1.fields.btn_press_det = ES_BTN_DET_EN;
	mutex_lock(&escore->access_lock);

	/* Read default value */
	rc = escore_read(NULL, ES_BUTTON_CTRL1);
	if (rc < 0) {
		dev_err(escore_priv.dev,
			"%s(): ES_BUTTON_CTRL Read fail %d\n",
			__func__, rc);
		goto btn_cfg_exit;
	}

	/* Enable btn_press_det bit */
	btn_ctl1.value |= rc;

	/* Write updated value with Button Press Detection enabled */
	rc = escore_write(NULL, ES_BUTTON_CTRL1, btn_ctl1.value);
	if (rc < 0)
		pr_err("%s(): Error setting button control 1 %d\n",
		       __func__, rc);
btn_cfg_exit:
	mutex_unlock(&escore->access_lock);
	return rc;

}

static int chk_btn_press_event(
	struct escore_priv *escore, int sys_intr_resp)
{
	int rc = 0, value;
	union es755_accdet_status_reg accdet_status_reg;

	if (ES_BUTTON_PRESS_EVENT(sys_intr_resp)) {
		pr_debug("%s(): Button press event\n", __func__);

		/*
		 * Due to FW constraint, wait for 65ms before Button
		 * Detection
		 */
		msleep(65);

		value = escore_read_locked(NULL, ES_GET_ACCDET_STATUS);
		if (value < 0) {
			pr_err("%s(): Accessory detect status fail %d\n",
			       __func__, value);
			rc = value;
			goto err_exit;
		}

		accdet_status_reg.value = value;

		pr_debug("Impd:%d\n", accdet_status_reg.fields.impd_level);

		switch (accdet_status_reg.fields.impd_level) {
		case 0:
		case 1:
			snd_soc_jack_report(escore->jack,
					    SND_JACK_BTN_0, SND_JACK_BTN_0);
			escore->acc_btn_pressed = SND_JACK_BTN_0;
			pr_info("%s(): Button-0 Press event\n", __func__);
			break;

		case 2:
			snd_soc_jack_report(escore->jack,
					    SND_JACK_BTN_1, SND_JACK_BTN_1);
			escore->acc_btn_pressed = SND_JACK_BTN_1;
			pr_info("%s(): Button-1 Press event\n", __func__);
			break;

		case 3:
			snd_soc_jack_report(escore->jack,
					    SND_JACK_BTN_2, SND_JACK_BTN_2);
			escore->acc_btn_pressed = SND_JACK_BTN_2;
			pr_info("%s(): Button-2 Press event\n", __func__);
			break;

		default:
			escore->acc_btn_pressed = ES_ACC_BTN_INVALID;
			pr_err("Invalid button type : %d\n",
			       accdet_status_reg.fields.impd_level);
			break;
		}

		/* Avoid chip suspend when button is still pressed */
		if (escore->acc_det_status != ES_ACC_BUTTON_PRESSED) {
			rc = escore_pm_get_sync();
			if (rc < 0) {
				pr_err("%s(): pm_get_sync failed :%d\n",
				       __func__, rc);
				goto err_exit;
			}
			escore->acc_det_status = ES_ACC_BUTTON_PRESSED;
		}
	}

err_exit:
	return rc;
}

static int chk_btn_release_event(
	struct escore_priv *escore, int sys_intr_resp)
{
	int rc = 0;

	if (ES_BUTTON_RELEASE_EVENT(sys_intr_resp)) {
		pr_debug("%s(): Button release event\n", __func__);

		/*
		 * If user presses button for more than autosuspend
		 * time, firmware should not be put into suspend mode
		 * until button is released OR it is unplugged.
		 * Put the chip back to autosuspend when user releases
		 * the button.
		 */
		if (escore->acc_det_status == ES_ACC_BUTTON_PRESSED) {
			escore_pm_put_autosuspend();
			if (escore->mp_sleep_requested && escore->dhwpt_mode
						&& !escore->dhwpt_capture_on) {
				pr_debug("%s(): Put chip back into mp_sleep\n",
							__func__);
				es755_enter_mp_sleep();
			}
		}

		pr_debug("button type : %d\n", escore->acc_btn_pressed);

		switch (escore->acc_btn_pressed) {
		case SND_JACK_BTN_0:
			/* Button release event must be sent */
			snd_soc_jack_report(escore->jack, 0, SND_JACK_BTN_0);

			pr_info("%s(): Button-0 release event\n", __func__);
			break;

		case SND_JACK_BTN_1:
			/* Button release event must be sent */
			snd_soc_jack_report(escore->jack, 0, SND_JACK_BTN_1);
			pr_info("%s(): Button-1 release event\n", __func__);
			break;

		case SND_JACK_BTN_2:
			/* Button release event must be sent */
			snd_soc_jack_report(escore->jack, 0, SND_JACK_BTN_2);
			pr_info("%s(): Button-2 release event\n", __func__);
			break;

		case SND_JACK_BTN_3:
			/* Button release event must be sent */
			snd_soc_jack_report(escore->jack, 0, SND_JACK_BTN_3);
			pr_info("%s(): Button-3 release event\n", __func__);
			break;

		case ES_ACC_BTN_INVALID:
		default:
			pr_err("Invalid button type : %d\n",
			       escore->acc_btn_pressed);
			break;
		}

		escore->acc_det_status = ES_ACC_BUTTON_RELEASED;

	}

	return rc;
};

static int es755_codec_intr(struct notifier_block *self, unsigned long action,
		void *dev)
{
	struct escore_priv *escore = (struct escore_priv *)dev;
	union es755_accdet_status_reg accdet_status_reg;
	struct esxxx_accdet_config accdet_cfg = escore->pdata->accdet_cfg;
	int sys_intr_resp, value;
	int rc = 0;
	static int redetect_hs = 1;
	u8 impd_level;
	u8 mg_sel_force;
	u8 mg_select;
	u8 is_invalid_type;

	pr_debug("%s(): Event: 0x%04x\n", __func__, (u32)action);

	if (escore->dhwpt_mode) {
		/* Cancel delayed work of MP Sleep and bring chip to Normal
		   mode if it is in MP Sleep mode */
		pr_debug("%s(): Bring chip from MP Sleep to Normal mode\n",
				__func__);
		rc = es755_exit_mp_sleep();
		if (rc) {
			dev_err(escore_priv.dev,
					"%s(): es755_exit_mp_sleep() fail %d\n",
					__func__, rc);
			goto intr_exit;
		}
	}

	if (action & ES_CODEC_INTR_EVENT) {
		sys_intr_resp = escore_read_locked(NULL, ES_GET_SYS_INTERRUPT);
		if (sys_intr_resp < 0) {
			pr_err("%s(): Get System Event failed %d\n",
			       __func__, sys_intr_resp);
			goto intr_exit;
		}

		if (ES_FACTORY_INTR_EVENT(sys_intr_resp)) {
			pr_err("%s(): Factory mode interrupt reported !!\n",
						__func__);
			escore_pm_put_autosuspend();
			goto intr_exit;
		}

		if (ES_PLUG_EVENT(sys_intr_resp)) {

			/*
			 * Due to Quick insertion, FW might give more
			 * than 1 interrupt for single Plug event. if
			 * MIC Config is already sent, ignore
			 * subsequent Plug events.
			 */
			/*
			 * However, in case of Slow insertions, FW
			 * will send 2 Plug-in events which is valid and
			 * should not be ignored.
			 *
			 * Since the events are actually coming from the
			 * FW, HSW should display it as is. So, accept
			 * plug-in events even if it is a Duplicate.
			*/
			pr_info("%s(): Plug event\n", __func__);
			/* Enable MIC Detection */
			rc = es755_mic_config(escore);
			if (rc < 0) {
				pr_err("%s(): MIC config fail %d\n",
				       __func__, rc);
				goto intr_exit;
			}

			escore->acc_det_status = ES_ACC_PLUGGED;
		}

		if (ES_MICDET_EVENT(sys_intr_resp)) {

			value = escore_read_locked(NULL, ES_GET_ACCDET_STATUS);
			if (value < 0) {
				pr_err("%s(): Accessory detect status fail %d\n",
				       __func__, value);
				goto intr_exit;
			}

			accdet_status_reg.value = value;

			/* Ignore interrupt if
			 * plugdet_fsm2 = 1 and plugdet_fsm1 = 0
			 */
			if (accdet_status_reg.fields.plug_det_fsm2 &&
				!accdet_status_reg.fields.plug_det_fsm1) {
				pr_debug("%s(): Found PLUG_LOCK, ignore it\n",
				       __func__);
				goto intr_exit;
			}

			impd_level = accdet_status_reg.fields.impd_level;

			mg_select = accdet_status_reg.fields.mg_select;
			mg_sel_force = accdet_cfg.mic_det_enabled & (0x1);

			/* Updated Headphone Type Decode Table as per BAS_3584
			 *  ------------------------------
			 * | MG_SEL | IMP_LEVEL | HS_TYPE |
			 * |   X    |    0      |  LRG HP |
			 * |   0    |   1-5     |  Inval  |
			 * |   0    |    6      |  LRGM   |
			 * |   1    |   1-5     |  Inval  |
			 * |   1    |    6      |  LRMG   |
			 *  ------------------------------
			 */

			/* Open mic detected then according to es854 API guide
			 * HS_TYPE is assume to LRGG */
			if (ES_OPEN_MICDET_EVENT(sys_intr_resp)) {
				pr_info("%s(): LRGG Headset detected, Reporting as Headphone\n",
						__func__);
				snd_soc_jack_report(escore->jack,
					SND_JACK_HEADPHONE,
					SND_JACK_HEADPHONE);
				escore->acc_det_status = ES_ACC_DETECTED;

			} else if (impd_level) {
				pr_info("%s(): Headset detected\n", __func__);

				if (redetect_hs) {
					redetect_hs = 0;
					msleep(escore_priv.hs_delay);
					pr_debug("%s(): ***Redetect HS MIC with Delay %d\n",
						__func__, escore_priv.hs_delay);
					/* redetect_HS   */
					rc = es755_mic_config(escore);
					if (rc < 0) {
						pr_err("%s(): MIC config fail %d\n",
							   __func__, rc);
					}
					goto intr_exit;
				} else
					redetect_hs = 1;

				/* Mic Impedence - 6 */
				if (impd_level == MIC_IMPEDANCE_LEVEL) {
					if (mg_select)
						pr_info("LRMG Headset\n");
					else
						pr_info("LRGM Headset\n");
					is_invalid_type = false;
				} else {
					/* MIC Impedence - 1 to 5 or > 6 */
					is_invalid_type = true;
				}

				if (is_invalid_type) {
					pr_err("Invalid type:%d\n", impd_level);
					rc = -EINVAL;
					goto intr_exit;
				}
				escore->es_vs_route_preset =
						es755_vs_presets[ES755_MICHS];
				escore->es_cvs_preset =
						es755_cvs_presets[ES755_MICHS];
				escore->mic_source_change = 1;

				escore->button_config_required = 1;
				snd_soc_jack_report(escore->jack,
					SND_JACK_HEADSET, SND_JACK_HEADSET);

				escore->acc_det_status = ES_ACC_DETECTED;

				rc = es755_button_config(escore);
				if (rc < 0)
					goto intr_exit;

			} else {
				pr_info("%s(): Headphone detected\n",
						__func__);
				redetect_hs = 1;

				snd_soc_jack_report(escore->jack,
					SND_JACK_HEADPHONE,
					SND_JACK_HEADPHONE);

				escore->acc_det_status = ES_ACC_DETECTED;
			}

			pr_debug("%s(): AccDet status 0x%04x\n", __func__,
					accdet_status_reg.value);
		}

		/*
		 * Note: Un-Plug event should have higher priority compared to
		 * button press & release Event Checks.
		 */
		if (ES_UNPLUG_EVENT(sys_intr_resp)) {

			/*
			 * Due to Quick removal, FW might give more
			 * than 1 interrupt for single Unplug event.
			 * If Unplug Event is already detected, ignore
			 * subsequent Unplug events.
			 */
			if (escore->acc_det_status == ES_ACC_UNPLUGGED) {
				pr_debug("%s(): Ignoring Duplicate Unplug Event\n",
					 __func__);
				goto intr_exit;
			}

			value = escore_read_locked(NULL, ES_GET_ACCDET_STATUS);
			if (value < 0) {
				pr_err("%s(): Accessory detect status fail %d",
				       __func__, value);
				goto intr_exit;
			}
			accdet_status_reg.value = value;
			redetect_hs = 1; /* initialize the counter */

			/* Ignore interrupt if
			 * plugdet_fsm2 = 1 and plugdet_fsm1 = 1
			 */
			if (accdet_status_reg.fields.plug_det_fsm2 &&
			    accdet_status_reg.fields.plug_det_fsm1) {
				pr_info("%s(): Found UNPLUG_LOCK, ignore it\n",
					__func__);
				goto intr_exit;
			}

			pr_info("%s(): Unplug detected\n", __func__);
			escore->es_vs_route_preset =
				es755_vs_presets[escore->default_cvq_mic];
			escore->es_cvs_preset =
				es755_cvs_presets[escore->default_cvq_mic];
			/* Headset Unplug detected because
			 * button_config_required is enabled only when
			 * Headset is detected */
			if (escore->button_config_required) {
				escore->mic_source_change = 1;
				escore->button_config_required = 0;
			}
			snd_soc_jack_report(escore->jack, 0, JACK_DET_MASK);

			/*
			 * If user presses button for more than autosuspend
			 * time, firmware should not be put into suspend mode
			 * until button is released OR it is unplugged.
			 * Put the chip back to autosuspend when user unplugs.
			 */
			if (escore->acc_det_status == ES_ACC_BUTTON_PRESSED) {
				escore_pm_put_autosuspend();
				if (escore->mp_sleep_requested
						&& escore->dhwpt_mode
						&& !escore->dhwpt_capture_on) {
					pr_debug("%s(): %s\n",
						 "Put chip back into mp_sleep",
						__func__);
					es755_enter_mp_sleep();
				}
			}

			escore->acc_det_status = ES_ACC_UNPLUGGED;

			/*
			 * Since Headphone/set is removed, Mask the Button
			 * Press/Release event as well. This way, duplicate or
			 * invalid Button event is avoided.
			 */
			sys_intr_resp &= ~(ES_BUTTON_RELEASE_EVENT_MASK |
						ES_BUTTON_PRESS_EVENT_MASK);

		}

		if (ES_THERMAL_SHUTDOWN_EVENT(sys_intr_resp))
			pr_err("%s(): Thermal shut down detected\n",
				__func__);

		if (ES_LINEOUT_SHORT_CIRCUIT_EVENT(sys_intr_resp))
			pr_err("%s(): Lineout short circuit detected\n",
				__func__);

		if (ES_SPKR_LEFT_SHORT_CIRCUIT_EVENT(sys_intr_resp))
			pr_err("%s(): Speaker left short circuit detected\n",
				__func__);

		if (ES_SPKR_RIGHT_SHORT_CIRCUIT_EVENT(sys_intr_resp))
			pr_err("%s(): Speaker right short circuit detected\n",
				__func__);

		if (ES_HP_SHORT_CIRCUIT_EVENT(sys_intr_resp))
			pr_err("%s(): Headphone short circuit detected\n",
				__func__);

		if (ES_EP_SHORT_CIRCUIT_EVENT(sys_intr_resp))
			pr_err("%s(): Earphone short circuit detected\n",
				__func__);


		/*
		 * If previous Event is not Button Press, Test Button Press 1st
		 * & then Button Release.
		 */
		if (escore->acc_det_status != ES_ACC_BUTTON_PRESSED) {
			rc = chk_btn_press_event(escore, sys_intr_resp);
			if (rc < 0)
				goto intr_exit;

			chk_btn_release_event(escore, sys_intr_resp);

		}  else {

			/* Test Button Release 1st  & then Button Press */
			rc = chk_btn_release_event(escore, sys_intr_resp);
			if (rc < 0)
				goto intr_exit;

			chk_btn_press_event(escore, sys_intr_resp);
		}
	}

intr_exit:
	return NOTIFY_OK;
}

static irqreturn_t es755_irq_work(int irq, void *data)
{
	struct escore_priv *escore = (struct escore_priv *)data;
	int retry = ES_EVENT_STATUS_RETRY_COUNT;
	int rc, ret;
	u32 cmd = 0;
	u32 event_type = 0;

	pr_debug("%s() Interrupt received\n", __func__);
	if (!escore) {
		pr_err("%s(): Invalid IRQ data\n", __func__);
		return IRQ_HANDLED;
	}

	escore->intr_recvd = 1;

	/* Take wake lock to avoid system wide suspend */
repeat:
	rc = wait_event_interruptible(escore->irq_waitq,
			(escore->dev->power.is_suspended != true));
	if (unlikely(rc == -ERESTARTSYS)) {
		pr_err("%s(): Signal received, try again\n", __func__);
		goto repeat;
	}

	mutex_lock(&escore->access_lock);
	rc = escore_pm_get_sync();
	if (rc < 0) {
		dev_err(escore_priv.dev, "%s(): pm_get_sync() failed :%d\n",
								__func__, rc);
		mutex_unlock(&escore->access_lock);
		goto err_pm_get_sync;
	}

	/* Enable the clocks. This is required for codec interrupts. If chip
	 * is in suspend mode, runtime PM is disabled. This ISR is executed
	 * before system resume and hence chip needs to be provided clock
	 * before doing any command transfer.
	 */
	if (escore_priv.pdata->esxxx_clk_cb) {
		escore_priv.pdata->esxxx_clk_cb(1);
		/* Setup for clock stablization delay */
		msleep(ES_PM_CLOCK_STABILIZATION);
	}

	if (escore->dhwpt_mode) {
		/* Cancel delayed work of MP Sleep, and bring chip to
		   Normal mode if it is in MP Sleep mode */
		pr_debug("%s(): Bring chip from MP Sleep to Normal mode\n",
				__func__);
		rc = es755_exit_mp_sleep();
		if (rc) {
			dev_err(escore_priv.dev,
			"%s(): es755_exit_mp_sleep() fail %d\n",
			__func__, rc);
			goto irq_exit;
		}
	}

	/* BAS-2402: When pm usage count is non-zero and device is in
	 * VS Low Power mode, following delay is required for firmware
	 * to be ready after keyword detection interrupt is received.
	 * Runtime PM status value is set to ES_PM_OFF only by CVQ
	 * from application and increments pm usage count, otherwise
	 * setting Runtime PM status value to ES_PM_OFF is not advisable
	 * for actual usecases.
	 */
	if (escore->escore_power_state == ES_SET_POWER_STATE_VS_LOWPWR)
		msleep(escore_priv.delay.vs_to_normal);
	else if (escore->escore_power_state == ES_SET_POWER_STATE_MP_SLEEP)
		msleep(escore_priv.delay.mpsleep_to_normal);

	/* Power state change:
	 * Interrupt will put the chip into Normal State
	 */

	cmd = ES_GET_EVENT << 16;

	while (retry) {
		rc = escore_cmd_nopm(escore, cmd, &event_type);
		if (!rc || !--retry)
			break;
		pr_info("%s(): wakeup and retry\n", __func__);
		ret = escore_wakeup(escore);
		if (ret) {
			dev_err(escore->dev, "%s() wakeup failed ret = %d\n",
								__func__, ret);
			break;
		}
	}
	if (rc < 0) {
		pr_err("%s(): Error reading IRQ event: %d\n", __func__, rc);
		mutex_unlock(&escore->access_lock);
		goto irq_exit;
	}

	mutex_unlock(&escore->access_lock);

	if (escore->escore_power_state == escore->non_vs_sleep_state ||
		escore->escore_power_state == ES_SET_POWER_STATE_MP_SLEEP) {
		escore->escore_power_state = ES_SET_POWER_STATE_NORMAL;
	} else if (escore->escore_power_state == ES_SET_POWER_STATE_VS_LOWPWR) {
		escore->escore_power_state = ES_SET_POWER_STATE_VS_OVERLAY;

		/* If the chip is not awake, the reading of event will wakeup
		 * the chip which will result in interrupt reconfiguration
		 * from device resume. Reconfiguration is deferred till the
		 * time interrupt is processed from the notifier callback.
		 *
		 * The interrupt reconfiguration in this case will be taken
		 * care of by a low priority notifier callback
		 */
		escore->defer_intr_config = 1;
	}

	event_type &= ES_MASK_INTR_EVENT;
	mutex_lock(&escore->escore_event_type_mutex);
	escore->escore_event_type = event_type;
	mutex_unlock(&escore->escore_event_type_mutex);

	if (event_type != ES_NO_EVENT) {
		pr_debug("%s(): Notify subscribers about 0x%04x event",
				__func__, event_type);
		blocking_notifier_call_chain(escore->irq_notifier_list,
						event_type, escore);
	}

irq_exit:
	if (escore->mic_source_change && escore->voice_recognition)
		escore_pm_put_sync_suspend();
	else
		escore_pm_put_autosuspend();
err_pm_get_sync:
	escore->intr_recvd = 0;
	escore->mic_source_change = 0;
	return IRQ_HANDLED;
}

static BLOCKING_NOTIFIER_HEAD(es755_irq_notifier_list);

static struct notifier_block es755_codec_intr_cb = {
	.notifier_call = es755_codec_intr,
	.priority = ES_NORMAL,
};

int es755_core_probe(struct device *dev)
{
	struct esxxx_platform_data *pdata;
	int rc = 0;
	const char *fw_filename = "audience/es755-fw.bin";
	const char *fw_af_filename = "audience/es755-af.bin";

	/**/
	if (dev->of_node) {
		dev_info(dev, "Platform data from device tree\n");
		pdata = es755_populate_dt_pdata(dev);
		dev->platform_data = pdata;

		/* Retrieve FW names from DTS if provided */
		if (of_property_read_string(dev->of_node, "adnc,fw_filename",
					(const char **)&fw_filename)) {
			dev_err(escore_priv.dev, "%s, %s\n",
				"Property adnc,fw_filename missing or invalid",
				"Using default firmware name");
		}

	} else {
		dev_info(dev, "Platform data from board file\n");
		pdata = dev->platform_data;
	}

	escore_priv.pdata = pdata;

	mutex_init(&escore_priv.datablock_dev.datablock_read_mutex);
	mutex_init(&escore_priv.codec_mutex);
	mutex_init(&escore_priv.streaming_mutex);
	mutex_init(&escore_priv.msg_list_mutex);
	mutex_init(&escore_priv.datablock_dev.datablock_mutex);
	mutex_init(&escore_priv.escore_event_type_mutex);

	init_completion(&escore_priv.cmd_compl);
	init_waitqueue_head(&escore_priv.stream_in_q);
	init_waitqueue_head(&escore_priv.irq_waitq);
	INIT_LIST_HEAD(&escore_priv.msg_list);
	escore_priv.irq_notifier_list = &es755_irq_notifier_list;
	escore_priv.cmd_compl_mode = ES_CMD_COMP_POLL;
	escore_priv.wait_api_intr = 0;
	escore_priv.fw_af_filename = fw_af_filename;
	escore_priv.mic_source_change = 0;

	escore_priv.selected_endpoint = -1;
	memset(escore_priv.digital_gain, 0, sizeof(escore_priv.digital_gain));

	escore_priv.power_transition_wq =
		create_singlethread_workqueue("escore_power_transition_wq");
	INIT_DELAYED_WORK(&escore_priv.mp_sleep_work, es755_mp_sleep_work);

	rc = sysfs_create_group(&escore_priv.dev->kobj, &core_sysfs);
	if (rc) {
		dev_err(escore_priv.dev,
			"failed to create core sysfs entries: %d\n", rc);
	}

        if (!IS_ERR(pdata->reset_gpio) && !escore_priv.flag.reset_done)
		escore_gpio_reset(&escore_priv);

	rc = request_firmware((const struct firmware **)&escore_priv.standard,
			      fw_filename, escore_priv.dev);
	if (rc) {
		dev_err(escore_priv.dev,
			"%s(): request_firmware(%s) failed %d\n",
			__func__, fw_filename, rc);
		goto request_firmware_error;
	}

	escore_priv.boot_ops.bootup = es755_bootup;
	escore_priv.soc_component_dev_escore = &soc_component_dev_es755;
	escore_priv.dai = es755_dai;
	escore_priv.dai_nr = ES_NUM_CODEC_DAIS;
	escore_priv.api_addr_max = ES_API_ADDR_MAX;
	escore_priv.api_access = es755_api_access;
	escore_priv.reg_cache = a300_reg_cache;
	escore_priv.flag.is_codec = 1;
	escore_priv.escore_power_state = ES_SET_POWER_STATE_NORMAL;
	escore_priv.escore_event_type = ES_NO_EVENT;
	escore_priv.i2s_dai_data = i2s_dai_data;
	escore_priv.config_jack = es755_config_jack;
	escore_priv.non_vs_sleep_state = ES_SET_POWER_STATE_DEEP_SLEEP;
	if (escore_priv.pri_intf == ES_SLIM_INTF) {

		escore_priv.slim_rx = slim_rx;
		escore_priv.slim_tx = slim_tx;
		escore_priv.slim_dai_data = slim_dai_data;
		escore_priv.slim_setup = es755_slim_setup;

		escore_priv.slim_rx_ports = ES_SLIM_RX_PORTS;
		escore_priv.slim_tx_ports = ES_SLIM_TX_PORTS;
		escore_priv.codec_slim_dais = ES_NUM_CODEC_SLIM_DAIS;

		escore_priv.slim_tx_port_to_ch_map = es755_slim_tx_port_to_ch;
		escore_priv.slim_rx_port_to_ch_map = es755_slim_rx_port_to_ch;

		escore_priv.slim_dai_ops.set_channel_map =
				es755_slim_set_channel_map;

		/* Initialization of be_id goes here if required */
		escore_priv.slim_be_id = NULL;

		/* Initialization of _remote_ routines goes here if required */
		escore_priv.remote_cfg_slim_rx = NULL;
		escore_priv.remote_cfg_slim_tx = NULL;
		escore_priv.remote_close_slim_rx = NULL;
		escore_priv.remote_close_slim_tx = NULL;

		escore_priv.flag.local_slim_ch_cfg = 1;
		escore_priv.channel_dir = es755_channel_dir;
		escore_priv.slim_setup(&escore_priv);
	}
	escore_priv.i2s_dai_ops.hw_params = es755_hw_params;
	escore_priv.i2s_dai_ops.set_fmt = es755_set_fmt;
	escore_priv.i2s_dai_ops.shutdown = es755_shutdown;

	/* Setup Default Accessory Detect Parameters */
	escore_priv.acc_det_status = ES_ACC_INVALID;
	escore_priv.acc_btn_pressed = ES_ACC_BTN_INVALID;
	escore_priv.hs_delay = DEFAULT_HS_REDETECTION_DELAY;

	/* Event Interrupt registration */
	if (escore_priv.irq >= 0) {
		rc = request_threaded_irq(escore_priv.irq, NULL,
				es755_irq_work, IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				"escore-irq-work", &escore_priv);
		if (rc < 0) {
			pr_err("Error in registering interrupt :%d", rc);
			goto event_intr_error;
		} else {
			irq_set_irq_wake(escore_priv.irq, 1);
		}

		/* Disable the interrupt till needed */
		disable_irq(escore_priv.irq);

		escore_register_notify(escore_priv.irq_notifier_list,
				&es755_codec_intr_cb);
	}

	return rc;

event_intr_error:
	release_firmware(escore_priv.standard);
request_firmware_error:
	return rc;
}
EXPORT_SYMBOL_GPL(es755_core_probe);

static __init int es755_init(void)
{
	int rc = 0;

	pr_debug("%s()", __func__);

	escore_priv.codec_clk_en = false;
	/* Initially power level support is disabled */
	escore_priv.power_saving_mode = ES_POWER_LEVEL_DISABLE;
	escore_priv.device_name  = "elemental-addr";
	escore_priv.interface_device_name  = "slim-ifd";
	escore_priv.interface_device_elem_addr_name  =
					"slim-ifd-elemental-addr";
	mutex_init(&escore_priv.access_lock);
	mutex_init(&escore_priv.intf_probed_mutex);
	init_completion(&escore_priv.fw_download);
	escore_platform_init();
	escore_priv.pri_intf = ES_SPI_INTF;
	escore_priv.high_bw_intf = ES_SPI_INTF;
	rc = escore_spi_init();

	if (rc) {
		pr_debug("Error registering Audience eS755 driver: %d\n", rc);
		goto INIT_ERR;
	}

INIT_ERR:
	return rc;
}
module_init(es755_init);

static __exit void es755_exit(void)
{
	pr_debug("%s()\n", __func__);
	if (escore_priv.flag.af)
		release_firmware(escore_priv.af);

	if (escore_priv.power_transition_wq)
		destroy_workqueue(escore_priv.power_transition_wq);

	escore_spi_exit();
}
module_exit(es755_exit);


MODULE_DESCRIPTION("ASoC ES755 driver");
MODULE_AUTHOR("Greg Clemson <gclemson@audience.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:es755-codec");
MODULE_FIRMWARE("audience/es755-fw.bin");
