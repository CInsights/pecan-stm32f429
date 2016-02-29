#include "ch.h"
#include "hal.h"

#include "debug.h"
#include "modules.h"
#include "ov9655.h"
#include "pi2c.h"
#include "ssdv.h"
#include "aprs.h"
#include "base64.h"
#include <string.h>

static uint32_t image_id;

void encode_ssdv(uint8_t *image, uint32_t image_len, module_params_t* parm) {
	ssdv_t ssdv;
	uint8_t pkt[SSDV_PKT_SIZE];
	uint8_t pkt_base64[BASE64LEN(SSDV_PKT_SIZE)];
	uint16_t i = 0;
	uint8_t *b;
	uint16_t bi = 0;
	uint8_t c = SSDV_OK;

	// Init SSDV (FEC at 2FSK, non FEC at APRS)
	ssdv_enc_init(&ssdv, parm->protocol == PROT_SSDV_2FSK ? SSDV_TYPE_NORMAL : SSDV_TYPE_NOFEC, SSDV_CALLSIGN, ++image_id);
	ssdv_enc_set_buffer(&ssdv, pkt);

	while(true)
	{
		parm->lastCycle = chVTGetSystemTimeX(); // Update Watchdog timer

		while((c = ssdv_enc_get_packet(&ssdv)) == SSDV_FEED_ME)
		{
			b = &image[bi];
			uint8_t r = bi < image_len-128 ? 128 : image_len - bi;
			bi += 128;

			if(r <= 0)
			{
				TRACE_ERROR("SSDV > Premature end of file");
				break;
			}
			ssdv_enc_feed(&ssdv, b, r);
		}

		if(c == SSDV_EOI)
		{
			TRACE_INFO("SSDV > ssdv_enc_get_packet said EOI");
			break;
		} else if(c != SSDV_OK) {
			TRACE_ERROR("SSDV > ssdv_enc_get_packet failed: %i", c);
			return;
		}

		// Transmit packet
		radioMSG_t msg;
		uint32_t (*fptr)(void);
		fptr = parm->frequencyMethod;

		switch(parm->protocol) {
			case PROT_SSDV_APRS_2GFSK:
				TRACE_ERROR("IMG  > 2GFSK not yet implemented")
				break;

			case PROT_SSDV_APRS_AFSK:
				msg.mod = MOD_AFSK;
				msg.freq = (*fptr)();
				msg.power = parm->power;
				base64_encode(pkt, pkt_base64, sizeof(pkt));
				msg.bin_len = aprs_encode_image(msg.msg, pkt_base64, sizeof(pkt_base64));
				while(!transmitOnRadio(&msg)) // Try to insert message into message box less aggressively
					chThdSleepMilliseconds(2000);
				break;

			case PROT_SSDV_2FSK:
				msg.mod = MOD_2FSK;
				msg.freq = (*fptr)();
				msg.power = parm->power;
				memcpy(msg.msg, pkt, sizeof(pkt));
				msg.bin_len = 8*sizeof(pkt);
				while(!transmitOnRadio(&msg)) // Try to insert message into message box less aggressively
					chThdSleepMilliseconds(2000);
				break;

			default:
				TRACE_ERROR("IMG  > Unsupported protocol selected for module IMAGE");
		}

		i++;
	}

	TRACE_INFO("SSDV > %i packets", i);
}

THD_FUNCTION(moduleIMG, arg) {
	// Print infos
	module_params_t* parm = (module_params_t*)arg;
	TRACE_INFO("IMG  > Startup module IMAGE");
	TRACE_MODULE_INFO(parm, "IMG", "IMAGE");

	systime_t time = chVTGetSystemTimeX();
	while(true)
	{
		parm->lastCycle = chVTGetSystemTimeX(); // Watchdog timer
		TRACE_INFO("IMG  > Do module IMAGE cycle");

		//chThdSleepMilliseconds(60000);

		// Init I2C
		TRACE_INFO("IMG  > Init camera I2C");
		i2cCamInit();

		// Init camera
		OV9655_init();

		// Sample data from DCMI through DMA into RAM
		TRACE_INFO("IMG  > Capture image");
		palClearPad(PORT(LED_YELLOW), PIN(LED_YELLOW)); // Yellow LED shows when image is captured
		uint8_t tries = 5; // Try 5 times at maximum
		bool status;
		do { // Try capturing image until capture successful
			status = OV9655_Snapshot2RAM();
		} while(!status && --tries);
		palSetPad(PORT(LED_YELLOW), PIN(LED_YELLOW));

		uint8_t *image;
		uint32_t image_len = OV9655_getBuffer(&image);

		// Switch off camera
		OV9655_deinit();

		if(tries) { // Capture successful
			TRACE_INFO("IMG  > Encode/Transmit SSDV");
			encode_ssdv(image, image_len, parm);
		} else {
			TRACE_ERROR("IMG  > Image capturing failed");
		}

		time = chThdSleepUntilWindowed(time, time + S2ST(parm->cycle)); // Wait until time + cycletime
	}
}













