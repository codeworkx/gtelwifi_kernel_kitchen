/*
 *  Copyright (C) 2010,Imagis Technology Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/stat.h>
#include <asm/unaligned.h>
#include <linux/uaccess.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/err.h>

#include "ist30xx.h"
#include "ist30xx_update.h"
#include "ist30xx_tracking.h"

#if IST30XXC_DEBUG
#include "ist30xx_misc.h"
#endif

#if IST30XXC_INTERNAL_BIN
#include "./firmware/VIVALTO_VE_3M.h"
#endif  // IST30XXC_INTERNAL_BIN

#define TAGS_PARSE_OK           	(0)

int ist30xxc_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int msg_num, u8 *cmd_buf)
{
	int ret = 0;
	int idx = msg_num - 1;
	int size = msgs[idx].len;
	u8 *msg_buf = NULL;
	u8 *pbuf = NULL;
	int trans_size, max_size = 0;

	if (msg_num == WRITE_CMD_MSG_LEN)
		max_size = I2C_MAX_WRITE_SIZE;
	else if (msg_num == READ_CMD_MSG_LEN)
		max_size = I2C_MAX_READ_SIZE;

	if (unlikely(max_size == 0)) {
		ts_err("%s() : transaction size(%d)\n", __func__, max_size);
		return -EINVAL;
	}

	if (msg_num == WRITE_CMD_MSG_LEN) {
		msg_buf = kmalloc(max_size + IST30XXC_ADDR_LEN, GFP_KERNEL);
		if (!msg_buf)
			return -ENOMEM;
		memcpy(msg_buf, cmd_buf, IST30XXC_ADDR_LEN);
		pbuf = msgs[idx].buf;
	}

	while (size > 0) {
		trans_size = (size >= max_size ? max_size : size);

		msgs[idx].len = trans_size;
		if (msg_num == WRITE_CMD_MSG_LEN) {
			memcpy(&msg_buf[IST30XXC_ADDR_LEN], pbuf, trans_size);
			msgs[idx].buf = msg_buf;
			msgs[idx].len += IST30XXC_ADDR_LEN;
		}
		ret = i2c_transfer(adap, msgs, msg_num);
		if (unlikely(ret != msg_num)) {
			ts_err("%s() : i2c_transfer failed(%d), num=%d\n",
				__func__, ret, msg_num);
			break;
		}

		if (msg_num == WRITE_CMD_MSG_LEN)
			pbuf += trans_size;
		else
			msgs[idx].buf += trans_size;

		size -= trans_size;
	}

	if (msg_num == WRITE_CMD_MSG_LEN)
		kfree(msg_buf);

	return ret;
}

int ist30xxc_read_buf(struct i2c_client *client, u32 cmd, u32 *buf, u16 len)
{
	int ret, i;
	u32 le_reg = cpu_to_be32(cmd);

	struct i2c_msg msg[READ_CMD_MSG_LEN] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = IST30XXC_ADDR_LEN,
			.buf = (u8 *)&le_reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len * IST30XXC_DATA_LEN,
			.buf = (u8 *)buf,
		},
	};

	ret = ist30xxc_i2c_transfer(client->adapter, msg, READ_CMD_MSG_LEN, NULL);
	if (unlikely(ret != READ_CMD_MSG_LEN))
		return -EIO;

	for (i = 0; i < len; i++)
		buf[i] = cpu_to_be32(buf[i]);

	return 0;
}

int ist30xxc_write_buf(struct i2c_client *client, u32 cmd, u32 *buf, u16 len)
{
	int i;
	int ret;
	struct i2c_msg msg;
	u8 cmd_buf[IST30XXC_ADDR_LEN];
	u8 msg_buf[IST30XXC_DATA_LEN * (len + 1)];

	put_unaligned_be32(cmd, cmd_buf);

	if (likely(len > 0)) {
		for (i = 0; i < len; i++)
			put_unaligned_be32(buf[i], msg_buf + (i * IST30XXC_DATA_LEN));
	} else {
		/* then add dummy data(4byte) */
		put_unaligned_be32(0, msg_buf);
		len = 1;
	}

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len * IST30XXC_DATA_LEN;
	msg.buf = msg_buf;

	ret = ist30xxc_i2c_transfer(client->adapter, &msg, WRITE_CMD_MSG_LEN,
				   cmd_buf);
	if (unlikely(ret != WRITE_CMD_MSG_LEN))
		return -EIO;

	return 0;
}

// ISP burst r/w
int ist30xxc_isp_read_burst(struct i2c_client *client, u32 addr,
			u32 *buf32, u16 len)
{
	int ret = 0;
	int i;
	u16 max_len = I2C_MAX_READ_SIZE / IST30XXC_DATA_LEN;
    u16 remain_len = len;

	for (i = 0; i < len; i += max_len) {
		if (remain_len < max_len) max_len = remain_len;

		ret = ist30xxc_read_buf(client, addr, buf32, max_len);
		if (unlikely(ret)) {
			ts_err("Burst fail, addr: %x\n", __func__, addr);
			return ret;
		}

		buf32 += max_len;
        remain_len -= max_len;
	}

	return 0;
}

int ist30xxc_isp_write_burst(struct i2c_client *client, u32 addr,
			u32 *buf32, u16 len)
{
	int ret = 0;
	int i;
	u16 max_len = I2C_MAX_WRITE_SIZE / IST30XXC_DATA_LEN;
    u16 remain_len = len;

	for (i = 0; i < len; i += max_len) {
		if (remain_len < max_len) max_len = remain_len;
		ret = ist30xxc_write_buf(client, addr, buf32, max_len);
		if (unlikely(ret)) {
			ts_err("Burst fail, addr: %x\n", addr);
			return ret;
		}

		buf32 += max_len;
        remain_len -= max_len;
	}

	return 0;
}

#define IST30XXC_ISP_READ_TOTAL_S    (0x01)
#define IST30XXC_ISP_READ_TOTAL_B    (0x11)
#define IST30XXC_ISP_READ_MAIN_S     (0x02)
#define IST30XXC_ISP_READ_MAIN_B     (0x12)
#define IST30XXC_ISP_READ_INFO_S     (0x03)
#define IST30XXC_ISP_READ_INFO_B     (0x13)
#define IST30XXC_ISP_PROG_TOTAL_S    (0x04)
#define IST30XXC_ISP_PROG_TOTAL_B    (0x14)
#define IST30XXC_ISP_PROG_MAIN_S     (0x05)
#define IST30XXC_ISP_PROG_MAIN_B     (0x15)
#define IST30XXC_ISP_PROG_INFO_S     (0x06)
#define IST30XXC_ISP_PROG_INFO_B     (0x16)
#define IST30XXC_ISP_ERASE_BLOCK     (0x07)
#define IST30XXC_ISP_ERASE_SECTOR    (0x08)
#define IST30XXC_ISP_ERASE_PAGE      (0x09)
#define IST30XXC_ISP_ERASE_INFO      (0x0A)
#define IST30XXC_ISP_READ_TOTAL_CRC  (0x1B)
#define IST30XXC_ISP_READ_MAIN_CRC   (0x1C)
#define IST30XXC_ISP_READ_INFO_CRC   (0x1D)
int ist30xxc_isp_enable(struct i2c_client *client, bool enable)
{
	int ret = 0;

    if (enable) {
        ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_ISPEN, 0xDE01);
        if (unlikely(ret))
			return ret;

#if (IMAGIS_TSP_IC < IMAGIS_IST3038C)
        ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_MODE, 0x4100);
        if (unlikely(ret))
			return ret;

        ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_TEST_MODE1, 0x38);
        if (unlikely(ret))
			return ret;
