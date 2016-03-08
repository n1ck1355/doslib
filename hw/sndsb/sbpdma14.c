
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <assert.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/dos/dos.h>
#include <hw/dos/dosbox.h>
#include <hw/8237/8237.h>		/* 8237 DMA */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/sndsb/sndsb.h>
#include <hw/dos/doswin.h>
#include <hw/dos/tgusmega.h>
#include <hw/dos/tgussbos.h>

/* Try to detect DMA channel, by playing silent audio blocks via DMA and
 * watching whether or not the DMA pointer moves. This method is most likely
 * to work on both Creative hardware and SB clones. */
static unsigned char try_dma[] = {1,3};
void sndsb_probe_dma8_14(struct sndsb_ctx *cx) {
	struct dma_8237_allocation *dma = NULL; /* DMA buffer */
	unsigned int len = 22050 / 25;
	uint8_t ch,dmap=0,iter=0;
	uint8_t old_mask;

	if (cx->dma8 >= 0) return;
	if (cx->sbos) return; // NTS: this test causes SBOS to hang (WHY???)
	if (!sndsb_reset_dsp(cx)) return;

	dma = dma_8237_alloc_buffer(len);
	if (dma == NULL) return;

#if TARGET_MSDOS == 32
	memset(dma->lin,128,len);
#else
	_fmemset(dma->lin,128,len);
#endif

	_cli();
	old_mask = inp(d8237_ioport(/*channel*/1,/*port*/0xF));
	if (old_mask == 0xff) old_mask = 0;

	while (dmap < sizeof(try_dma)) {
		ch = try_dma[dmap];

		/* any DMA? and did the byte returned match the patttern? */
		for (iter=0;iter < 10;iter++) {
			_cli();
			outp(d8237_ioport(ch,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(ch) | D8237_MASK_SET); /* mask */

			outp(d8237_ioport(ch,D8237_REG_W_WRITE_MODE),
					D8237_MODER_CHANNEL(ch) | D8237_MODER_TRANSFER(D8237_MODER_XFER_WRITE) | D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));

			inp(d8237_ioport(ch,D8237_REG_R_STATUS));
			d8237_write_count(ch,len);
			d8237_write_base(ch,dma->phys);
			outp(d8237_ioport(ch,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(ch)); /* unmask */
			_sti();

			/* use DSP command 0x14 */
			sndsb_interrupt_ack(cx,sndsb_interrupt_reason(cx));
			sndsb_write_dsp_timeconst(cx,0xD3); /* 22050Hz */
			sndsb_write_dsp(cx,0x14);
			sndsb_write_dsp(cx,(len - 1));
			sndsb_write_dsp(cx,(len - 1) >> 8);

			/* wait 100ms (period should be 50ms) */
			t8254_wait(t8254_us2ticks(100000));

			outp(d8237_ioport(ch,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(ch) | D8237_MASK_SET); /* mask */

			if ((d8237_read_count(ch)&0xFFFFUL) == 0UL) {
				/* it worked */
			}
			else {
				sndsb_reset_dsp(cx);
				break;
			}
		}

		if (iter == 10) {
			cx->dma8 = ch;
			if (cx->ess_chipset != SNDSB_ESS_NONE || cx->is_gallant_sc6600) cx->dma16 = cx->dma8;
			break;
		}

		dmap++;
	}

	outp(d8237_ioport(/*channel*/1,0xF),old_mask);
	_sti();

	dma_8237_free_buffer(dma);
}

