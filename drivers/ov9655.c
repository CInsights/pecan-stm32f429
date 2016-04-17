/*
	Takes an VGA image of OV9655 camera and encodes it in realtime to JPEG.
	The image has a capture time of 264ms.
	For sampling a picture PA0 must be pulled high. (User button on STM32F429 discovery board)
	The picture is then transmitted by serial interface on PD5 (38k2 8n1).
	PG13 is blinking (Green LED on STM32F429 discovery board) when the uC is
	ideling. So when the picture has been transmitted completely PG13 is
	blinking.

	This example does not need any external SDRAM. It just uses it's internal
	SRAM. So this example should also work on the STM32F407 discovery board.

	The inital code came from http://mikrocontroller.bplaced.net/wordpress/?page_id=1115

	The camera must be connected to

	Camera Data pin		STM32 designator	Possible Hardware Pins for DCMI		Pin used in this implementation
	HREF				HSYNC				PA4 PH8								PA4
	PCLK				PIXCLK				PA6									PA6
	VSYNC				VSYNC				PB7 PG9 PI5							PB7
	D2					D0					PA9 PC6 PH9							PC6
	D3					D1					PA10 PC7 PH10						PC7
	D4					D2					PC8 PE0 PG10 PH11					PC8
	D5					D3					PC9 PE1 PG11 PH12					PC9
	D6					D4					PC11 PE4 PH14						PE4
	D7					D5					PB6 PD3 PI4							PB6
	D8					D6					PB8 PE5 PI6							PE5
	D9					D7					PB9 PE6 PI7							PE6
	XCLK				none				PA8 (clockout, not DCMI specific)	PA8

	RET and PWDN can be left unconnected
*/

#include "ch.h"
#include "hal.h"
#include "ov9655.h"
#include "pi2c.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_dcmi.h"
#include "board.h"
#include "defines.h"

#include "jpegant/arch.h"
#include "jpegant/dct.h"
#include "jpegant/jpegenc.h"

#include <string.h>

#ifdef QVGA
#define OV9655_MAXX					320
#define OV9655_MAXY					240
#else
#define OV9655_MAXX					640
#define OV9655_MAXY					480
#endif

#define OV9655_BUFFER_SIZE			OV9655_MAXX*OV9655_MAXY
#define OV9655_DCMI_BASE_ADR		((uint32_t)0x50050000)
#define OV9655_DCMI_REG_DR_OFFSET	0x28
#define OV9655_DCMI_REG_DR_ADDRESS	(OV9655_DCMI_BASE_ADR | OV9655_DCMI_REG_DR_OFFSET)

uint16_t ov9655_ram_buffer[OV9655_BUFFER_SIZE];

BGR RGB16x16[16][16] __attribute__((section(".ram4")));
color_t R[16][16], G[16][16], B[16][16] __attribute__((section(".ram4")));
conv Y8x8[2][2][8][8] __attribute__((section(".ram4"))); // four 8x8 blocks - 16x16
conv Cb8x8[8][8] __attribute__((section(".ram4")));
conv Cr8x8[8][8] __attribute__((section(".ram4")));
uint64_t subsclk = 0;
uint64_t readclk = 0;
uint8_t jpeg[50*1024] __attribute__((section(".ram4"))); // JPEG output buffer
uint32_t jpeg_pos = 0;
bool ov9655_samplingFinished;