#endif
	} else {
		ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_ISPEN, 0x2);		
        if (unlikely(ret))
			return ret;
	}

    msleep(1);

	return ret;
}

int ist30xxc_isp_mode(struct i2c_client *client, int mode)
{
	int ret = 0;
	u32 val = 0;

	switch (mode) {
	case IST30XXC_ISP_READ_TOTAL_S:
		val = 0x8090;
		break;
    case IST30XXC_ISP_READ_TOTAL_B:
    case IST30XXC_ISP_READ_TOTAL_CRC:
		val = 0x8190;
		break;
	case IST30XXC_ISP_READ_MAIN_S:
		val = 0x0090;
		break;
    case IST30XXC_ISP_READ_MAIN_B:
    case IST30XXC_ISP_READ_MAIN_CRC:
		val = 0x0190;
		break;
	case IST30XXC_ISP_READ_INFO_S:
		val = 0x0098;
		break;
	case IST30XXC_ISP_READ_INFO_B:
    case IST30XXC_ISP_READ_INFO_CRC:
		val = 0x0198;
		break;
	case IST30XXC_ISP_PROG_TOTAL_S:
		val = 0x8050;
		break;
	case IST30XXC_ISP_PROG_TOTAL_B:
		val = 0x8150;
		break;
	case IST30XXC_ISP_PROG_MAIN_S:
		val = 0x0050;
		break;
	case IST30XXC_ISP_PROG_MAIN_B:
		val = 0x0150;
		break;
	case IST30XXC_ISP_PROG_INFO_S:
		val = 0x0058;
		break;
	case IST30XXC_ISP_PROG_INFO_B:
		val = 0x0158;
		break;
	case IST30XXC_ISP_ERASE_BLOCK:
		val = 0x0031;
		break;
	case IST30XXC_ISP_ERASE_SECTOR:
		val = 0x0032;
		break;
	case IST30XXC_ISP_ERASE_PAGE:
		val = 0x0030;
		break;
    case IST30XXC_ISP_ERASE_INFO:
		val = 0x0038;
		break;
	default:
		ts_err("ISP fail, unknown mode\n");
		return -EINVAL;
	}

#if (IMAGIS_TSP_IC > IMAGIS_IST3032C)
    val &= ~(0x8000);
#endif    

	ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_MODE, val);
	if (unlikely(ret)) {
		ts_err("ISP fail, IST30XXC_FLASH_MODE\n");
		return ret;
	}

	return 0;
}

int ist30xxc_isp_erase(struct i2c_client *client, int mode, u32 index)
{
	int ret = 0;

	ret = ist30xxc_isp_mode(client, mode);
	if (unlikely(ret))
		return ret;

   	ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_DIN, index);
    if (unlikely(ret)) {
	    ts_err("ISP fail, IST30XXC_FLASH_DIN\n");
	    return ret;
    }

	msleep(50); // Flash erase time : Mininum 40msec

	return ret;
}


int ist30xxc_isp_program(struct i2c_client *client, u32 addr, int mode, 
		       const u32 *buf32, int len)
{
	int ret = 0;

    ret = ist30xxc_isp_mode(client, mode);
	if (unlikely(ret))
		return ret;
    
	ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_ADDR, addr);
	if (unlikely(ret)) {
		ts_err("ISP fail, IST30XXC_FLASH_ADDR\n");
		return ret;
	}

    if (mode & 0x10)
        ret = ist30xxc_isp_write_burst(client, 
                IST30XXC_FLASH_DIN, (u32 *)buf32, len);
    else
        ret = ist30xxc_write_buf(client, IST30XXC_FLASH_DIN, (u32 *)buf32, len);

    if (unlikely(ret)) {
        ts_err("ISP fail, IST30XXC_FLASH_DIN\n");
        return ret;
   	}

	return ret;
}

int ist30xxc_isp_read(struct i2c_client *client, u32 addr, int mode,
		      u32 *buf32, int len)
{
	int ret = 0;
	
	/* IST30xxB ISP read mode */
	ret = ist30xxc_isp_mode(client, mode);
	if (unlikely(ret))
		return ret;

	ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_ADDR, addr);
	if (unlikely(ret)) {
		ts_err("ISP fail, IST30XXC_FLASH_ADDR\n");
		return ret;
	}

    if (mode & 0x10)
        ret = ist30xxc_isp_read_burst(client, IST30XXC_FLASH_DOUT, buf32, len);
    else
        ret = ist30xxc_read_buf(client, IST30XXC_FLASH_DOUT, buf32, len);

  	if (unlikely(ret)) {
	    ts_err("ISP fail, IST30XXC_FLASH_DOUT\n");
   		return ret;
    }

	return 0;
}

int ist30xxc_cmd_read_chksum(struct i2c_client *client, int mode, 
			     u32 start_addr, u32 end_addr, u32 *chksum)
{
	int ret = 0;
	u32 val = (1 << 28) | (1 << 25) | (1 << 24) | (1 << 20) | (1 << 16); 

	val |= (end_addr / IST30XXC_ADDR_LEN) - 1;

    ret = ist30xxc_isp_mode(client, mode);
	if (unlikely(ret))
		return ret;

	ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_ADDR, start_addr);
	if (unlikely(ret)) {
		ts_err("ISP fail, IST30XXC_FLASH_ADDR (%x)\n", val);
		return ret;
	}

    ret = ist30xxc_write_cmd(client, IST30XXC_FLASH_AUTO_READ, val);
	if (unlikely(ret)) {
		ts_err("ISP fail, IST30XXC_FLASH_AUTO_READ (%x)\n", val);
		return ret;
	}

	msleep(100);

	ret = ist30xxc_read_reg(client, IST30XXC_FLASH_CRC, chksum);
	if (unlikely(ret)) {
		ts_err("ISP fail, IST30XXC_FLASH_CRC (%x)\n", chksum);
		return ret;
	}

	return 0;
}

int ist30xxc_read_chksum(struct ist30xxc_data *data, u32 *chksum)
{
	int ret = 0;
	u32 start_addr, end_addr;

	start_addr = data->tags.fw_addr;
	end_addr = data->tags.sensor_addr + data->tags.sensor_size;
	ret = ist30xxc_cmd_read_chksum(data->client, 
            IST30XXC_ISP_READ_TOTAL_CRC, start_addr, end_addr, chksum);
	if (unlikely(ret))
		return ret;

	ts_info("chksum: %x(%x~%x)\n", *chksum, start_addr, end_addr);

	return 0;
}

int ist30xxc_read_chksum_all(struct i2c_client *client, u32 *chksum)
{
	int ret = 0;
	u32 start_addr, end_addr;

	start_addr = IST30XXC_FLASH_BASE_ADDR;
	end_addr = IST30XXC_FLASH_BASE_ADDR + IST30XXC_FLASH_TOTAL_SIZE;
	ret = ist30xxc_cmd_read_chksum(client, 
            IST30XXC_ISP_READ_TOTAL_CRC, start_addr, end_addr, chksum);
	if (unlikely(ret))
		return ret;

	ts_info("chksum: %x(%x~%x)\n", *chksum, start_addr, end_addr);

	return 0;
}

