/* brickletboot
 * Copyright (C) 2016 Olaf Lüke <olaf@tinkerforge.com>
 *
 * tinydma.c: sam0 DMA driver with small memory/flash footprint
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "tinydma.h"

#include <string.h>
#include "clock.h"
#include "system_interrupt.h"

struct _dma_module {
	volatile bool _dma_init;
	volatile uint32_t allocated_channels;
	uint8_t free_channels;
};

struct _dma_module _dma_inst = {
	._dma_init = false,
	.allocated_channels = 0,
	.free_channels = CONF_MAX_USED_CHANNEL_NUM,
};

/** Maximum retry counter for resuming a job transfer. */
#define MAX_JOB_RESUME_COUNT    10000

/** DMA channel mask. */
#define DMA_CHANNEL_MASK   (0x1f)


/** Internal DMA resource pool. */
static struct dma_resource* _dma_active_resource[CONF_MAX_USED_CHANNEL_NUM];

/* DMA channel interrup flag. */
uint8_t g_chan_interrupt_flag[CONF_MAX_USED_CHANNEL_NUM]={0};

/**
 * \brief Find a free channel for a DMA resource.
 *
 * Find a channel for the requested DMA resource.
 *
 * \return Status of channel allocation.
 * \retval DMA_INVALID_CHANNEL  No channel available
 * \retval count          Allocated channel for the DMA resource
 */
static uint8_t _dma_find_first_free_channel_and_allocate(void)
{
	uint8_t count;
	uint32_t tmp;
	bool allocated = false;

	system_interrupt_enter_critical_section();

	tmp = _dma_inst.allocated_channels;

	for (count = 0; count < CONF_MAX_USED_CHANNEL_NUM; ++count) {
		if (!(tmp & 0x00000001)) {
			/* If free channel found, set as allocated and return
			 *number */

			_dma_inst.allocated_channels |= 1 << count;
			_dma_inst.free_channels--;
			allocated = true;

			break;
		}

		tmp = tmp >> 1;
	}

	system_interrupt_leave_critical_section();

	if (!allocated) {
		return DMA_INVALID_CHANNEL;
	} else {
		return count;
	}
}

/**
 * \brief Release an allocated DMA channel.
 *
 * \param[in]  channel  Channel id to be released
 *
 */
static void _dma_release_channel(uint8_t channel)
{
	_dma_inst.allocated_channels &= ~(1 << channel);
	_dma_inst.free_channels++;
}

/**
 * \brief Configure the DMA resource.
 *
 * \param[in]  dma_resource Pointer to a DMA resource instance
 * \param[out] resource_config Configurations of the DMA resource
 *
 */
static void _dma_set_config(struct dma_resource *resource,
		struct dma_resource_config *resource_config)
{
	Assert(resource);
	Assert(resource_config);
	uint32_t temp_CHCTRLB_reg;
	system_interrupt_enter_critical_section();

	/** Select the DMA channel and clear software trigger */
	DMAC->CHID.reg = DMAC_CHID_ID(resource->channel_id);
	DMAC->SWTRIGCTRL.reg &= (uint32_t)(~(1 << resource->channel_id));

	temp_CHCTRLB_reg = DMAC_CHCTRLB_LVL(resource_config->priority) | \
			DMAC_CHCTRLB_TRIGSRC(resource_config->peripheral_trigger) | \
			DMAC_CHCTRLB_TRIGACT(resource_config->trigger_action);


	if(resource_config->event_config.input_action){
	temp_CHCTRLB_reg |= DMAC_CHCTRLB_EVIE | DMAC_CHCTRLB_EVACT(
				resource_config->event_config.input_action);
	}

	/** Enable event output, the event output selection is configured in
	 * each transfer descriptor  */
	if (resource_config->event_config.event_output_enable) {
		temp_CHCTRLB_reg |= DMAC_CHCTRLB_EVOE;
	}