// I2C camera configuration for QVGA resolution
static const uint8_t OV9655_CONFIG[] =
{
	0x00,0x00,0x01,0x80,0x02,0x80,0x03,0x02,0x04,0x03,0x09,0x01,
	0x0b,0x57,0x0e,0x61,0x0f,0x40,0x13,0xc7,0x14,0x3a,0x16,0x24,
	0x17,0x18,0x18,0x04,0x19,0x01,0x1a,0x81,0x1e,0x00,0x24,0x3c,
	0x25,0x36,0x26,0x72,0x27,0x08,0x28,0x08,0x29,0x15,0x2a,0x00,
	0x2b,0x00,0x2c,0x08,0x33,0x00,0x34,0x3f,0x35,0x00,0x36,0x3a,
	0x38,0x72,0x39,0x57,0x3a,0xcc,0x3b,0x04,0x3d,0x99,0x3e,0x02,
	0x3f,0xc1,0x40,0xc0,0x41,0x41,0x42,0xc0,0x43,0x0a,0x44,0xf0,
	0x45,0x46,0x46,0x62,0x47,0x2a,0x48,0x3c,0x4a,0xfc,0x4b,0xfc,
	0x4c,0x7f,0x4d,0x7f,0x4e,0x7f,0x4f,0x98,0x50,0x98,0x51,0x00,
	0x52,0x28,0x53,0x70,0x54,0x98,0x58,0x1a,0x59,0x85,0x5a,0xa9,
	0x5b,0x64,0x5c,0x84,0x5d,0x53,0x5e,0x0e,0x5f,0xf0,0x60,0xf0,
	0x61,0xf0,0x62,0x00,0x63,0x00,0x64,0x02,0x65,0x20,0x66,0x00,
	0x69,0x0a,
	#ifdef SEC1_80
	0x6b,0xda,
	#else
	0x6b,0x5a,
	#endif
	0x6c,0x04,0x6d,0x55,0x6e,0x00,0x6f,0x9d,
	0x70,0x21,0x71,0x78,0x74,0x10,0x75,0x10,0x76,0x01,0x77,0x02,
	0x7A,0x12,0x7B,0x08,0x7C,0x16,0x7D,0x30,0x7E,0x5e,0x7F,0x72,
	0x80,0x82,0x81,0x8e,0x82,0x9a,0x83,0xa4,0x84,0xac,0x85,0xb8,
	0x86,0xc3,0x87,0xd6,0x88,0xe6,0x89,0xf2,0x8a,0x24,0x8c,0x80,
	0x90,0x7d,0x91,0x7b,0x9d,0x02,0x9e,0x02,0x9f,0x7a,0xa0,0x79,
	0xa1,0x40,0xa4,0x50,0xa5,0x68,0xa6,0x4a,0xa8,0xc1,0xa9,0xef,
	0xaa,0x92,0xab,0x04,0xac,0x80,0xad,0x80,0xae,0x80,0xaf,0x80,
	0xb2,0xf2,0xb3,0x20,0xb4,0x20,0xb5,0x00,0xb6,0xaf,0xb6,0xaf,
	0xbb,0xae,0xbc,0x7f,0xbd,0x7f,0xbe,0x7f,0xbf,0x7f,0xbf,0x7f,
	0xc0,0xaa,0xc1,0xc0,0xc2,0x01,0xc3,0x4e,0xc6,0x05,0xc9,0xe0,
	0xca,0xe8,0xcb,0xf0,0xcc,0xd8,0xcd,0x93,0x12,0x63,0x40,0x10,
	0x11,0x00,
	#ifdef QVGA
	0x15,0x08,0x32,0x12,0x72,0x11,0x73,0x01,0xc7,0x81
	#else
	0x15,0x08,0x32,0x09,0x72,0x00,0x73,0x00,0xc7,0x80
	#endif
};


// RGB to YCbCr Conversion:
// Y = 0.299*R + 0.587*G + 0.114*B
__inline color RGB2Y(const color r, const color g, const color b)
{
	return (32768 + 19595*r + 38470*g + 7471*b) >> 16;
}
// Cb = 128 - 0.1687*R - 0.3313*G + 0.5*B
__inline color RGB2Cb(const color r, const color g, const color b)
{
	return (8421376 - 11058*r - 21709*g + 32767*b) >> 16;
}
// Cr = 128 + 0.5*R - 0.4187*G - 0.0813*B
__inline color RGB2Cr(const color r, const color g, const color b)
{
	return (8421376 + 32767*r - 27438*g - 5329*b) >> 16;
}