int ist30xxc_isp_fw_read(struct ist30xxc_data *data, u32 *buf32)
{
	int ret = 0;
	int i;
	int len;
	u32 addr = IST30XXC_FLASH_BASE_ADDR;

	ist30xxc_reset(data, true);

	/* IST30xxB ISP enable */
	ret = ist30xxc_isp_enable(data->client, true);
	if (unlikely(ret))
		return ret;

#if I2C_BURST_MODE
    for (i = 0; i < IST30XXC_FLASH_TOTAL_SIZE; i += I2C_MAX_READ_SIZE) {
		len = I2C_MAX_READ_SIZE / IST30XXC_DATA_LEN;
		if ((IST30XXC_FLASH_TOTAL_SIZE - i) < I2C_MAX_READ_SIZE)
            len = (IST30XXC_FLASH_TOTAL_SIZE - i) / IST30XXC_DATA_LEN;
        ret = ist30xxc_isp_read(data->client, addr, IST30XXC_ISP_READ_TOTAL_B, 
                 buf32, len);    
        if (unlikely(ret))
	    	goto isp_fw_read_end;

        addr += len;
        buf32 += len;
    }
#else
	for (i = 0; i < IST30XXC_FLASH_MAIN_SIZE; i += IST30XXC_DATA_LEN) {
		ret = ist30xxc_isp_read(data->client, addr, IST30XXC_ISP_READ_TOTAL_S, 
                buf32, 1);
		if (unlikely(ret))
			goto isp_fw_read_end;

		addr++;
		buf32++;
	}
#endif
isp_fw_read_end:
	/* IST30xxC ISP disable */
	ist30xxc_isp_enable(data->client, false);
    ist30xxc_reset(data, false);
	return ret;
}

int ist30xxc_isp_fw_update(struct ist30xxc_data *data, const u8 *buf)
{
#if !(I2C_BURST_MODE)
    int i;
#endif
    int ret = 0;
	u32 addr = IST30XXC_FLASH_BASE_ADDR;

    ist30xxc_reset(data, true);

	/* IST30xxC ISP enable */
	ret = ist30xxc_isp_enable(data->client, true);
	if (unlikely(ret))
		goto isp_fw_update_end;

    /* IST30xxC ISP erase */
	ret = ist30xxc_isp_erase(data->client, IST30XXC_ISP_ERASE_BLOCK, 0);
	if (unlikely(ret))
		goto isp_fw_update_end;

#if (IST30XXC_FLASH_INFO_SIZE > 0)
    ret = ist30xxc_isp_erase(data->client, IST30XXC_ISP_ERASE_INFO, 0);
	if (unlikely(ret))
		goto isp_fw_update_end;
#endif

    ist30xxc_reset(data, true);

    /* IST30xxC ISP enable */
	ret = ist30xxc_isp_enable(data->client, true);
	if (unlikely(ret))
		goto isp_fw_update_end;

#if I2C_BURST_MODE
	/* IST30xxC ISP write burst */
#if 0
    for (i = 0; i < IST30XXC_FLASH_TOTAL_SIZE / IST30XXC_FLASH_PAGE_SIZE; i++) {
        ret = ist30xxc_isp_program(data->client, addr, 
                IST30XXC_ISP_PROG_TOTAL_B, (u32 *)buf, 
                IST30XXC_FLASH_PAGE_SIZE / IST30XXC_DATA_LEN);    
        if (unlikely(ret))
	    	goto isp_fw_update_end;

        addr += IST30XXC_FLASH_PAGE_SIZE / IST30XXC_DATA_LEN;
        buf += IST30XXC_FLASH_PAGE_SIZE;
    }
#else
    ret = ist30xxc_isp_program(data->client, addr, IST30XXC_ISP_PROG_TOTAL_B, 
             (u32 *)buf, IST30XXC_FLASH_TOTAL_SIZE / IST30XXC_DATA_LEN);    
    if (unlikely(ret))
       	goto isp_fw_update_end;
#endif
#else
    /* IST30xxC ISP write single */
    for (i = 0; i < IST30XXC_FLASH_TOTAL_SIZE; i += IST30XXC_DATA_LEN) {
		ret = ist30xxc_isp_program(data->client, addr, 
                IST30XXC_ISP_PROG_TOTAL_S, (u32 *)buf, 1);
		if (unlikely(ret))
			goto isp_fw_update_end;

		addr++;
		buf += IST30XXC_DATA_LEN;
	}
#endif

isp_fw_update_end:
	/* IST30xxC ISP disable */
	ist30xxc_isp_enable(data->client, false);
   	ist30xxc_reset(data, false);
	return ret;
}

u32 ist30xxc_parse_ver(struct ist30xxc_data *data, int flag, const u8 *buf)
{
	u32 ver = 0;
	u32 *buf32 = (u32 *)buf;

	if (flag == FLAG_CORE)
		ver = (u32)buf32[(data->tags.flag_addr + 0x1FC) >> 2];
	else if (flag == FLAG_TEST)
		ver = (u32)buf32[(data->tags.flag_addr + 0x1F4) >> 2];
	else if (flag == FLAG_FW)
		ver = (u32)(buf32[(data->tags.cfg_addr + 0x4) >> 2] & 0xFFFF);
	else
		ts_warn("Parsing ver's flag is not corrent!\n");

	return ver;
}

int cal_ms_delay = CAL_WAIT_TIME;
int ist30xxc_calib_wait(struct ist30xxc_data *data)
{
	int cnt = cal_ms_delay;

	data->status.calib_msg = 0;
	while (cnt-- > 0) {
		msleep(100);

		if (data->status.calib_msg) {
			ts_info("Calibration status : %d, Max raw gap : %d - (%08x)\n",
				 CALIB_TO_STATUS(data->status.calib_msg),
				 CALIB_TO_GAP(data->status.calib_msg),
				 data->status.calib_msg);
			if (CALIB_TO_STATUS(data->status.calib_msg) == 0)
				return 0;  // Calibrate success
			else
				return -EAGAIN;
		}
	}
	ts_warn("Calibration time out\n");

	return -EPERM;
}

int ist30xxc_calibrate(struct ist30xxc_data *data, int wait_cnt)
{
	int ret = -ENOEXEC;

	ts_info("*** Calibrate %ds ***\n", cal_ms_delay / 10);

	data->status.update = 1;
    ist30xxc_disable_irq(data);
	while (1) {
		ret = ist30xxc_cmd_calibrate(data->client);
		if (unlikely(ret))
			continue;

		ist30xxc_enable_irq(data);
		ret = ist30xxc_calib_wait(data);
		if (likely(!ret))
			break;

        ist30xxc_disable_irq(data);

        if (--wait_cnt == 0)
            break;

        ist30xxc_reset(data, false);
	}

    ist30xxc_disable_irq(data);
    ist30xxc_reset(data, false);
	data->status.update = 2;
	ist30xxc_enable_irq(data);

	return ret;
}


