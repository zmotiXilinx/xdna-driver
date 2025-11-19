// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include "aie2_pci.h"
#include "aie2_msg_priv.h"
#include "amdxdna_mgmt.h"

#define AIE2_MGMT_APP_ID		0xFF

static const char * const fw_log_level_str[] = {
	"OFF",
	"ERR",
	"WRN",
	"INF",
	"DBG",
	"MAX"
};

struct fw_rbf_entry_header {
  u8  magic;           // 0xCA
  u8  payload_words;   // size in multiple of sizeof(ipu_log_ring_entry_payload_word_t) of payload (excluding this header and corresponding footer)
  u16 sequence_number; // 1 - 2^16-1: valid entries
  u32 rsvd;            // reserved: set to zero
} __packed;

struct fw_rbf_entry_footer {
  u32 rsvd;            // reserved: set to zero
  u16 sequence_number; // 1 - 2^16-1: valid entries
  u8  payload_words;   // size in multiple of sizeof(ipu_log_ring_entry_payload_word_t) of payload (excluding this header and corresponding footer)
  u8  magic;           // 0xBA
} __packed;

void aie2_fw_log_parse(struct amdxdna_dev *xdna, char *buffer, size_t size)
{
	char *end = buffer + size;

	if (!size)
		return;

	while (buffer < end) {
		struct fw_rbf_entry_header *entry_header = (struct fw_rbf_entry_header *)buffer;
		struct fw_log_header {
			u64 timestamp;
			u32 format      : 1;
			u32 reserved_1  : 7;
			u32 level       : 3;
			u32 reserved_11 : 5;
			u32 appn        : 8;
			u32 argc        : 8;
			u32 line        : 16;
			u32 module      : 16;
		} *header;
		const u32 header_size = sizeof(struct fw_log_header);
		char appid[20];
		u32 msg_size;

		header = (struct fw_log_header *) (buffer + sizeof(struct fw_rbf_entry_header));

		if (header->format != FW_LOG_FORMAT_FULL || !header->argc || header->level > 4) {
			XDNA_ERR(xdna, "Potential buffer overflow or corruption!\n");
			buffer += AMDXDNA_DPT_FW_LOG_MSG_ALIGN;
			continue;
		}

		msg_size = (header->argc) * sizeof(u32);
		struct fw_rbf_entry_footer *entry_footer = (struct fw_rbf_entry_footer *)(buffer + sizeof(struct fw_rbf_entry_header) + entry_header->payload_words * sizeof(u64));
		const u32 entry_size = sizeof(struct fw_rbf_entry_header) + sizeof(struct fw_rbf_entry_footer) + entry_header->payload_words * sizeof(u64);
		if (entry_size > size) {
			XDNA_ERR(xdna, "Log entry size exceeds available buffer size");
			return;
		}

		msleep(1);

		if (entry_footer->magic != 0xBA)
			XDNA_ERR(xdna, "Log entry footer magic is corrupted: 0x%x", entry_footer->magic);
		if (entry_footer->sequence_number != entry_header->sequence_number)
			XDNA_ERR(xdna, "Log entry footer sequence number is corrupted: %d != %d", entry_footer->sequence_number, entry_header->sequence_number);
		if (entry_footer->payload_words != entry_header->payload_words)
			XDNA_ERR(xdna, "Log entry footer payload words is corrupted: %d != %d", entry_footer->payload_words, entry_header->payload_words);
		if (entry_header->magic != 0xCA)
			XDNA_ERR(xdna, "Log entry header magic is corrupted: 0x%x", entry_header->magic);

		if (header->appn == AIE2_MGMT_APP_ID)
			scnprintf(appid, sizeof(appid), "MGMNT");
		else
			scnprintf(appid, sizeof(appid), "APP%2d", header->appn);

		XDNA_INFO(xdna, "[%lld] [%d] [%s] [%s]: %s", header->timestamp, entry_header->sequence_number,
			  fw_log_level_str[header->level], appid, (char *)(buffer + header_size + sizeof(struct fw_rbf_entry_header)));

		buffer += ALIGN(header_size + msg_size + sizeof(struct fw_rbf_entry_footer) + sizeof(struct fw_rbf_entry_header), AMDXDNA_DPT_FW_LOG_MSG_ALIGN);
	}
}