	/* Write config to CTRLB register */
	DMAC->CHCTRLB.reg = temp_CHCTRLB_reg;



	system_interrupt_leave_critical_section();
}

void dma_get_config_defaults(struct dma_resource_config *config)
{
	Assert(config);
	/* Set as priority 0 */
	config->priority = DMA_PRIORITY_LEVEL_0;
	/* Only software/event trigger */
	config->peripheral_trigger = 0;
	/* Transaction trigger */
	config->trigger_action = DMA_TRIGGER_ACTION_TRANSACTION;

	/* Event configurations, no event input/output */
	config->event_config.input_action = DMA_EVENT_INPUT_NOACT;
	config->event_config.event_output_enable = false;
#ifdef FEATURE_DMA_CHANNEL_STANDBY
	config->run_in_standby = false;
#endif
}

/**
 * \brief Allocate a DMA with configurations.
 *
 * This function will allocate a proper channel for a DMA transfer request.
 *
 * \param[in,out]  dma_resource Pointer to a DMA resource instance
 * \param[in] transfer_config Configurations of the DMA transfer
 *
 * \return Status of the allocation procedure.
 *
 * \retval STATUS_OK The DMA resource was allocated successfully
 * \retval STATUS_ERR_NOT_FOUND DMA resource allocation failed
 */
enum status_code dma_allocate(struct dma_resource *resource,
		struct dma_resource_config *config)
{
	uint8_t new_channel;

	Assert(resource);

	system_interrupt_enter_critical_section();

	/* Find the proper channel */
	new_channel = _dma_find_first_free_channel_and_allocate();

	/* If no channel available, return not found */
	if (new_channel == DMA_INVALID_CHANNEL) {
		system_interrupt_leave_critical_section();

		return STATUS_ERR_NOT_FOUND;
	}

	/* Set the channel */
	resource->channel_id = new_channel;

	/** Perform a reset for the allocated channel */
	DMAC->CHID.reg = DMAC_CHID_ID(resource->channel_id);
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

#ifdef FEATURE_DMA_CHANNEL_STANDBY
	if(config->run_in_standby){
		DMAC->CHCTRLA.reg |= DMAC_CHCTRLA_RUNSTDBY;
	}
#endif

	/** Configure the DMA control,channel registers and descriptors here */
	_dma_set_config(resource, config);

	resource->descriptor = NULL;

	/* Log the DMA resource into the internal DMA resource pool */
	_dma_active_resource[resource->channel_id] = resource;

	system_interrupt_leave_critical_section();

	return STATUS_OK;
}

/**
 * \brief Free an allocated DMA resource.
 *
 * This function will free an allocated DMA resource.
 *
 * \param[in,out] resource Pointer to the DMA resource
 *
 * \return Status of the free procedure.
 *
 * \retval STATUS_OK The DMA resource was freed successfully
 * \retval STATUS_BUSY The DMA resource was busy and can't be freed
 * \retval STATUS_ERR_NOT_INITIALIZED DMA resource was not initialized
 */
enum status_code dma_free(struct dma_resource *resource)
{
	Assert(resource);
	Assert(resource->channel_id != DMA_INVALID_CHANNEL);

	system_interrupt_enter_critical_section();

	/* Check if channel is busy */
	if (dma_is_busy(resource)) {
		system_interrupt_leave_critical_section();
		return STATUS_BUSY;
	}

	/* Check if DMA resource was not allocated */
	if (!(_dma_inst.allocated_channels & (1 << resource->channel_id))) {
		system_interrupt_leave_critical_section();
		return STATUS_ERR_NOT_INITIALIZED;
	}

	/* Release the DMA resource */
	_dma_release_channel(resource->channel_id);

	/* Reset the item in the DMA resource pool */
	_dma_active_resource[resource->channel_id] = NULL;

	system_interrupt_leave_critical_section();

	return STATUS_OK;
}