int ist30xxc_parse_tags(struct ist30xxc_data *data, const u8 *buf, 
        const u32 size)
{
	int ret = -EPERM;
    struct ist30xxc_tags *tags;

	tags = (struct ist30xxc_tags *)(&buf[size - sizeof(struct ist30xxc_tags)]);

	if (!strncmp(tags->magic1, IST30XXC_TAG_MAGIC, sizeof(tags->magic1))
	    && !strncmp(tags->magic2, IST30XXC_TAG_MAGIC, sizeof(tags->magic2))
	    ) {
		data->tags = *tags;

        data->tags.fw_addr -= data->tags.rom_base;
        data->tags.cfg_addr -= data->tags.rom_base;
        data->tags.sensor_addr -= data->tags.rom_base;
        data->tags.cp_addr -= data->tags.rom_base;
        data->tags.flag_addr -= data->tags.rom_base;

        data->fw.index = data->tags.fw_addr;
        data->fw.size = tags->flag_addr - tags->fw_addr + 
				tags->flag_size;
        data->fw.chksum = tags->chksum;

        ts_verb("Tagts magic1: %s, magic2: %s\n", 
                data->tags.magic1, data->tags.magic2);
        ts_verb(" rom: %x\n", data->tags.rom_base);
        ts_verb(" ram: %x\n", data->tags.ram_base);
        ts_verb(" fw: %x(%x)\n", data->tags.fw_addr, data->tags.fw_size);
        ts_verb(" cfg: %x(%x)\n", data->tags.cfg_addr, data->tags.cfg_size);
        ts_verb(" sensor: %x(%x)\n", 
                data->tags.sensor_addr, data->tags.sensor_size);
        ts_verb(" cp: %x(%x)\n", data->tags.cp_addr, data->tags.cp_size);
        ts_verb(" flag: %x(%x)\n", data->tags.flag_addr, data->tags.flag_size);
        ts_verb(" zvalue: %x\n", data->tags.zvalue_base);
        ts_verb(" algo: %x\n", data->tags.algo_base);
        ts_verb(" raw: %x\n", data->tags.raw_base);
        ts_verb(" filter: %x\n", data->tags.filter_base);
        ts_verb(" chksum: %x\n", data->tags.chksum);
        ts_verb(" chksum_all: %x\n", data->tags.chksum_all);
        ts_verb(" build time: %04d/%02d/%02d (%02d:%02d:%02d)\n",
             data->tags.year, data->tags.month, data->tags.day, 
             data->tags.hour, data->tags.min, data->tags.sec);

        ret = 0;
	}

	return ret;
}

int ist30xxc_get_update_info(struct ist30xxc_data *data, const u8 *buf,
			     const u32 size)
{
	int ret;

	ret = ist30xxc_parse_tags(data, buf, size);
	if (unlikely(ret != TAGS_PARSE_OK))
		ts_warn("Cannot find tags of F/W, make a tags by 'tagts.exe'\n");

    return ret;
}

#define TSP_INFO_SWAP_XY    (1 << 0)
#define TSP_INFO_FLIP_X     (1 << 1)
#define TSP_INFO_FLIP_Y     (1 << 2)
u32 ist30xxc_info_cal_crc(u32 *buf)
{
    int i;
    u32 chksum32 = 0;

    for (i = 0; i < IST30XXC_MAX_CMD_SIZE - 1; i++) {
        chksum32 += *buf++;
    }

    return chksum32;
}

int ist30xxc_tsp_update_info(struct ist30xxc_data *data)
{
	int ret = 0;
    u32 chksum;
    u32 info[IST30XXC_MAX_CMD_SIZE];
	u32 tsp_lcd, tsp_swap, tsp_scr, tsp_gtx, tsp_ch;
	u32 tkey_info0, tkey_info1, tkey_info2;
    u32 finger_info, baseline, threshold;
	TSP_INFO *tsp = &data->tsp_info;
	TKEY_INFO *tkey = &data->tkey_info;

    ret = ist30xxc_cmd_hold(data->client, 1);
    if (unlikely(ret))
        return ret;
    
    ret = ist30xxc_burst_read(data->client, 
                IST30XXC_DA_ADDR(eHCOM_GET_CHIP_ID), &info[0], 
                IST30XXC_MAX_CMD_SIZE, true);
    if (unlikely(ret))
        return ret;

    ist30xxc_cmd_hold(data->client, 0);
    if (unlikely(ret)) {
        ist30xxc_reset(data, false);
        return ret;
    }
    
    ret = ist30xxc_read_cmd(data, IST30XXC_REG_CHIPID, &data->chip_id);
    if (unlikely(ret))
        return ret;

    if ((info[IST30XXC_CMD_VALUE(eHCOM_GET_CHIP_ID)] != data->chip_id) && 
            (info[IST30XXC_CMD_VALUE(eHCOM_GET_CHIP_ID)] != 0) &&
            (info[IST30XXC_CMD_VALUE(eHCOM_GET_CHIP_ID)] != 0xFFFF))
        return -EINVAL;

    chksum = ist30xxc_info_cal_crc((u32*)info);
    if (chksum != info[IST30XXC_MAX_CMD_SIZE - 1]) {
        ts_err("info checksum : %08X, %08X\n", 
                chksum, info[IST30XXC_MAX_CMD_SIZE - 1]);
        return -EINVAL;
    }
	ts_info("info read success\n");
    
    tsp_lcd = info[IST30XXC_CMD_VALUE(eHCOM_GET_LCD_INFO)];
    tsp_ch = info[IST30XXC_CMD_VALUE(eHCOM_GET_TSP_INFO)];
    tkey_info0 = info[IST30XXC_CMD_VALUE(eHCOM_GET_KEY_INFO_0)];
    tkey_info1 = info[IST30XXC_CMD_VALUE(eHCOM_GET_KEY_INFO_1)];
    tkey_info2 = info[IST30XXC_CMD_VALUE(eHCOM_GET_KEY_INFO_2)];
    tsp_scr = info[IST30XXC_CMD_VALUE(eHCOM_GET_SCR_INFO)];
    tsp_gtx = info[IST30XXC_CMD_VALUE(eHCOM_GET_GTX_INFO)];
    tsp_swap = info[IST30XXC_CMD_VALUE(eHCOM_GET_SWAP_INFO)];
    finger_info = info[IST30XXC_CMD_VALUE(eHCOM_GET_FINGER_INFO)];
    baseline = info[IST30XXC_CMD_VALUE(eHCOM_GET_BASELINE)];
    threshold = info[IST30XXC_CMD_VALUE(eHCOM_GET_TOUCH_TH)];

	tsp->ch_num.rx = (tsp_ch >> 16) & 0xFFFF;
	tsp->ch_num.tx = tsp_ch & 0xFFFF;

  	tsp->node.len = tsp->ch_num.tx * tsp->ch_num.rx;

	tsp->gtx.num = (tsp_gtx >> 24) & 0xFF;
	tsp->gtx.ch_num[0] = (tsp_gtx >> 16) & 0xFF;
	tsp->gtx.ch_num[1] = (tsp_gtx >> 8) & 0xFF;
	tsp->gtx.ch_num[2] = 0xFF;
	tsp->gtx.ch_num[3] = 0xFF;

	tsp->finger_num = finger_info;
	tsp->dir.swap_xy = (tsp_swap & TSP_INFO_SWAP_XY ? true : false);
	tsp->dir.flip_x = (tsp_swap & TSP_INFO_FLIP_X ? true : false);
	tsp->dir.flip_y = (tsp_swap & TSP_INFO_FLIP_Y ? true : false);

    tsp->baseline = baseline & 0xFFFF;
    tsp->threshold = threshold & 0xFFFF;

    tsp->screen.rx = (tsp_scr >> 16) & 0xFFFF;
	tsp->screen.tx = tsp_scr & 0xFFFF;

	if (tsp->dir.swap_xy) {
        tsp->width = tsp_lcd & 0xFFFF;
	    tsp->height = (tsp_lcd >> 16) & 0xFFFF;
	} else {
        tsp->width = (tsp_lcd >> 16) & 0xFFFF;
    	tsp->height = tsp_lcd & 0xFFFF;
	}

#if IST30XXC_USE_KEY
    tkey->enable = (((tkey_info0 >> 24) & 0xFF) ? true : false);
	tkey->key_num = (tkey_info0 >> 16) & 0xFF;
	tkey->ch_num[0].tx = tkey_info0 & 0xFF;
	tkey->ch_num[0].rx = (tkey_info0 >> 8) & 0xFF;
	tkey->ch_num[1].tx = (tkey_info1 >> 16) & 0xFF;
	tkey->ch_num[1].rx = (tkey_info1 >> 24) & 0xFF;
	tkey->ch_num[2].tx = tkey_info1 & 0xFF;
	tkey->ch_num[2].rx = (tkey_info1 >> 8) & 0xFF;
	tkey->ch_num[3].tx = (tkey_info2 >> 16) & 0xFF;
	tkey->ch_num[3].rx = (tkey_info2 >> 24) & 0xFF;
	tkey->ch_num[4].tx = tkey_info2 & 0xFF;
	tkey->ch_num[4].rx = (tkey_info2 >> 8) & 0xFF;

    tkey->baseline = (baseline >> 16) & 0xFFFF;
    tkey->threshold = (threshold >> 16) & 0xFFFF;
#endif

    return ret;
}