void subsample2(BGR rgb[16][16], conv Y[2][2][8][8], conv cb[8][8], conv cr[8][8])
{
	unsigned r, c;

	for (r = 0; r < 16; r += 2)
	for (c = 0; c < 16; c += 2)
	{
		//unsigned rr, cc;
		unsigned i, j, k, l;
		unsigned sR, sG, sB;
		unsigned R, G, B;

		i = r >> 3, j = c >> 3,
		k = r & 7, l = c & 7;

		sR = R = rgb[r][c].Red,
		sG = G = rgb[r][c].Green,
		sB = B = rgb[r][c].Blue;
		Y[i][j][k][l] = RGB2Y(R, G, B)-128;

		sR += R = rgb[r][c+1].Red,
		sG += G = rgb[r][c+1].Green,
		sB += B = rgb[r][c+1].Blue;
		Y[i][j][k][l+1] = RGB2Y(R, G, B)-128;

		sR += R = rgb[r+1][c].Red,
		sG += G = rgb[r+1][c].Green,
		sB += B = rgb[r+1][c].Blue;
		Y[i][j][k+1][l] = RGB2Y(R, G, B)-128;

		sR += R = rgb[r+1][c+1].Red,
		sG += G = rgb[r+1][c+1].Green,
		sB += B = rgb[r+1][c+1].Blue;
		Y[i][j][k+1][l+1] = RGB2Y(R, G, B)-128;

		// calculating an average values
		R = sR >> 2,
		G = sG >> 2,
		B = sB >> 2;

		cb[r>>1][c>>1] = (conv)RGB2Cb(R, G, B)-128;
		cr[r>>1][c>>1] = (conv)RGB2Cr(R, G, B)-128;
	}
}

void write_jpeg(const unsigned char buff[], unsigned size)
{
	if(jpeg_pos+size > sizeof(jpeg)) // Buffer overflow protection
		size = sizeof(jpeg)-jpeg_pos;
	
	memcpy(&jpeg[jpeg_pos], buff, size);
	jpeg_pos += size;
}

/**
  * Captures an image from the camera.
  * Returns false in case of an error.
  */
bool OV9655_Snapshot2RAM(void)
{
	// Reset output pointer
	jpeg_pos = 0;

	// Add JPEG header to buffer
	huffman_start(OV9655_MAXY & -16, OV9655_MAXX & -16);

	// DCMI init
	palClearPad(PORT(LED_YELLOW), PIN(LED_YELLOW)); // Yellow LED shows when image is captured
	OV9655_InitDCMI();

	// Encode JPEG data
	ov9655_samplingFinished = false;
	systime_t timeout = chVTGetSystemTimeX() + MS2ST(1000); // Timeout 1sec
	while(!ov9655_samplingFinished && chVTGetSystemTimeX() < timeout)
		chThdSleepMilliseconds(1);

	palSetPad(PORT(LED_YELLOW), PIN(LED_YELLOW));
	chThdSleepMilliseconds(1);						// Tiny break to mark encoding (visible on oscilloscope)
	palClearPad(PORT(LED_YELLOW), PIN(LED_YELLOW)); // Yellow LED shows when image is encoded

	if(chVTGetSystemTimeX() >= timeout) { // Timeout has occurred

		return false;

	} else { // Data successfully sampled

		unsigned y,x,xb,yb;
		uint16_t color;

		for(y = 0; y < OV9655_MAXY-15; y += 16)
		{
			for(x = 0; x < OV9655_MAXX-15; x += 16)
			{
				// Create 16x16 block
				for(yb=0; yb<16; yb++) {
					for(xb=0; xb<16; xb++) {
						color=ov9655_ram_buffer[(yb+y)*OV9655_MAXX + (xb+x)];
						RGB16x16[yb][xb].Blue = ((color&0x001F)<<3);	// 5bit blue
						RGB16x16[yb][xb].Green = ((color&0x07E0)>>3);	// 6bit green
						RGB16x16[yb][xb].Red = ((color&0xF800)>>8);		// 5bit red
					}
				}

				// Encode JPEG
				// getting subsampled Cb and Cr
				subsample2(RGB16x16, Y8x8, Cb8x8, Cr8x8);

				dct(Y8x8[0][0], Y8x8[0][0]);	// 1 Y-transform
				dct(Y8x8[0][1], Y8x8[0][1]);	// 2 Y-transform
				dct(Y8x8[1][0], Y8x8[1][0]);	// 3 Y-transform
				dct(Y8x8[1][1], Y8x8[1][1]);	// 4 Y-transform
				dct(Cb8x8, Cb8x8);				// Cb-transform
				dct(Cr8x8, Cr8x8);				// Cr-transform

				huffman_encode(HUFFMAN_CTX_Y, (short*)Y8x8[0][0]);
				huffman_encode(HUFFMAN_CTX_Y, (short*)Y8x8[0][1]);
				huffman_encode(HUFFMAN_CTX_Y, (short*)Y8x8[1][0]);
				huffman_encode(HUFFMAN_CTX_Y, (short*)Y8x8[1][1]);
				huffman_encode(HUFFMAN_CTX_Cb, (short*)Cb8x8);
				huffman_encode(HUFFMAN_CTX_Cr, (short*)Cr8x8);
			}
		}
	}

	// Add JPEG footer to buffer
	huffman_stop();

	palSetPad(PORT(LED_YELLOW), PIN(LED_YELLOW));

	return true;
}