#if 0
/**
 * \brief Abort a DMA transfer.
 *
 * This function will abort a DMA transfer. The DMA channel used for the DMA
 * resource will be disabled.
 * The block transfer count will also be calculated and written to the DMA
 * resource structure.
 *
 * \note The DMA resource will not be freed after calling this function.
 *       The function \ref dma_free() can be used to free an allocated resource.
 *
 * \param[in,out] resource Pointer to the DMA resource
 *
 */
void dma_abort_job(struct dma_resource *resource)
{
	uint32_t write_size;
	uint32_t total_size;

	Assert(resource);
	Assert(resource->channel_id != DMA_INVALID_CHANNEL);

	system_interrupt_enter_critical_section();

	DMAC->CHID.reg = DMAC_CHID_ID(resource->channel_id);
	DMAC->CHCTRLA.reg = 0;

	system_interrupt_leave_critical_section();

	/* Get transferred size */
	total_size = descriptor_section[resource->channel_id].BTCNT.reg;
	write_size = _write_back_section[resource->channel_id].BTCNT.reg;
	resource->transfered_size = total_size - write_size;

	resource->job_status = STATUS_ABORTED;
}
#endif
/**
 * \brief Suspend a DMA transfer.
 *
 * This function will request to suspend the transfer of the DMA resource.
 * The channel is kept enabled, can receive transfer triggers (the transfer
 * pending bit will be set), but will be removed from the arbitration scheme.
 * The channel operation can be resumed by calling \ref dma_resume_job().
 *
 * \note This function sets the command to suspend the DMA channel
 * associated with a DMA resource. The channel suspend interrupt flag
 * indicates whether the transfer is truly suspended.
 *
 * \param[in] resource Pointer to the DMA resource
 *
 */
void dma_suspend_job(struct dma_resource *resource)
{
	Assert(resource);
	Assert(resource->channel_id != DMA_INVALID_CHANNEL);

	system_interrupt_enter_critical_section();

	/* Select the channel */
	DMAC->CHID.reg = DMAC_CHID_ID(resource->channel_id);

	/* Send the suspend request */
	DMAC->CHCTRLB.reg |= DMAC_CHCTRLB_CMD_SUSPEND;

	system_interrupt_leave_critical_section();
}

/**
 * \brief Resume a suspended DMA transfer.
 *
 * This function try to resume a suspended transfer of a DMA resource.
 *
 * \param[in] resource Pointer to the DMA resource
 *
 */
void dma_resume_job(struct dma_resource *resource)
{
	uint32_t bitmap_channel;
	uint32_t count = 0;

	Assert(resource);
	Assert(resource->channel_id != DMA_INVALID_CHANNEL);

	/* Get bitmap of the allocated DMA channel */
	bitmap_channel = (1 << resource->channel_id);

	/* Check if channel was suspended */
	if (resource->job_status != STATUS_SUSPEND) {
		return;
	}

	system_interrupt_enter_critical_section();

	/* Send resume request */
	DMAC->CHID.reg = DMAC_CHID_ID(resource->channel_id);
	DMAC->CHCTRLB.reg |= DMAC_CHCTRLB_CMD_RESUME;

	system_interrupt_leave_critical_section();

	/* Check if transfer job resumed */
	for (count = 0; count < MAX_JOB_RESUME_COUNT; count++) {
		if ((DMAC->BUSYCH.reg & bitmap_channel) == bitmap_channel) {
			break;
		}
	}

	if (count < MAX_JOB_RESUME_COUNT) {
		/* Job resumed */
		resource->job_status = STATUS_BUSY;
	} else {
		/* Job resume timeout */
		resource->job_status = STATUS_ERR_TIMEOUT;
	}
}

/**
 * \brief Create a DMA transfer descriptor with configurations.
 *
 * This function will set the transfer configurations to the DMA transfer
 * descriptor.
 *
 * \param[in] descriptor Pointer to the DMA transfer descriptor
 * \param[in] config Pointer to the descriptor configuration structure
 *
 */
