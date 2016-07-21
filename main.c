#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#include "ptime.h"
#include "config.h"
#include "debug.h"
#include "modules.h"
#include "padc.h"
#include "pi2c.h"
#include "pac1720.h"
#include "bme280.h"
#include "sd.h"

static virtual_timer_t vt;			// Virtual timer for LED blinking
uint32_t counter = 0;				// Main thread counter
bool error = 0;						// Error LED flag
bool led_on = false;
systime_t wdg_buffer = S2ST(60);	// Software thread monitor buffer

// Hardware Watchdog configuration
static const WDGConfig wdgcfg = {
	STM32_IWDG_PR_256,
	STM32_IWDG_RL(10000),
	STM32_IWDG_WIN_DISABLED
};

/**
  * LED blinking routine
  * RED LED blinks: One or more modules crashed (software watchdog)
  * GREEN LED blinks: I'm alive! (STM32 crashed if not blinking)
  * YELLOW LED: Camera takes a photo (See image.c)
  */
static void led_cb(void *led_sw) {
	#if MIN_LED_VBAT != 0
	// Switch off LEDs below battery threshold
	if(!led_on)
	{
		palSetPad(PORT(LED_1RED), PIN(LED_1RED));
		palSetPad(PORT(LED_2YELLOW), PIN(LED_2YELLOW));
		palSetPad(PORT(LED_3GREEN), PIN(LED_3GREEN));
		palSetPad(PORT(LED_4GREEN), PIN(LED_4GREEN));

		chSysLockFromISR();
		chVTSetI(&vt, MS2ST(500), led_cb, led_sw);
		chSysUnlockFromISR();

		return;
	}
	#endif

	// Switch LEDs
	palWritePad(PORT(LED_3GREEN), PIN(LED_3GREEN), (bool)led_sw);	// Show I'M ALIVE
	if(error) {
		palWritePad(PORT(LED_1RED), PIN(LED_1RED), (bool)led_sw);	// Show error
	} else {
		palSetPad(PORT(LED_1RED), PIN(LED_1RED));	// Shut off error
	}

	led_sw = (void*)!led_sw; // Set next state

	chSysLockFromISR();
	chVTSetI(&vt, MS2ST(500), led_cb, led_sw);
	chSysUnlockFromISR();
}

/**
  * Main routine is starting up system, runs the software watchdog (module monitoring), controls LEDs
  */
int main(void) {
	halInit();					// Startup HAL
	chSysInit();				// Startup RTOS

	DEBUG_INIT();				// Debug Init (Serial debug port, LEDs)
	TRACE_INFO("MAIN > Startup");

	pi2cInit();					// Startup I2C
	initEssentialModules();		// Startup required modules (input/output modules)
	initModules();				// Startup optional modules (eg. POSITION, LOG, ...)
	pac1720_init();				// Startup current measurement
	initSD();					// Startup SD

	chThdSleepMilliseconds(100);

	// Initialize LED timer
	chVTObjectInit(&vt);
	chVTSet(&vt, MS2ST(500), led_cb, 0);

	chThdSleepMilliseconds(1000);

	// Initialize Watchdog
	wdgStart(&WDGD1, &wdgcfg);
	wdgReset(&WDGD1);

	while(true) {
		// Print time every 10 sec
		if(counter % 10 == 0)
			PRINT_TIME("MAIN");

		// Thread monitor
		bool aerror = false; // Temporary error flag
		bool healthy;
		systime_t lu;

		for(uint8_t i=0; i<sizeof(config)/sizeof(module_conf_t); i++) {
			
			if(config[i].active) { // Is active?

				// Determine health
				healthy = true;
				switch(config[i].trigger.type)
				{
					case TRIG_ONCE:
						healthy = true;
						break;

					case TRIG_EVENT:
						switch(config[i].trigger.event)
						{
							case NO_EVENT:
								healthy = true;
								break;
							case EVENT_NEW_POINT:
								healthy = config[i].last_update + S2ST(TRACK_CYCLE_TIME) + wdg_buffer > chVTGetSystemTimeX();
								break;
						}
						break;

					case TRIG_TIMEOUT:
						healthy = config[i].last_update + S2ST(config[i].trigger.timeout) + wdg_buffer > chVTGetSystemTimeX();
						break;

					case TRIG_CONTINOUSLY:
						healthy = config[i].last_update + wdg_buffer > chVTGetSystemTimeX();
						break;
				}
				healthy = healthy || config[i].init_delay + wdg_buffer > chVTGetSystemTimeX();

				// Debugging every 10 sec
				if(counter % 10 == 0) {
					lu = chVTGetSystemTimeX() - config[i].last_update;
					if(healthy) {
						TRACE_INFO("WDG  > Module %s OK (last activity %d.%03d sec ago)", config[i].name, ST2MS(lu)/1000, ST2MS(lu)%1000);
					} else {
						TRACE_ERROR("WDG  > Module %s failed (last activity %d.%03d sec ago)", config[i].name, ST2MS(lu)/1000, ST2MS(lu)%1000);
					}
				}

				if(!healthy)
					aerror = true; // Set error flag

			}
		}

		// Watchdog RADIO
		healthy = watchdog_radio + wdg_buffer > chVTGetSystemTimeX();
		lu = chVTGetSystemTimeX() - watchdog_radio;

		if(counter % 10 == 0) {
			if(healthy) {
				TRACE_INFO("WDG  > Module RAD OK (last activity %d.%03d sec ago)", ST2MS(lu)/1000, ST2MS(lu)%1000);
			} else {
				TRACE_ERROR("WDG  > Module RAD failed (last activity %d.%03d sec ago)", ST2MS(lu)/1000, ST2MS(lu)%1000);
			}
		}
		if(!healthy)
			aerror = true; // Set error flag

		// Watchdog TRACKING
		healthy = watchdog_tracking + S2ST(TRACK_CYCLE_TIME) + wdg_buffer > chVTGetSystemTimeX();
		lu = chVTGetSystemTimeX() - watchdog_radio;
		if(counter % 10 == 0) {
			if(healthy) {
				TRACE_INFO("WDG  > Module TRAC OK (last activity %d.%03d sec ago)", ST2MS(lu)/1000, ST2MS(lu)%1000);
			} else {
				TRACE_ERROR("WDG  > Module TRAC failed (last activity %d.%03d sec ago)", ST2MS(lu)/1000, ST2MS(lu)%1000);
			}
		}
		if(!healthy)
			aerror = true; // Set error flag

		// Update hardware (LED, WDG)
		error = aerror;			// Update error LED flag
		if(!error)
		{
			wdgReset(&WDGD1);	// Reset hardware watchdog at no error
		} else {
			TRACE_ERROR("WDG  > No reset");
		}

		led_on = getBatteryVoltageMV() >= MIN_LED_VBAT; // Switch on LEDs if battery voltage above threshold

		chThdSleepMilliseconds(1000);
		counter++;
	}
}