int ist30xxc_get_tsp_info(struct ist30xxc_data *data)
{
	int ret = 0;
    int retry = 3;
#if IST30XXC_INTERNAL_BIN
    TSP_INFO *tsp = &data->tsp_info;
	TKEY_INFO *tkey = &data->tkey_info;
	u8 *cfg_buf;
#endif

    while (retry--) {
        ret = ist30xxc_tsp_update_info(data);
        if (ret == 0) {
            ts_info("tsp update info success!\n");
           	return ret;
        }
    }

#if IST30XXC_INTERNAL_BIN
	cfg_buf = (u8 *)&data->fw.buf[data->tags.cfg_addr];

	tsp->ch_num.rx = (u8)cfg_buf[0x0C];
	tsp->ch_num.tx = (u8)cfg_buf[0x0D];
	tsp->node.len = tsp->ch_num.tx * tsp->ch_num.rx;    

	tsp->gtx.num = (u8)cfg_buf[0x10];
	tsp->gtx.ch_num[0] = (u8)cfg_buf[0x11];
	tsp->gtx.ch_num[1] = (u8)cfg_buf[0x12];
	tsp->gtx.ch_num[2] = (u8)cfg_buf[0x13];
	tsp->gtx.ch_num[3] = (u8)cfg_buf[0x14];

	tsp->finger_num = (u8)cfg_buf[0x1A];
	tsp->dir.swap_xy = (bool)(cfg_buf[0x1B] & TSP_INFO_SWAP_XY ? true : false);
	tsp->dir.flip_x = (bool)(cfg_buf[0x1B] & TSP_INFO_FLIP_X ? true : false);
	tsp->dir.flip_y = (bool)(cfg_buf[0x1B] & TSP_INFO_FLIP_Y ? true : false);

	tsp->baseline = (u16)((cfg_buf[0x31] << 8) | cfg_buf[0x30]);
    tsp->threshold = (u16)((cfg_buf[0x279] << 8) | cfg_buf[0x278]);

	tsp->screen.rx = (u8)cfg_buf[0x0E];
	tsp->screen.tx = (u8)cfg_buf[0x0F];

    if (tsp->dir.swap_xy) {
        tsp->width = (u16)((cfg_buf[0x19] << 8) | cfg_buf[0x18]);
    	tsp->height = (u16)((cfg_buf[0x17] << 8) | cfg_buf[0x16]);
	} else {
        tsp->width = (u16)((cfg_buf[0x17] << 8) | cfg_buf[0x16]);
    	tsp->height = (u16)((cfg_buf[0x19] << 8) | cfg_buf[0x18]);
	}

#if IST30XXC_USE_KEY
    tkey->enable = (bool)(cfg_buf[0x3A] & 1);
	tkey->key_num = (u8)cfg_buf[0x3B];
	tkey->ch_num[0].tx = (u8)cfg_buf[0x3E];
	tkey->ch_num[0].rx = (u8)cfg_buf[0x43];
	tkey->ch_num[1].tx = (u8)cfg_buf[0x3F];
	tkey->ch_num[1].rx = (u8)cfg_buf[0x44];
	tkey->ch_num[2].tx = (u8)cfg_buf[0x40];
	tkey->ch_num[2].rx = (u8)cfg_buf[0x45];
	tkey->ch_num[3].tx = (u8)cfg_buf[0x41];
	tkey->ch_num[3].rx = (u8)cfg_buf[0x46];
	tkey->ch_num[4].tx = (u8)cfg_buf[0x42];
	tkey->ch_num[4].rx = (u8)cfg_buf[0x47];
	
    tkey->baseline = (u16)((cfg_buf[0x3D] << 8) | cfg_buf[0x3C]);
    tkey->threshold = (u16)((cfg_buf[0x27F] << 8) | cfg_buf[0x27E]);
#endif
#endif    

	return ret;
}

void ist30xxc_print_info(struct ist30xxc_data *data)
{
	TSP_INFO *tsp = &data->tsp_info;
	TKEY_INFO *tkey = &data->tkey_info;

	ts_info("*** TSP/TKEY info ***\n");
	ts_info("TSP info: \n");
	ts_info(" finger num: %d\n", tsp->finger_num);
	ts_info(" dir swap: %d, flip x: %d, y: %d\n",
		 tsp->dir.swap_xy, tsp->dir.flip_x, tsp->dir.flip_y);
	ts_info(" baseline: %d\n", tsp->baseline);
    ts_info(" threshold: %d\n", tsp->threshold);
	ts_info(" ch_num tx: %d, rx: %d\n", tsp->ch_num.tx, tsp->ch_num.rx);
	ts_info(" screen tx: %d, rx: %d\n", tsp->screen.tx, tsp->screen.rx);
	ts_info(" width: %d, height: %d\n", tsp->width, tsp->height);
	ts_info(" gtx num: %d, ch [1]: %d, [2]: %d, [3]: %d, [4]: %d\n",
		 tsp->gtx.num, tsp->gtx.ch_num[0], tsp->gtx.ch_num[1],
		 tsp->gtx.ch_num[2], tsp->gtx.ch_num[3]);
	ts_info(" node len: %d\n", tsp->node.len);
	ts_info("TKEY info: \n");
	ts_info(" enable: %d, key num: %d\n", tkey->enable, tkey->key_num);
	ts_info(" ch [0]: %d,%d [1]: %d,%d [2]: %d,%d [3]: %d,%d [4]: %d,%d\n",
		 tkey->ch_num[0].tx, tkey->ch_num[0].rx, tkey->ch_num[1].tx,
		 tkey->ch_num[1].rx, tkey->ch_num[2].tx, tkey->ch_num[2].rx,
		 tkey->ch_num[3].tx, tkey->ch_num[3].rx, tkey->ch_num[4].tx,
		 tkey->ch_num[4].rx);
	ts_info(" baseline : %d\n", tkey->baseline);
    ts_info(" threshold: %d\n", tkey->threshold);
}