int aie2_fw_log_init(struct amdxdna_dev *xdna, size_t size, u8 level)
{
	struct amdxdna_mgmt_dma_hdl *dma_hdl = xdna->fw_log->dma_hdl;
	u32 msi_idx, msi_address;
	int ret;

	if (level >= MAX_FW_LOG_LEVEL) {
		XDNA_ERR(xdna,  "Invalid firmware log level: %d", level);
		return -EINVAL;
	}

	mutex_lock(&xdna->dev_handle->aie2_lock);
	ret = aie2_calibrate_time(xdna->dev_handle);
	if (ret) {
		XDNA_ERR(xdna, "Failed to calibrate time: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}
	ret = aie2_config_fw_log(xdna->dev_handle, dma_hdl, size, &msi_idx, &msi_address);
	if (ret) {
		/* Sliently fail for device generation that don't support FW logging */
		if (ret != -EOPNOTSUPP)
			XDNA_ERR(xdna, "Failed to init fw log buffer: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}

	ret = aie2_set_log_level(xdna->dev_handle, level);
	if (ret) {
		XDNA_ERR(xdna, "Failed to init fw log level: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}

	ret = aie2_set_log_format(xdna->dev_handle, FW_LOG_FORMAT_FULL);
	if (ret) {
		XDNA_ERR(xdna, "Failed to init fw log format: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}

	ret = aie2_set_log_destination(xdna->dev_handle, FW_LOG_DESTINATION_DRAM);
	if (ret) {
		XDNA_ERR(xdna, "Failed to init fw log destination: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}
	mutex_unlock(&xdna->dev_handle->aie2_lock);

	xdna->fw_log->io_base = xdna->dev_handle->mbox_base;
	xdna->fw_log->msi_address = msi_address & AIE2_DPT_MSI_ADDR_MASK;
	xdna->fw_log->msi_idx = msi_idx;

	return ret;
}

int aie2_fw_log_config(struct amdxdna_dev *xdna, u8 level)
{
	int ret;

	if (level == FW_LOG_LEVEL_NONE || level >= MAX_FW_LOG_LEVEL) {
		XDNA_ERR(xdna,  "Invalid firmware log level: %d", level);
		return -EINVAL;
	}

	mutex_lock(&xdna->dev_handle->aie2_lock);
	ret = aie2_set_log_level(xdna->dev_handle, level);
	if (ret)
		XDNA_ERR(xdna, "Failed to init fw log level: %d", ret);
	mutex_unlock(&xdna->dev_handle->aie2_lock);

	return ret;
}

int aie2_fw_log_fini(struct amdxdna_dev *xdna)
{
	struct amdxdna_mgmt_dma_hdl *dma_hdl = xdna->fw_log->dma_hdl;
	int ret;

	mutex_lock(&xdna->dev_handle->aie2_lock);
	ret = aie2_set_log_destination(xdna->dev_handle, FW_LOG_DESTINATION_FIXED);
	if (ret) {
		/* Sliently fail for device generation that don't support FW logging */
		if (ret != -EOPNOTSUPP)
			XDNA_ERR(xdna, "Failed to reset fw log destination: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}

	ret = aie2_config_fw_log(xdna->dev_handle, dma_hdl, 0, NULL, NULL);
	if (ret) {
		XDNA_ERR(xdna, "Failed to reset fw log buffer: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}
	mutex_unlock(&xdna->dev_handle->aie2_lock);
	return 0;
}

void aie2_fw_trace_parse(struct amdxdna_dev *xdna, char *buffer, size_t size)
{
	if (!size)
		return;


	u32 num_entries = size / 32;

	for (size_t i = 0; i < num_entries; ++i) {
		struct fw_rbf_entry_header *entry_header = (struct fw_rbf_entry_header *) buffer;
		struct fw_rbf_entry_footer *entry_footer = (struct fw_rbf_entry_footer *) (buffer + sizeof(struct fw_rbf_entry_header) + entry_header->payload_words * sizeof(u64));
		struct trace_payload {
			u64 timestamp;
			u64 payload;
		} *payload;
		payload = (struct trace_payload *) (buffer + sizeof(struct fw_rbf_entry_header));
		if (entry_footer->magic != 0xBA)
			XDNA_ERR(xdna, "Log entry footer magic is corrupted: 0x%x", entry_footer->magic);
		if (entry_footer->sequence_number != entry_header->sequence_number)
			XDNA_ERR(xdna, "Log entry footer sequence number is corrupted: %d != %d", entry_footer->sequence_number, entry_header->sequence_number);
		if (entry_footer->payload_words != entry_header->payload_words)
			XDNA_ERR(xdna, "Log entry footer payload words is corrupted: %d != %d", entry_footer->payload_words, entry_header->payload_words);
		if (entry_header->magic != 0xCA)
			XDNA_ERR(xdna, "Log entry header magic is corrupted: 0x%x", entry_header->magic);

		XDNA_INFO(xdna, "[%lld] [%05d] payload: 0x%016llx", payload->timestamp, entry_header->sequence_number, payload->payload);
		buffer += 32;
	}
}

int aie2_fw_trace_init(struct amdxdna_dev *xdna, size_t size, u32 categories)
{
	struct amdxdna_mgmt_dma_hdl *dma_hdl = xdna->fw_trace->dma_hdl;
	u32 msi_idx, msi_address;
	int ret;

	mutex_lock(&xdna->dev_handle->aie2_lock);
	ret = aie2_calibrate_time(xdna->dev_handle);
	if (ret) {
		XDNA_ERR(xdna, "Failed to calibrate time: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}
	ret = aie2_start_fw_trace(xdna->dev_handle, dma_hdl, size, categories, &msi_idx,
				  &msi_address);
	if (ret) {
		/* Sliently fail for device generation that don't support FW tracing */
		if (ret != -EOPNOTSUPP)
			XDNA_ERR(xdna, "Failed to init fw trace buffer: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}
	mutex_unlock(&xdna->dev_handle->aie2_lock);

	xdna->fw_trace->io_base = xdna->dev_handle->mbox_base;
	xdna->fw_trace->msi_address = msi_address & AIE2_DPT_MSI_ADDR_MASK;
	xdna->fw_trace->msi_idx = msi_idx;

	return ret;
}

int aie2_fw_trace_config(struct amdxdna_dev *xdna, u32 categories)
{
	int ret;

	mutex_lock(&xdna->dev_handle->aie2_lock);
	ret = aie2_set_trace_categories(xdna->dev_handle, categories);
	if (ret)
		XDNA_ERR(xdna, "Failed to init fw trace categories: %d", ret);
	mutex_unlock(&xdna->dev_handle->aie2_lock);

	return ret;
}

int aie2_fw_trace_fini(struct amdxdna_dev *xdna)
{
	int ret;

	mutex_lock(&xdna->dev_handle->aie2_lock);
	ret = aie2_stop_fw_trace(xdna->dev_handle);
	if (ret) {
		XDNA_ERR(xdna, "Failed to stop fw trace: %d", ret);
		mutex_unlock(&xdna->dev_handle->aie2_lock);
		return ret;
	}
	mutex_unlock(&xdna->dev_handle->aie2_lock);
	return 0;
}