/**
  * ISR callback called when DCMI buffer moved into RAM by DMA completly
  * Buffer size: OV9655_BUFFER_SIZE = OV9655_MAXX * 16 * bytes
  * Double buffer mode is activated.
  */
void OV9655_dma_avail(uint32_t flags)
{
	(void)flags;
	ov9655_samplingFinished = true;
	dmaStreamDisable(STM32_DMA2_STREAM1);
}

/**
  * Initializes DMA
  */
void OV9655_InitDMA(void)
{
    const stm32_dma_stream_t *stream = STM32_DMA2_STREAM1;
    dmaStreamAllocate(stream, 10, (stm32_dmaisr_t)OV9655_dma_avail, NULL);
    dmaStreamSetPeripheral(stream, ((uint32_t*)OV9655_DCMI_REG_DR_ADDRESS));
    dmaStreamSetMemory0(stream, (uint32_t)ov9655_ram_buffer);
    dmaStreamSetTransactionSize(stream, OV9655_BUFFER_SIZE/sizeof(uint16_t));
    dmaStreamSetMode(stream, STM32_DMA_CR_CHSEL(1) | STM32_DMA_CR_DIR_P2M |
							STM32_DMA_CR_MINC | STM32_DMA_CR_PSIZE_WORD |
							STM32_DMA_CR_MSIZE_HWORD | STM32_DMA_CR_MBURST_SINGLE |
							STM32_DMA_CR_PBURST_SINGLE | STM32_DMA_CR_TCIE);
    dmaStreamSetFIFO(stream, STM32_DMA_FCR_FTH_FULL);
    dmaStreamEnable(stream);
}

void OV9655_DeinitDMA(void)
{
    const stm32_dma_stream_t *stream = STM32_DMA2_STREAM1;
    dmaStreamDisable(stream);
}

/**
  * Initializes DCMI
  */
void OV9655_InitDCMI(void)
{
	// Clock enable
	RCC->AHB2ENR |= RCC_AHB2Periph_DCMI;

	// Configure DCMI
	DCMI->CR &=	~((uint32_t)DCMI_CR_CM     | DCMI_CR_ESS   | DCMI_CR_PCKPOL |
				DCMI_CR_HSPOL  | DCMI_CR_VSPOL | DCMI_CR_FCRC_0 | 
				DCMI_CR_FCRC_1 | DCMI_CR_EDM_0 | DCMI_CR_EDM_1); 
	DCMI->CR =	DCMI_CaptureMode_SnapShot | DCMI_SynchroMode_Hardware | DCMI_PCKPolarity_Falling |
				DCMI_VSPolarity_High | DCMI_HSPolarity_High | DCMI_CaptureRate_All_Frame | DCMI_ExtendedDataMode_8b;

	// DCMI enable
	DCMI->CR |= (uint32_t)DCMI_CR_ENABLE;
	// Capture enable
	DCMI->CR |= (uint32_t)DCMI_CR_CAPTURE;
}

void OV9655_DeinitDCMI(void)
{
	// Clock enable
	RCC->AHB2ENR &= ~RCC_AHB2Periph_DCMI;
}