#define update_next_step(ret)   { if (unlikely(ret)) goto end; }
int ist30xxc_fw_update(struct ist30xxc_data *data, const u8 *buf, int size)
{
	int ret = 0;
	u32 chksum = 0;
	struct ist30xxc_fw *fw = &data->fw;

	ts_info("*** Firmware update ***\n");
	ts_info(" core: %x, fw: %x, test: %x (addr: 0x%x ~ 0x%x)\n",
		 fw->bin.core_ver, fw->bin.fw_ver, fw->bin.test_ver,
		 fw->index, (fw->index + fw->size));

	data->status.update = 1;
    data->status.update_result = 0;

	ist30xxc_disable_irq(data);

	ret = ist30xxc_isp_fw_update(data, buf);
	update_next_step(ret);

    ret = ist30xxc_read_cmd(data, eHCOM_GET_CRC32, &chksum);
	if (unlikely((ret) || (chksum != fw->chksum))) {
        if (unlikely(ret))
            ist30xxc_reset(data, false);

        goto end;
    }

	ret = ist30xxc_get_ver_info(data);
	update_next_step(ret);

end:
	if (unlikely(ret)) {
        data->status.update_result = 1;
		ts_warn("Firmware update Fail!, ret=%d\n", ret);
	} else if (unlikely(chksum != fw->chksum)) {
        data->status.update_result = 1;
		ts_warn("Error CheckSum: %x(%x)\n", chksum, fw->chksum);
		ret = -ENOEXEC;
	}

	ist30xxc_enable_irq(data);

	data->status.update = 2;

	return ret;
}

int ist30xxc_fw_recovery(struct ist30xxc_data *data)
{
	int ret = -EPERM;
	u8 *fw = data->fw.buf;
	int fw_size = data->fw.buf_size;

	ret = ist30xxc_get_update_info(data, fw, fw_size);
    if (ret)
        return ret;

	data->fw.bin.core_ver = ist30xxc_parse_ver(data, FLAG_CORE, fw);
	data->fw.bin.fw_ver = ist30xxc_parse_ver(data, FLAG_FW, fw);
	data->fw.bin.test_ver = ist30xxc_parse_ver(data, FLAG_TEST, fw);

	mutex_lock(&ist30xxc_mutex);
	ret = ist30xxc_fw_update(data, fw, fw_size);
    if (ret == 0)
    	ist30xxc_calibrate(data, 1);
	mutex_unlock(&ist30xxc_mutex);

	ist30xxc_start(data);

	return ret;
}

#if IST30XXC_INTERNAL_BIN
int ist30xxc_check_fw(struct ist30xxc_data *data, const u8 *buf)
{
	int ret;
	u32 chksum;

	ret = ist30xxc_read_cmd(data, eHCOM_GET_CRC32, &chksum);
	if (unlikely(ret)) {
        ist30xxc_reset(data, false);
		return ret;
    }

	if (unlikely(chksum != data->fw.chksum)) {
		ts_warn("Checksum compare error, (IC: %08x, Bin: %08x)\n",
			 chksum, data->fw.chksum);
		return -EPERM;
	}

	return 0;
}

bool ist30xxc_check_valid_vendor(u32 tsp_vendor)
{
#if (IMAGIS_TSP_IC < IMAGIS_IST3038C)
    switch (tsp_vendor) {
	case TSP_TYPE_ALPS:
	case TSP_TYPE_EELY:
	case TSP_TYPE_TOP:
	case TSP_TYPE_MELFAS:
	case TSP_TYPE_ILJIN:
	case TSP_TYPE_SYNOPEX:
	case TSP_TYPE_SMAC:
	case TSP_TYPE_TAEYANG:
	case TSP_TYPE_TOVIS:
	case TSP_TYPE_ELK:
	case TSP_TYPE_OTHERS:
		return true;
	default:
		return false;
	}
#else
    if (tsp_vendor < TSP_TYPE_NO)
        return true;
#endif
	
    return false;
}

#if (IMAGIS_TSP_IC < IMAGIS_IST3038C)
#if IST30XXC_MULTIPLE_TSP
void ist30xxc_set_tsp_fw(struct ist30xxc_data *data)
{
	char *str;
	struct ist30xxc_fw *fw = &data->fw;

	switch (data->tsp_type) {
	case TSP_TYPE_ILJIN:
		str = "ALPS/ILJIN";
		fw->buf = (u8 *)ist30xxc_fw;
		fw->buf_size = sizeof(ist30xxc_fw);
		break;
	case TSP_TYPE_EELY:
		str = "EELY";
		fw->buf = (u8 *)ist30xxc_fw2;
		fw->buf_size = sizeof(ist30xxc_fw2);
		break;

	case TSP_TYPE_UNKNOWN:
	default:
		str = "Unknown";
		ts_warn("Unknown TSP vendor(0x%x)\n", data->tsp_type);
		break;
	}
	ts_info("TSP vendor : %s(%x)\n", str, data->tsp_type);
}
#endif  // IST30XXC_MULTIPLE_TSP
#endif  // (IMAGIS_TSP_IC < IMAGIS_IST3038C)

int ist30xxc_check_auto_update(struct ist30xxc_data *data)
{
	int ret = 0;
    int retry = IST30XXC_MAX_RETRY_CNT;
	u32 tsp_type = TSP_TYPE_UNKNOWN;
	bool tsp_check = false;
   	u32 chksum;
	struct ist30xxc_fw *fw = &data->fw;

    while (retry--) {
		ret = ist30xxc_read_cmd(data, eHCOM_GET_TSP_VENDOR, &tsp_type);
		if (likely(ret == 0)) {
			if (likely(ist30xxc_check_valid_vendor(tsp_type) == true))
				tsp_check = true;
			break;
		}
        
        ist30xxc_reset(data, false);
	}
    
    ts_info("TSP vendor: %x\n", tsp_type);

    if (unlikely(!tsp_check))
		goto fw_check_end;

	ist30xxc_get_ver_info(data);

	if (likely((fw->cur.fw_ver > 0) && (fw->cur.fw_ver < 0xFFFFFFFF))) {
		ts_info("Version compare IC: %x(%x), BIN: %x(%x)\n", fw->cur.fw_ver, 
                fw->cur.core_ver, fw->bin.fw_ver, fw->bin.core_ver);

		/* If FW version is same, check FW checksum */
		if (likely((fw->cur.core_ver == fw->bin.core_ver) &&
			   (fw->cur.fw_ver == fw->bin.fw_ver) &&
			   (fw->cur.test_ver == 0))) {
			ret = ist30xxc_read_cmd(data, eHCOM_GET_CRC32, &chksum);
			if (unlikely((ret) || (chksum != fw->chksum))) {
				ts_warn("Checksum error, IC: %x, Bin: %x (ret: %d)\n",
					 chksum, fw->chksum, ret);
				goto fw_check_end;
			}
		}

		/*
		 *  fw->cur.core_ver : Core version in TSP IC
		 *  fw->cur.fw_ver : FW version if TSP IC
		 *  fw->bin.core_ver : Core version in FW Binary
		 *  fw->bin.fw_ver : FW version in FW Binary
		 */
		/* If the ver of binary is higher than ver of IC, FW update operate. */

		if (likely((fw->cur.core_ver >= fw->bin.core_ver) &&
			   (fw->cur.fw_ver >= fw->bin.fw_ver)))
			return 0;
	}

fw_check_end:
	return -EAGAIN;
}