void dma_descriptor_create(DmacDescriptor* descriptor,
	struct dma_descriptor_config *config)
{
	/* Set block transfer control */
	descriptor->BTCTRL.bit.VALID = config->descriptor_valid;
	descriptor->BTCTRL.bit.EVOSEL = config->event_output_selection;
	descriptor->BTCTRL.bit.BLOCKACT = config->block_action;
	descriptor->BTCTRL.bit.BEATSIZE = config->beat_size;
	descriptor->BTCTRL.bit.SRCINC = config->src_increment_enable;
	descriptor->BTCTRL.bit.DSTINC = config->dst_increment_enable;
	descriptor->BTCTRL.bit.STEPSEL = config->step_selection;
	descriptor->BTCTRL.bit.STEPSIZE = config->step_size;

	/* Set transfer size, source address and destination address */
	descriptor->BTCNT.reg = config->block_transfer_count;
	descriptor->SRCADDR.reg = config->source_address;
	descriptor->DSTADDR.reg = config->destination_address;

	/* Set next transfer descriptor address */
	descriptor->DESCADDR.reg = config->next_descriptor_address;
}

/**
 * \brief Add a DMA transfer descriptor to a DMA resource.
 *
 * This function will add a DMA transfer descriptor to a DMA resource.
 * If there was a transfer descriptor already allocated to the DMA resource,
 * the descriptor will be linked to the next descriptor address.
 *
 * \param[in] resource Pointer to the DMA resource
 * \param[in] descriptor Pointer to the transfer descriptor
 *
 * \retval STATUS_OK The descriptor is added to the DMA resource
 * \retval STATUS_BUSY The DMA resource was busy and the descriptor is not added
 */
enum status_code dma_add_descriptor(struct dma_resource *resource,
		DmacDescriptor* descriptor)
{
	DmacDescriptor* desc = resource->descriptor;

	if (resource->job_status == STATUS_BUSY) {
		return STATUS_BUSY;
	}

	/* Look up for an empty space for the descriptor */
	if (desc == NULL) {
		resource->descriptor = descriptor;
	} else {
		/* Looking for end of descriptor link */
		while(desc->DESCADDR.reg != 0) {
			desc = (DmacDescriptor*)(desc->DESCADDR.reg);
		}

		/* Set to the end of descriptor list */
		desc->DESCADDR.reg = (uint32_t)descriptor;
	}

	return STATUS_OK;
}

void tinydma_init(DmacDescriptor *descriptor_section, DmacDescriptor *write_back_section) {
	// Initialize clocks for DMA
#if (SAML21) || (SAML22) || (SAMC20) || (SAMC21) || (SAMR30)
	system_ahb_clock_set_mask(MCLK_AHBMASK_DMAC);
#else
	system_ahb_clock_set_mask(PM_AHBMASK_DMAC);
	system_apb_clock_set_mask(SYSTEM_CLOCK_APB_APBB,
			PM_APBBMASK_DMAC);
#endif

	// Perform a software reset before enable DMA controller
	DMAC->CTRL.reg &= ~DMAC_CTRL_DMAENABLE;
	DMAC->CTRL.reg = DMAC_CTRL_SWRST;

	// Setup descriptor base address and write back section base address
	DMAC->BASEADDR.reg = (uint32_t)descriptor_section;
	DMAC->WRBADDR.reg = (uint32_t)write_back_section;

	// Enable all priority level at the same time
	DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN(0xf);

	system_interrupt_enable(SYSTEM_INTERRUPT_MODULE_DMA);
}

void tinydma_start_transfer(const uint8_t channel_id) {
	system_interrupt_enter_critical_section();

	// Enable transfer
	DMAC->CHID.reg = DMAC_CHID_ID(channel_id);
	DMAC->CHCTRLA.reg |= DMAC_CHCTRLA_ENABLE;

	system_interrupt_leave_critical_section();
}