/**
  * Initializes GPIO (for DCMI)
  * The high speed clock supports communication by I2C (XCLK = 16MHz)
  */
void OV9655_InitGPIO(void)
{
	palSetPadMode(PORT(CAM_HREF), PIN(CAM_HREF), PAL_MODE_ALTERNATE(13));	// HSYNC -> PA4
	palSetPadMode(PORT(CAM_PCLK), PIN(CAM_PCLK), PAL_MODE_ALTERNATE(13));	// PCLK  -> PA6
	palSetPadMode(PORT(CAM_VSYNC), PIN(CAM_VSYNC), PAL_MODE_ALTERNATE(13));	// VSYNC -> PB7
	palSetPadMode(PORT(CAM_D2), PIN(CAM_D2), PAL_MODE_ALTERNATE(13));		// D0    -> PC6
	palSetPadMode(PORT(CAM_D3), PIN(CAM_D3), PAL_MODE_ALTERNATE(13));		// D1    -> PC7
	palSetPadMode(PORT(CAM_D4), PIN(CAM_D4), PAL_MODE_ALTERNATE(13));		// D2    -> PC8
	palSetPadMode(PORT(CAM_D5), PIN(CAM_D5), PAL_MODE_ALTERNATE(13));		// D3    -> PC9
	palSetPadMode(PORT(CAM_D6), PIN(CAM_D6), PAL_MODE_ALTERNATE(13));		// D4    -> PE4
	palSetPadMode(PORT(CAM_D7), PIN(CAM_D7), PAL_MODE_ALTERNATE(13));		// D5    -> PB6
	palSetPadMode(PORT(CAM_D8), PIN(CAM_D8), PAL_MODE_ALTERNATE(13));		// D6    -> PE5
	palSetPadMode(PORT(CAM_D9), PIN(CAM_D9), PAL_MODE_ALTERNATE(13));		// D7    -> PE6

	palSetPadMode(PORT(CAM_OFF), PIN(CAM_OFF), PAL_MODE_OUTPUT_PUSHPULL);	// CAM_OFF

	i2cCamInit();
}

/**
  * Setup a CLOCKOUT pin (PA8) which is needed by the camera (XCLK pin)
  */
void OV9655_InitClockout(void)
{
	palSetPadMode(PORT(CAM_XCLK), PIN(CAM_XCLK), PAL_MODE_ALTERNATE(0));	// PA8    -> XCLK
	RCC->CFGR = (RCC->CFGR & 0xF89FFFFF) | (0x0 << 24) | (0x0 << 21);		// 16MHz on PA8
}

uint32_t OV9655_getBuffer(uint8_t** buffer) {
	*buffer = jpeg;
	return jpeg_pos;
}

void OV9655_TransmitConfig(void) {
	for(uint32_t i=0; i<sizeof(OV9655_CONFIG); i+=2) {
		i2cCamSend(OV9655_I2C_ADR, &OV9655_CONFIG[i], 2, NULL, 0, MS2ST(100));
		chThdSleepMilliseconds(10);
	}
}

void OV9655_init(void) {
	TRACE_INFO("CAM  > Init pins");
	OV9655_InitClockout();
	OV9655_InitGPIO();

	// Power on OV9655
	TRACE_INFO("CAM  > Switch on");
	palClearPad(PORT(CAM_OFF), PIN(CAM_OFF));	// Switch on camera

	// Send settings to OV9655
	TRACE_INFO("CAM  > Transmit config to camera");
	OV9655_TransmitConfig();

	// DCMI DMA
	TRACE_INFO("CAM  > Init DMA");
	OV9655_InitDMA();

	// DCMI Init
	TRACE_INFO("CAM  > Init DCMI");
	OV9655_InitDCMI();
}

void OV9655_deinit(void) {
	// DCMI Init
	TRACE_INFO("CAM  > Deinit DCMI");
	OV9655_DeinitDCMI();

	// DCMI DMA
	TRACE_INFO("CAM  > Deinit DMA");
	OV9655_DeinitDMA();

	// Power off OV9655
	TRACE_INFO("CAM  > Switch off");
	palSetPad(PORT(CAM_OFF), PIN(CAM_OFF));	// Switch off camera
}