int ist30xxc_auto_bin_update(struct ist30xxc_data *data)
{
	int ret = 0;
	int retry = IST30XXC_MAX_RETRY_CNT;
	struct ist30xxc_fw *fw = &data->fw;

	fw->buf = (u8 *)ist30xxc_fw;
	fw->buf_size = sizeof(ist30xxc_fw);

#if (IMAGIS_TSP_IC < IMAGIS_IST3038C)
#if IST30XXC_MULTIPLE_TSP
	ist30xxc_set_tsp_fw(data);
#endif
#endif

    ret = ist30xxc_get_update_info(data, fw->buf, fw->buf_size);
    if (unlikely(ret))
        return 1;
    fw->bin.core_ver = ist30xxc_parse_ver(data, FLAG_CORE, fw->buf);
	fw->bin.fw_ver = ist30xxc_parse_ver(data, FLAG_FW, fw->buf);
	fw->bin.test_ver = ist30xxc_parse_ver(data, FLAG_TEST, fw->buf);

    ts_info("IC: %x, Binary ver core: %x, fw: %x, test: %x\n",
		 data->chip_id, fw->bin.core_ver, fw->bin.fw_ver, fw->bin.test_ver);

	mutex_lock(&ist30xxc_mutex);
	ret = ist30xxc_check_auto_update(data);
	mutex_unlock(&ist30xxc_mutex);

	if (likely(ret >= 0))
		return ret;

update_bin:   // TSP is not ready / FW update
	ts_info("Update version. fw(core, test): %x(%x, %x) -> %x(%x, %x)\n",
		 fw->cur.fw_ver, fw->cur.core_ver, fw->cur.test_ver,
		 fw->bin.fw_ver, fw->bin.core_ver, fw->bin.test_ver);

	mutex_lock(&ist30xxc_mutex);
	while (retry--) {
		ret = ist30xxc_fw_update(data, fw->buf, fw->buf_size);
		if (unlikely(!ret))
			break;
	}
	mutex_unlock(&ist30xxc_mutex);

	if (unlikely(ret))
		return ret;

	if (unlikely(retry > 0 && ist30xxc_check_fw(data, fw->buf)))
		goto update_bin;

    mutex_lock(&ist30xxc_mutex);
	ist30xxc_calibrate(data, IST30XXC_MAX_RETRY_CNT);
    mutex_unlock(&ist30xxc_mutex);

	return ret;
}
#endif // IST30XXC_INTERNAL_BIN

#define MAX_FILE_PATH   255
/* sysfs: /sys/class/touch/firmware/firmware */
ssize_t ist30xxc_fw_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t size)
{
	int ret;
	int fw_size = 0;
	u8 *fw = NULL;
	const u8 *buff = 0;
	mm_segment_t old_fs = { 0 };
	struct file *fp = NULL;
	long fsize = 0, nread = 0;
	char fw_path[MAX_FILE_PATH];
	const struct firmware *request_fw = NULL;
	int mode = 0;
	int calib = 1;
    struct ist30xxc_data *data = dev_get_drvdata(dev);

	sscanf(buf, "%d %d", &mode, &calib);

	switch (mode) {
	case MASK_UPDATE_INTERNAL:
#if IST30XXC_INTERNAL_BIN
		fw = data->fw.buf;
		fw_size = data->fw.buf_size;
#else
		ts_warn("Not support internal bin!!\n", );
		return size;
#endif
		break;

	case MASK_UPDATE_FW:
		ret = request_firmware(&request_fw, IST30XXC_FW_NAME,
				       &data->client->dev);
		if (ret) {
			ts_warn("File not found, %s\n", IST30XXC_FW_NAME);
			return size;
		}

		fw = (u8 *)request_fw->data;
		fw_size = (u32)request_fw->size;
        ts_info("firmware is loaded!!\n");
		break;

        case MASK_UPDATE_SDCARD:
		old_fs = get_fs();
		set_fs(get_ds());

		snprintf(fw_path, MAX_FILE_PATH, "/sdcard/%s",
			 IST30XXC_FW_NAME);
		fp = filp_open(fw_path, O_RDONLY, 0);
		if (IS_ERR(fp)) {
			ts_info("file %s open error:%d\n", fw_path, (s32)fp);
			goto err_file_open;
		}

		fsize = fp->f_path.dentry->d_inode->i_size;

		buff = kzalloc((size_t)fsize, GFP_KERNEL);
		if (!buff) {
			ts_info("fail to alloc buffer\n");
			goto err_alloc;
		}

		nread = vfs_read(fp, (char __user *)buff, fsize, &fp->f_pos);
		if (nread != fsize) {
			ts_info("mismatch fw size\n");
			goto err_fw_size;
		}

		fw = (u8 *)buff;
		fw_size = (u32)fsize;

		filp_close(fp, current->files);
		ts_info("firmware is loaded!!\n");
		break;

	case MASK_UPDATE_ERASE:
		ts_info("EEPROM all erase!!\n");

		mutex_lock(&ist30xxc_mutex);
		ist30xxc_disable_irq(data);
        ist30xxc_reset(data, true);
		ist30xxc_isp_enable(data->client, true);
        ist30xxc_isp_erase(data->client, IST30XXC_ISP_ERASE_BLOCK, 0);
#if (IST30XXC_FLASH_INFO_SIZE > 0)        
        ist30xxc_isp_erase(data->client, IST30XXC_ISP_ERASE_INFO, 0);
#endif
        ist30xxc_isp_enable(data->client, false);
		ist30xxc_reset(data, false);
        ist30xxc_start(data);
		ist30xxc_enable_irq(data);
		mutex_unlock(&ist30xxc_mutex);

	default:
		return size;
	}

	ret = ist30xxc_get_update_info(data, fw, fw_size);
    if (ret)
        goto err_get_info;
        
	data->fw.bin.core_ver = ist30xxc_parse_ver(data, FLAG_CORE, fw);
	data->fw.bin.fw_ver = ist30xxc_parse_ver(data, FLAG_FW, fw);
	data->fw.bin.test_ver = ist30xxc_parse_ver(data, FLAG_TEST, fw);

	mutex_lock(&ist30xxc_mutex);
	ist30xxc_fw_update(data, fw, fw_size);

	if (calib)
		ist30xxc_calibrate(data, 1);
	mutex_unlock(&ist30xxc_mutex);

	ist30xxc_start(data);

err_get_info:
	if (request_fw != NULL)
		release_firmware(request_fw);

	if (fp != NULL) {
err_fw_size:
		kfree(buff);
err_alloc:
		filp_close(fp, NULL);
err_file_open:
		set_fs(old_fs);
	}

	return size;
}

/* sysfs: /sys/class/touch/firmware/fw_sdcard */
ssize_t ist30xxc_fw_sdcard_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
    int ret = 0;
	int fw_size = 0;
	u8 *fw = NULL;
	const u8 *buff = 0;
	mm_segment_t old_fs = { 0 };
	struct file *fp = NULL;
	long fsize = 0, nread = 0;
	char fw_path[MAX_FILE_PATH];
    struct ist30xxc_data *data = dev_get_drvdata(dev);

	old_fs = get_fs();
	set_fs(get_ds());

	snprintf(fw_path, MAX_FILE_PATH, "/sdcard/%s",
		 IST30XXC_FW_NAME);
	fp = filp_open(fw_path, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		ts_info("file %s open error:%d\n", fw_path, (s32)fp);
		goto err_file_open;
	}

	fsize = fp->f_path.dentry->d_inode->i_size;

	buff = kzalloc((size_t)fsize, GFP_KERNEL);
	if (!buff) {
		ts_info("fail to alloc buffer\n");
		goto err_alloc;
	}

	nread = vfs_read(fp, (char __user *)buff, fsize, &fp->f_pos);
	if (nread != fsize) {
		ts_info("mismatch fw size\n");
		goto err_fw_size;
	}

	fw = (u8 *)buff;
	fw_size = (u32)fsize;

	filp_close(fp, current->files);
	ts_info("firmware is loaded!!\n");

	ret = ist30xxc_get_update_info(data, fw, fw_size);
    if (ret)
        goto err_get_info;
	data->fw.bin.core_ver = ist30xxc_parse_ver(data, FLAG_CORE, fw);
	data->fw.bin.fw_ver = ist30xxc_parse_ver(data, FLAG_FW, fw);
	data->fw.bin.test_ver = ist30xxc_parse_ver(data, FLAG_TEST, fw);

	mutex_lock(&ist30xxc_mutex);
	ist30xxc_fw_update(data, fw, fw_size);
	mutex_unlock(&ist30xxc_mutex);

	ist30xxc_start(data);

err_get_info:
err_fw_size:
	if (buff)
		kfree(buff);
err_alloc:
	if (fp)
		filp_close(fp, NULL);
err_file_open:
	set_fs(old_fs);

	return 0;
}

/* sysfs: /sys/class/touch/firmware/firmware */
ssize_t ist30xxc_fw_status_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int count;
    struct ist30xxc_data *data = dev_get_drvdata(dev);

	switch (data->status.update) {
	case 1:
		count = sprintf(buf, "Downloading\n");
		break;
	case 2:
        if (data->status.update_result) {
    		count = sprintf(buf, "Update fail\n");
        } else {
		    count = sprintf(buf, "Update success, ver %x(%x, %x)-> %x(%x, %x), "
				"status : %d, gap : %d\n",
				data->fw.prev.fw_ver, data->fw.prev.core_ver,
                data->fw.prev.test_ver, data->fw.cur.fw_ver, 
                data->fw.cur.core_ver, data->fw.cur.test_ver,
				CALIB_TO_STATUS(data->status.calib_msg),
				CALIB_TO_GAP(data->status.calib_msg));
        }
		break;
	default:
		count = sprintf(buf, "Pass\n");
	}

	return count;
}

/* sysfs: /sys/class/touch/firmware/fw_read */
u32 buf32_eflash[IST30XXC_FLASH_TOTAL_SIZE / IST30XXC_DATA_LEN];
ssize_t ist30xxc_fw_read_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int i;
    mm_segment_t old_fs = { 0 };
    struct file *fp = NULL;
    char fw_path[MAX_FILE_PATH];
    u8 *buf8 = (u8 *)buf32_eflash;
    struct ist30xxc_data *data = dev_get_drvdata(dev);

	mutex_lock(&ist30xxc_mutex);
	ist30xxc_disable_irq(data);

   	ist30xxc_isp_fw_read(data, buf32_eflash);
	for (i = 0; i < IST30XXC_FLASH_TOTAL_SIZE; i += 16) {
		ts_debug("%07x: %02x%02x %02x%02x %02x%02x %02x%02x "
			  "%02x%02x %02x%02x %02x%02x %02x%02x\n", i,
			  buf8[i], buf8[i + 1], buf8[i + 2], buf8[i + 3],
			  buf8[i + 4], buf8[i + 5], buf8[i + 6], buf8[i + 7],
			  buf8[i + 8], buf8[i + 9], buf8[i + 10], buf8[i + 11],
			  buf8[i + 12], buf8[i + 13], buf8[i + 14], buf8[i + 15]);
	}

    old_fs = get_fs();
	set_fs(get_ds());

    snprintf(fw_path, MAX_FILE_PATH, "/sdcard/%s", IST30XXC_BIN_NAME);
    fp = filp_open(fw_path, O_CREAT|O_WRONLY|O_TRUNC, 0);
    if (IS_ERR(fp)) {
		ts_info("file %s open error:%d\n", fw_path, (s32)fp);
		goto err_file_open;
	}
    
    fp->f_op->write(fp, buf8, IST30XXC_FLASH_TOTAL_SIZE, &fp->f_pos);
    fput(fp);

	filp_close(fp, NULL);
	set_fs(old_fs);

err_file_open:
	ist30xxc_enable_irq(data);
	mutex_unlock(&ist30xxc_mutex);

	ist30xxc_start(data);

	return 0;
}

/* sysfs: /sys/class/touch/firmware/version */
ssize_t ist30xxc_fw_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int count;
    struct ist30xxc_data *data = dev_get_drvdata(dev);

	count = sprintf(buf, "ID: %x, f/w core: %x, fw: %x, test: %x\n",
			data->chip_id, data->fw.cur.core_ver,
			data->fw.cur.fw_ver, data->fw.cur.test_ver);

#if IST30XXC_INTERNAL_BIN
	{
		char msg[128];

		ist30xxc_get_update_info(data, data->fw.buf, data->fw.buf_size);

		count += snprintf(msg, sizeof(msg),
				  " Header - f/w ver core: %x, fw: %x, test: %x\r\n",
				  ist30xxc_parse_ver(data, FLAG_CORE, data->fw.buf),
				  ist30xxc_parse_ver(data, FLAG_FW, data->fw.buf),
				  ist30xxc_parse_ver(data, FLAG_TEST, data->fw.buf));
		strncat(buf, msg, sizeof(msg));
	}
#endif

	return count;
}


/* sysfs  */
static DEVICE_ATTR(fw_read, S_IRUGO, ist30xxc_fw_read_show, NULL);
static DEVICE_ATTR(firmware, S_IRUGO | S_IWUSR | S_IWGRP, ist30xxc_fw_status_show,
		   ist30xxc_fw_store);
static DEVICE_ATTR(fw_sdcard, S_IRUGO, ist30xxc_fw_sdcard_show, NULL);
static DEVICE_ATTR(version, S_IRUGO, ist30xxc_fw_version_show, NULL);

struct class *ist30xxc_class;
struct device *ist30xxc_fw_dev;

static struct attribute *fw_attributes[] = {
	&dev_attr_fw_read.attr,
	&dev_attr_firmware.attr,
	&dev_attr_fw_sdcard.attr,
	&dev_attr_version.attr,
	NULL,
};

static struct attribute_group fw_attr_group = {
	.attrs	= fw_attributes,
};

int ist30xxc_init_update_sysfs(struct ist30xxc_data *data)
{
	/* /sys/class/touch */
	ist30xxc_class = class_create(THIS_MODULE, "touch");

	/* /sys/class/touch/firmware */
	ist30xxc_fw_dev = device_create(ist30xxc_class, NULL, 0, data, "firmware");

	/* /sys/class/touch/firmware/... */
	if (unlikely(sysfs_create_group(&ist30xxc_fw_dev->kobj, &fw_attr_group)))
		ts_err("Failed to create sysfs group(%s)!\n", "firmware");

	data->status.update = 0;
	data->status.calib = 0;
    data->status.update_result = 0;

	return 0;
}
