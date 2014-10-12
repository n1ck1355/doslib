
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/vga/vga.h>
#include <hw/dos/dos.h>
#include <hw/8237/8237.h>		/* DMA controller */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/vga/vgagui.h>
#include <hw/vga/vgatty.h>

#define MAX_FLOPPY_CONTROLLER 4

struct floppy_controller {
	uint16_t		base_io;
	int8_t			irq;
	int8_t			dma;

	/* known state */
	uint8_t			ps2_status[2];		/* 0x3F0-0x3F1 */
	uint8_t			st[4];			/* ST0...ST3 */
							/*  Status Register ST0 */
							/*     [7:6] IC (Interrupt code) */
							/*            00 = normal termination, no errors */
							/*            01 = abnormal termination */
							/*            10 = invalid command */
							/*            11 = abnormal termination by polling */
							/*     [5:5] Seek end */
							/*     [4:4] Unit check */
							/*     [3:3] Drive not ready */
							/*     [2:2] Head currently active */
							/*     [1:0] Currently selected drive */
							/*  Status Register ST1 */
							/*     [7:7] End of Cylinder */
							/*     [6:6] always 0 */
							/*     [5:5] Data error */
							/*     [4:4] Time out (data override) */
							/*     [3:3] always 0 */
							/*     [2:2] No data */
							/*     [1:1] Not writeable */
							/*     [0:0] No address mark */
							/*  Status Register ST2 */
							/*     [7:7] always 0 */
							/*     [6:6] Deleted Address Mark */
							/*     [5:5] CRC error in data field */
							/*     [4:4] Wrong Cylinder */
							/*     [3:3] Seek equal */
							/*     [2:2] Seek error */
							/*     [1:1] Bad cylinder */
							/*     [0:0] Not data address mark DAM */
							/*  Status Register ST3 */
							/*     [7:7] Error, drive error signal is active */
							/*     [6:6] Write protection */
							/*     [5:5] Ready, drive ready signal is active */
							/*     [4:4] Track 0 */
							/*     [3:3] Double sided drive */
							/*     [2:2] HDSEL (head select) signal from drive */
							/*     [1:0] Drive select */
	uint8_t			digital_out;		/* last value written to 0x3F2 */
							/*     [7:7] MOTD (Motor control, drive D) */
							/*     [6:6] MOTC (Motor control, drive C) */
							/*     [5:5] MOTB (Motor control, drive B) */
							/*     [4:4] MOTA (Motor control, drive A) */
							/*     [3:3] DMA (DMA+IRQ enable) */
							/*     [2:2] !REST (Controller reset) 1=enable 0=reset */
							/*     [1:0] Drive select (0=A 1=B 2=C 3=D) */
	uint8_t			main_status;		/* last value read from 0x3F4 */
							/*     [7:7] MRQ (Main request) 1=data register ready */
							/*     [6:6] DIO (Data input/output) 1=expecting CPU read 0=expecting CPU write */
							/*     [5:5] NDMA (non-DMA mode) 1=not in DMA mode */
							/*     [4:4] BUSY (instruction, device busy) 1=active */
							/*     [3:3] ACTD Drive D in positioning mode */
							/*     [2:2] ACTC Drive C in positioning mode */
							/*     [1:1] ACTB Drive B in positioning mode */
							/*     [0:0] ACTA Drive A in positioning mode */
	uint8_t			digital_in;		/* last value read from 0x3F7 */
							/*     [7:7] CHAN (Disk Change) 1=disk changed since last command  (PS/2 model 30 bit is inverted) */
							/* * PS/2 except Model 30 */
							/*     [6:3] all 1's */
							/* * PS/2 Model 30 */
							/*     [6:4] all 0's */
	uint8_t			control_cfg;		/* last value written to 0x3F7 */
							/*     [1:0] Data transfer rate */
							/*            11 = 1 mbit/sec */
							/*            10 = 250 kbit/sec */
							/*            01 = 300 kbit/sec */
							/*            00 = 500 kbit/sec */
	uint8_t			cylinder;		/* last known cylinder position */
	uint16_t		irq_fired;

	/* desired state */
	uint8_t			use_dma:1;		/* if set, use DMA. else, use PIO data transfer */

	/* what we know about the controller */
	uint8_t			version;		/* result of command 0x10 (determine controller version) or 0x00 if not yet queried */
	uint8_t			ps2_mode:1;		/* controller is in PS/2 mode (has status registers A and B I/O ports) */
	uint8_t			at_mode:1;		/* controller is in AT mode (has Digital Input Register and Control Config Reg) */
	uint8_t			digital_out_rw:1;	/* digital out (0x3F2) is readable */
};

/* standard I/O ports for floppy controllers */
struct floppy_controller		floppy_standard_isa[2] = {
	/*base, IRQ, DMA*/
	{0x3F0, 6,   2},
	{0x370, 6,   2}
};

struct floppy_controller *floppy_get_standard_isa_port(int x) {
	if (x < 0 || x >= (sizeof(floppy_standard_isa)/sizeof(floppy_standard_isa[0]))) return NULL;
	return &floppy_standard_isa[x];
}

struct dma_8237_allocation*		floppy_dma = NULL; /* DMA buffer */

struct floppy_controller		floppy_controllers[MAX_FLOPPY_CONTROLLER];
int8_t					floppy_controllers_init = -1;

struct floppy_controller *floppy_get_controller(int x) {
	if (x < 0 || x >= MAX_FLOPPY_CONTROLLER) return NULL;
	if (floppy_controllers[x].base_io == 0) return NULL;
	return &floppy_controllers[x];
}

int wait_for_enter_or_escape() {
	int c;

	do {
		c = getch();
		if (c == 0) c = getch() << 8;
	} while (!(c == 13 || c == 27));

	return c;
}

struct floppy_controller *alloc_floppy_controller() {
	unsigned int i=0;

	while (i < MAX_FLOPPY_CONTROLLER) {
		if (floppy_controllers[i].base_io == 0)
			return &floppy_controllers[i];
	}

	return NULL;
}

void floppy_controller_read_ps2_status(struct floppy_controller *i) {
	if (i->ps2_mode) {
		i->ps2_status[0] = inp(i->base_io+0);
		i->ps2_status[1] = inp(i->base_io+1);
	}
	else {
		i->ps2_status[0] = i->ps2_status[1] = 0xFF;
	}
}

static inline uint8_t floppy_controller_read_status(struct floppy_controller *i) {
	i->main_status = inp(i->base_io+4); /* 0x3F4 main status */
	return i->main_status;
}

static inline uint8_t floppy_controller_read_DIR(struct floppy_controller *i) {
	if (i->at_mode || i->ps2_mode)
		i->digital_in = inp(i->base_io+7); /* 0x3F7 */
	else
		i->digital_in = 0xFF;

	return i->digital_in;
}

static inline void floppy_controller_write_DOR(struct floppy_controller *i,unsigned char c) {
	i->digital_out = c;
	outp(i->base_io+2,c);	/* 0x3F2 Digital Output Register */
}

static inline void floppy_controller_write_CCR(struct floppy_controller *i,unsigned char c) {
	i->control_cfg = c;
	outp(i->base_io+7,c);	/* 0x3F7 Control Configuration Register */
}

void floppy_controller_set_data_transfer_rate(struct floppy_controller *i,unsigned char rsel) {
	if (rsel > 3) return;
	floppy_controller_write_CCR(i,(i->control_cfg & ~3) + rsel); /* change bits [1:0] */
}

void floppy_controller_drive_select(struct floppy_controller *i,unsigned char drv) {
	if (drv > 3) return;

	i->digital_out &= ~0x03;
	i->digital_out |= drv;
	outp(i->base_io+2,i->digital_out);	/* 0x3F2 Digital Output Register */
}

void floppy_controller_set_motor_state(struct floppy_controller *i,unsigned char drv,unsigned char set) {
	if (drv > 3) return;

	i->digital_out &= ~(0x10 << drv);
	i->digital_out |= (set?(0x10 << drv):0x00);
	outp(i->base_io+2,i->digital_out);	/* 0x3F2 Digital Output Register */
}

void floppy_controller_enable_dma(struct floppy_controller *i,unsigned char set) {
	if (i->dma < 0 || i->irq < 0) set = 0;

	i->use_dma = !!set;
	i->digital_out &= ~0x08;
	i->digital_out |= (set?0x08:0x00);
	outp(i->base_io+2,i->digital_out);	/* 0x3F2 Digital Output Register */
}

void floppy_controller_enable_dma_otr(struct floppy_controller *i,unsigned char set) {
	unsigned char c;

	c = i->digital_out;
	c &= ~0x08;
	c |= (set?0x08:0x00);
	outp(i->base_io+2,c);			/* 0x3F2 Digital Output Register */
}

void floppy_controller_set_reset(struct floppy_controller *i,unsigned char set) {
	i->digital_out &= ~0x04;
	i->digital_out |= (set?0x00:0x04);	/* bit is INVERTED (0=reset 1=normal) */
	outp(i->base_io+2,i->digital_out);	/* 0x3F2 Digital Output Register */
}

struct floppy_controller *floppy_controller_probe(struct floppy_controller *i) {
	struct floppy_controller *ret = NULL;
	uint8_t t1;

	if (i == NULL) return NULL;
	if (i->base_io == 0) return NULL;

	/* is anything there? the best we can hope for is to probe the I/O port range and see if SOMETHING responds */
	t1 = inp(i->base_io+4); /* main status register (we can't assume the ability to read the digital output register, the data reg might be 0xFF) */
	if (t1 == 0xFF) return NULL;

	ret = alloc_floppy_controller();
	if (ret == NULL) return NULL;
	memset(ret,0,sizeof(*ret));

	ret->base_io = i->base_io;
	if (i->irq >= 2 && i->irq <= 15)
		ret->irq = i->irq;
	else
		ret->irq = -1;

	if (i->dma >= 0 && i->dma <= 7)
		ret->dma = i->dma;
	else
		ret->dma = -1;

	ret->use_dma = (ret->dma >= 0 && ret->irq >= 0); /* FIXME: I'm sure there's a way to do DMA without IRQ */

	/* if something appears at 0x3F0-0x3F1, assume "PS/2 mode".
	 * there are finer intricate details where the meaning of some bits
	 * completely change or invert their meaning between PS/2 and Model 30,
	 * so we don't concern ourself with them, we only care that there's
	 * something there and we can let the program using this lib figure it out. */
	t1 = inp(ret->base_io+0);
	t1 &= inp(ret->base_io+1);
	if (t1 != 0xFF) ret->ps2_mode = 1;

	/* what about the AT & PS/2 style CCR & DIR */
	t1 = inp(ret->base_io+7);
	if (t1 != 0xFF) ret->at_mode = 1;

	/* and ... guess */
	floppy_controller_write_DOR(ret,0x04+(ret->use_dma?0x08:0x00));	/* most BIOSes: DMA/IRQ enable, !reset, motor off, drive A select */
	floppy_controller_read_status(ret);

	/* is the Digital Out port readable? */
	t1 = inp(ret->base_io+2);
	if (t1 == ret->digital_out) ret->digital_out_rw = 1;

	floppy_controller_read_DIR(ret);

	return ret;
}

int init_floppy_controller_lib() {
	if (floppy_controllers_init < 0) {
		memset(floppy_controllers,0,sizeof(floppy_controllers));
		floppy_controllers_init = 0;

		cpu_probe();
		probe_dos();
		detect_windows();

		/* do NOT under any circumstances talk directly to the floppy from under Windows! */
		if (windows_mode != WINDOWS_NONE) return (floppy_controllers_init=0);

		/* init OK */
		floppy_controllers_init = 1;
	}

	return floppy_controllers_init;
}

void free_floppy_controller_lib() {
}

/*------------------------------------------------------------------------*/

char	tmp[1024];

/* "current position" */
static int current_sect = 1;
static int current_head = 0;
static int current_track = 0;
static int current_sectsize_p2 = 2;		/* NTS: 128 << N, 128 << 2 = 512 */
static int current_sectcount = 2;		/* two sectors */
static unsigned char allow_multitrack = 1;	/* set MT bit */
static unsigned char mfm_mode = 1;		/* set bit to indicate MFM (not FM) */

static void (interrupt *my_floppy_old_irq)() = NULL;
static struct floppy_controller *my_floppy_irq_floppy = NULL;
static unsigned long floppy_irq_counter = 0;
static int my_floppy_irq_number = -1;

void do_floppy_controller_unhook_irq(struct floppy_controller *fdc);

static void interrupt my_floppy_irq() {
	int i;

	i = vga_width*(vga_height-1);

	_cli();
	if (my_floppy_irq_floppy != NULL) {
		my_floppy_irq_floppy->irq_fired++;

		/* ack PIC */
		if (my_floppy_irq_floppy->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);

		/* If too many IRQs fired, then unhook the IRQ and use polling from now on. */
		if (my_floppy_irq_floppy->irq_fired >= 0xFFFEU) {
			do_floppy_controller_unhook_irq(my_floppy_irq_floppy);
			vga_alpha_ram[i+12] = 0x1C00 | '!';
			my_floppy_irq_floppy->irq_fired = ~0; /* make sure the IRQ counter is as large as possible */
		}
	}

	/* we CANNOT use sprintf() here. sprintf() doesn't work to well from within an interrupt handler,
	 * and can cause crashes in 16-bit realmode builds. */
	vga_alpha_ram[i++] = 0x1F00 | 'I';
	vga_alpha_ram[i++] = 0x1F00 | 'R';
	vga_alpha_ram[i++] = 0x1F00 | 'Q';
	vga_alpha_ram[i++] = 0x1F00 | ':';
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter / 100000UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /  10000UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /   1000UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /    100UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /     10UL) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ('0' + ((floppy_irq_counter /       1L) % 10UL));
	vga_alpha_ram[i++] = 0x1F00 | ' ';

	floppy_irq_counter++;
}

void do_floppy_controller_hook_irq(struct floppy_controller *fdc) {
	if (my_floppy_irq_number >= 0 || my_floppy_old_irq != NULL || fdc->irq < 0)
		return;

	/* let the IRQ know what floppy controller */
	my_floppy_irq_floppy = fdc;

	/* enable on floppy controller */
	p8259_mask(fdc->irq);
	floppy_controller_enable_dma(fdc,1);

	/* hook IRQ */
	my_floppy_old_irq = _dos_getvect(irq2int(fdc->irq));
	_dos_setvect(irq2int(fdc->irq),my_floppy_irq);
	my_floppy_irq_number = fdc->irq;

	/* enable at PIC */
	p8259_unmask(fdc->irq);
}

void do_floppy_controller_unhook_irq(struct floppy_controller *fdc) {
	if (my_floppy_irq_number < 0 || my_floppy_old_irq == NULL || fdc->irq < 0)
		return;

	/* disable on floppy controller, then mask at PIC */
	p8259_mask(fdc->irq);
	floppy_controller_enable_dma(fdc,0);

	/* restore the original vector */
	_dos_setvect(irq2int(fdc->irq),my_floppy_old_irq);
	my_floppy_irq_number = -1;
	my_floppy_old_irq = NULL;
}

void do_floppy_controller_enable_irq(struct floppy_controller *fdc,unsigned char en) {
	if (!en || fdc->irq < 0 || fdc->irq != my_floppy_irq_number)
		do_floppy_controller_unhook_irq(fdc);
	if (en && fdc->irq >= 0)
		do_floppy_controller_hook_irq(fdc);
}

static inline uint8_t floppy_controller_busy_in_instruction(struct floppy_controller *fdc) {
	return ((fdc->main_status & 0x10) == 0x10)?1:0; /* busy == 1 */
}

static inline uint8_t floppy_controller_data_io_ready(struct floppy_controller *fdc) {
	return ((fdc->main_status & 0x80) == 0x80)?1:0; /* data ready == 1 */
}

static inline uint8_t floppy_controller_can_read_data(struct floppy_controller *fdc) {
	return ((fdc->main_status & 0xC0) == 0xC0)?1:0; /* data ready == 1 and data i/o == 1 */
}

static inline uint8_t floppy_controller_can_read_data_non_dma(struct floppy_controller *fdc) {
	return ((fdc->main_status & 0xE0) == 0xE0)?1:0; /* data ready == 1 and data i/o == 1 and NDMA == 1 */
}

static inline uint8_t floppy_controller_can_write_data(struct floppy_controller *fdc) {
	return ((fdc->main_status & 0xC0) == 0x80)?1:0; /* data ready == 1 and data i/o == 0 */
}

static inline uint8_t floppy_controller_can_write_data_non_dma(struct floppy_controller *fdc) {
	return ((fdc->main_status & 0xE0) == 0xA0)?1:0; /* data ready == 1 and data i/o == 0 AND NDMA == 1 */
}

static inline uint8_t floppy_controller_read_data_byte(struct floppy_controller *fdc) {
	return inp(fdc->base_io+5);
}

static inline void floppy_controller_write_data_byte(struct floppy_controller *fdc,uint8_t b) {
	outp(fdc->base_io+5,b);
}

static inline int floppy_controller_wait_data_ready(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (floppy_controller_data_io_ready(fdc)) return 1;
	} while (--timeout != 0);

	return 0;
}

static inline int floppy_controller_wait_data_read_non_dma_ready(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (floppy_controller_can_read_data_non_dma(fdc)) return 1;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

static inline int floppy_controller_wait_data_write_non_dma_ready(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (floppy_controller_can_write_data_non_dma(fdc)) return 1;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

static inline void floppy_controller_reset_irq_counter(struct floppy_controller *fdc) {
	fdc->irq_fired = 0;
}

int floppy_controller_wait_busy_in_instruction(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (!floppy_controller_busy_in_instruction(fdc)) return 1;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

int floppy_controller_wait_data_ready_ms(struct floppy_controller *fdc,unsigned int timeout) {
	do {
		floppy_controller_read_status(fdc);
		if (floppy_controller_data_io_ready(fdc)) return 1;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

int floppy_controller_wait_irq(struct floppy_controller *fdc,unsigned int timeout,unsigned int counter) {
	do {
		if (fdc->irq_fired >= counter) break;
		t8254_wait(t8254_us2ticks(1000));
	} while (--timeout != 0);

	return 0;
}

int floppy_controller_write_data(struct floppy_controller *fdc,const unsigned char *data,int len) {
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli(); /* clear interrupts so we can focus on talking to the FDC */
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_can_write_data(fdc)) {
			if (ret == 0) ret = -2;
			break;
		}

		floppy_controller_write_data_byte(fdc,*data++);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

#if TARGET_MSDOS == 32
int floppy_controller_write_data_ndma(struct floppy_controller *fdc,const unsigned char *data,int len) {
#else
int floppy_controller_write_data_ndma(struct floppy_controller *fdc,const unsigned char far *data,int len) {
#endif
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli();
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_wait_data_write_non_dma_ready(fdc,1000)) {
			if (ret == 0) ret = -2;
			break;
		}

		floppy_controller_write_data_byte(fdc,*data++);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

int floppy_controller_read_data(struct floppy_controller *fdc,unsigned char *data,int len) {
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli();
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_can_read_data(fdc)) {
			if (ret == 0) ret = -2;
			break;
		}

		*data++ = floppy_controller_read_data_byte(fdc);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

#if TARGET_MSDOS == 32
int floppy_controller_read_data_ndma(struct floppy_controller *fdc,unsigned char *data,int len) {
#else
int floppy_controller_read_data_ndma(struct floppy_controller *fdc,unsigned char far *data,int len) {
#endif
	unsigned int oflags = get_cpu_flags();
	int ret = 0;

	_cli();
	while (len > 0) {
		if (!floppy_controller_wait_data_ready(fdc,1000)) {
			if (ret == 0) ret = -1;
			break;
		}
		if (!floppy_controller_wait_data_read_non_dma_ready(fdc,1000)) {
			if (ret == 0) ret = -2;
			break;
		}

		*data++ = floppy_controller_read_data_byte(fdc);
		len--; ret++;
	}

	if (oflags&0x200/*IF interrupt enable was on*/) _sti();
	return ret;
}

void do_floppy_controller_reset(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;

	vga_msg_box_create(&vgabox,"FDC reset in progress",0,0);

	floppy_controller_set_reset(fdc,1);
	t8254_wait(t8254_us2ticks(1000000));
	floppy_controller_set_reset(fdc,0);
	floppy_controller_wait_data_ready_ms(fdc,1000);
	floppy_controller_read_status(fdc);

	vga_msg_box_destroy(&vgabox);
}

void do_check_interrupt_status(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;
	char cmd[10],resp[10];
	int rd,wd,rdo,wdo;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Check Interrupt Status (x8h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   1   0   0   0
	 *
	 */

	wdo = 1;
	cmd[0] = 0x08;	/* Check interrupt status */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 1) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* wait for data ready. does not fire an IRQ (because you use this to clear IRQ status!) */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* NTS: It's not specified whether this returns 2 bytes if success and 1 if no IRQ pending.. or...? */
	rdo = 2;
	resp[1] = 0;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		sprintf(tmp,"Failed to read data from the FDC, %u/%u",rd,rdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Check Interrupt Status (x8h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |        Current Cylinder
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* return value is ST0 and the current cylinder */
	fdc->st[0] = resp[0];
	fdc->cylinder = resp[1];
}

void do_spin_up_motor(struct floppy_controller *fdc,unsigned char drv) {
	if (drv > 3) return;

	if (!(fdc->digital_out & (0x10 << drv))) {
		struct vga_msg_box vgabox;

		vga_msg_box_create(&vgabox,"Spinning up motor",0,0);

		/* if the motor isn't on, then turn it on, and then wait for motor to stabilize */
		floppy_controller_set_motor_state(fdc,drv,1);
		t8254_wait(t8254_us2ticks(500000)); /* 500ms */

		vga_msg_box_destroy(&vgabox);
	}
}

void do_seek_drive_rel(struct floppy_controller *fdc,int track) {
	struct vga_msg_box vgabox;
	char cmd[10];
	int wd,wdo;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	floppy_controller_reset_irq_counter(fdc);

	/* Seek Relative (1xFh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  1 DIR   0   0   1   1   1   1
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |         Cylinder Step
	 *
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select
	 *        DIR = Step direction (1=inward increasing, 0=outward decreasing)
	 *  Cyl. step = How many tracks to step */

	wdo = 3;
	cmd[0] = 0x8F + (track < 0 ? 0x40 : 0x00); /* Seek rel [6:6] = DIR */
	cmd[1] = (fdc->digital_out&3)+(current_head?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] HD (doesn't matter) */;
	cmd[2] = abs(track);
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 3) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* fires an IRQ. doesn't return state */
	if (fdc->use_dma) floppy_controller_wait_irq(fdc,1000,1);
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Seek Relative (1xFh) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);
}

void do_seek_drive(struct floppy_controller *fdc,uint8_t track) {
	struct vga_msg_box vgabox;
	char cmd[10];
	int wd,wdo;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	floppy_controller_reset_irq_counter(fdc);

	/* Seek (xFh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   1   1   1   1
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |            Cylinder
	 *
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select
	 *   Cylinder = Track to move to */

	wdo = 3;
	cmd[0] = 0x0F;	/* Seek */
	cmd[1] = (fdc->digital_out&3)+(current_head?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] HD (doesn't matter) */;
	cmd[2] = track;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 3) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* fires an IRQ. doesn't return state */
	if (fdc->use_dma) floppy_controller_wait_irq(fdc,1000,1);
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Seek (xFh) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);
}

void do_check_drive_status(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;
	char cmd[10],resp[10];
	int rd,wd,rdo,wdo;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Check Drive Status (x4h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   0   1   0   0
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *
	 *       HD = Head select (on PC platform, doesn't matter)
	 *  DR1,DR0 = Drive select */

	wdo = 2;
	cmd[0] = 0x04;	/* Check drive status */
	cmd[1] = (fdc->digital_out&3)+(current_head?0x04:0x00)/* [1:0] = DR1,DR0 [2:2] = HD = 0 */;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Check Drive Status (x4h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST3
         */

	rdo = 1;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		sprintf(tmp,"Failed to read data from the FDC, %u/%u",rd,rdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* return value is ST3 */
	fdc->st[3] = resp[0];
}

void do_calibrate_drive(struct floppy_controller *fdc) {
	struct vga_msg_box vgabox;
	char cmd[10];
	int wd,wdo;

	do_spin_up_motor(fdc,fdc->digital_out&3);

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	floppy_controller_reset_irq_counter(fdc);

	/* Calibrate Drive (x7h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0   0   0   0   0   1   1   1
	 *      1 |  x   x   x   x   x   0 DR1 DR0
	 *
	 *  DR1,DR0 = Drive select */

	wdo = 2;
	cmd[0] = 0x07;	/* Calibrate */
	cmd[1] = (fdc->digital_out&3)/* [1:0] = DR1,DR0 */;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		sprintf(tmp,"Failed to write data to FDC, %u/%u",wd,wdo);
		vga_msg_box_create(&vgabox,tmp,0,0);
		wait_for_enter_or_escape();
		vga_msg_box_destroy(&vgabox);
		do_floppy_controller_reset(fdc);
		return;
	}

	/* fires an IRQ. doesn't return state */
	if (fdc->use_dma) floppy_controller_wait_irq(fdc,1000,1);
	floppy_controller_wait_data_ready_ms(fdc,1000);

	/* Calibrate Drive (x7h) response
	 *
	 * (none)
	 */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* use Check Interrupt Status */
	do_check_interrupt_status(fdc);
}

int do_read_sector_id(unsigned char resp[7],struct floppy_controller *fdc,unsigned char head) {
	int rd,wd,rdo,wdo;
	char cmd[10];

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	/* Read ID (xAh)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |  0  MF   0   0   1   0   1   0
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *
	 *         MF = MFM/FM
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	wdo = 2;
	cmd[0] = 0x0A + (mfm_mode?0x40:0x00); /* Read sector ID [6:6] MFM */
	cmd[1] = (fdc->digital_out&3)+(head<<2)/* [1:0] = DR1,DR0 [2:2] = HD */;
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* wait for data ready. does not fire an IRQ */
	floppy_controller_wait_data_ready_ms(fdc,1000);

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		do_floppy_controller_reset(fdc);
		return 0;
	}

	/* Read ID (xAh) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |           Cylinder
	 *      4 |             Head
	 *      5 |        Sector Number
	 *      6 |         Sector Size
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	return rd;
}

void do_read_sector_id_demo(struct floppy_controller *fdc) {
	unsigned char headsel = current_head;
	char resp[10];
	int c;

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

	vga_write("Read Sector ID demo. Space to switch heads.\n\n");

	do {
		if (kbhit()) {
			c = getch();
			if (c == 0) c = getch() << 8;

			if (c == 27)
				break;
			else if (c == ' ')
				headsel ^= 1;
		}

		if ((c=do_read_sector_id(resp,fdc,headsel)) == 7) {
			vga_moveto(0,2);
			vga_write_color(0x0F);

			sprintf(tmp,"ST: %02xh %02xh %02xh %02xh\n",fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3]);
			vga_write(tmp);

			sprintf(tmp,"C/H/S/sz: %-3u/%-3u/%-3u/%-3u       \n",resp[3],resp[4],resp[5],resp[6]);
			vga_write(tmp);
		}
		else {
			vga_moveto(0,2);
			vga_write_color(0x0F);

			sprintf(tmp,"ST: --h --h --h --h (ret=%d)\n",c);
			vga_write(tmp);

			sprintf(tmp,"C/H/S/sz: ---/---/---/---       \n");
			vga_write(tmp);
		}
	} while (1);
}

signed long prompt_signed_track_number() {
	signed long track = 0;
	struct vga_msg_box box;
	VGA_ALPHA_PTR sco;
	char temp_str[16];
	int c,i=0;

	vga_msg_box_create(&box,"Enter relative track number:",2,0);
	sco = vga_alpha_ram + ((box.y+2) * vga_width) + box.x + 2;
	while (1) {
		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			track = 0;
			break;
		}
		else if (c == 13) {
			if (i == 0) break;
			temp_str[i] = 0;
			if (isdigit(temp_str[0]))
				track = strtol(temp_str,NULL,0);
			else
				track = 0;

			break;
		}
		else if (isdigit(c) || c == '-') {
			if (i < 15) {
				sco[i] = c | 0x1E00;
				temp_str[i++] = c;
			}
		}
		else if (c == 8) {
			if (i > 0) i--;
			sco[i] = ' ' | 0x1E00;
		}
	}
	vga_msg_box_destroy(&box);

	return track;
}

unsigned long prompt_track_number() {
	unsigned long track = 0;
	struct vga_msg_box box;
	VGA_ALPHA_PTR sco;
	char temp_str[16];
	int c,i=0;

	vga_msg_box_create(&box,"Enter track number:",2,0);
	sco = vga_alpha_ram + ((box.y+2) * vga_width) + box.x + 2;
	while (1) {
		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			track = (int)(~0UL);
			break;
		}
		else if (c == 13) {
			if (i == 0) break;
			temp_str[i] = 0;
			if (isdigit(temp_str[0]))
				track = strtol(temp_str,NULL,0);
			else
				track = 0;

			break;
		}
		else if (isdigit(c)) {
			if (i < 15) {
				sco[i] = c | 0x1E00;
				temp_str[i++] = c;
			}
		}
		else if (c == 8) {
			if (i > 0) i--;
			sco[i] = ' ' | 0x1E00;
		}
	}
	vga_msg_box_destroy(&box);

	return track;
}

void do_floppy_write_test(struct floppy_controller *fdc) {
	unsigned int data_length;
	unsigned int returned_length = 0;
	char cmd[10],resp[10];
	uint8_t cyl,head,sect,ssz,nsect;
	int rd,rdo,wd,wdo,x,p;

	cyl = current_track;
	head = current_head&1;
	sect = current_sect;
	nsect = current_sectcount;
	ssz = current_sectsize_p2;

	data_length = nsect * (128 << ssz);

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

	sprintf(tmp,"Writing C/H/S/sz/num %u/%u/%u/%u/%u\n",cyl,head,sect,128 << ssz,nsect);
	vga_write(tmp);

	do_spin_up_motor(fdc,fdc->digital_out&3);

	if (floppy_dma == NULL) return;
	if (data_length > floppy_dma->length) return;

	for (x=0;(unsigned int)x < data_length;x++) floppy_dma->lin[x] = x;

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	if (fdc->dma >= 0 && fdc->use_dma) {
		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma) | D8237_MASK_SET); /* mask */

		outp(d8237_ioport(fdc->dma,D8237_REG_W_WRITE_MODE),
			D8237_MODER_CHANNEL(fdc->dma) |
			D8237_MODER_TRANSFER(D8237_MODER_XFER_READ) |
			D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));

		d8237_write_count(fdc->dma,data_length);
		d8237_write_base(fdc->dma,floppy_dma->phys);

		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma)); /* unmask */

		inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS)); /* read status port to clear TC bits */
	}

	/* Write Sector (x5h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 | MT  MF   0   0   0   1   0   1
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |            Cylinder
	 *      3 |              Head
	 *      4 |         Sector Number
	 *      5 |          Sector Size
	 *      6 |    Track length/last sector
	 *      7 |        Length of GAP3
	 *      8 |          Data Length
	 *
	 *         MT = Multi-track
	 *         MF = MFM/FM
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	/* NTS: To ensure we read only one sector, Track length/last sector must equal
	 * Sector Number field. The floppy controller stops reading on error or when
	 * sector number matches. This is very important for the PIO mode of this code,
	 * else we'll never complete reading properly and never get to reading back
	 * the status bytes. */

	wdo = 9;
	cmd[0] = (allow_multitrack?0x80:0x00)/*Multitrack*/ + (mfm_mode?0x40:0x00)/* MFM */ + 0x05/* WRITE DATA */;
	cmd[1] = (fdc->digital_out&3)/* [1:0] = DR1,DR0 [2:2] = HD */;
	cmd[2] = cyl;	/* cyl=0 */
	cmd[3] = head;	/* head=0 */
	cmd[4] = sect;	/* sector=1 */
	cmd[5] = ssz;	/* sector size=2 (512 bytes) */
	cmd[6] = sect+nsect-1; /* last sector of the track (what sector to stop at). */
	cmd[7] = 0;	/* gap size (??) */
	cmd[8] = 0xFF;	/* DTL (not used) */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	vga_write("Write in progress\n");

	/* fires an IRQ */
	if (fdc->use_dma) {
		floppy_controller_wait_irq(fdc,10000,1); /* 10 seconds */
		floppy_controller_wait_data_ready_ms(fdc,1000);
	}
	else {
		/* NOTES:
		 *
		 * Sun/Oracle VirtualBox floppy emulation bug: PIO mode doesn't seem to be supported properly on write
		 * for more than one sector. It will SEEM like it works, but when you read the data back only the first
		 * sector was ever written. On the same configuration, READ works fine using the same PIO mode. */
		while (returned_length < data_length) {
			if ((returned_length+(128 << ssz)) > data_length) break;
			floppy_controller_wait_data_ready_ms(fdc,10000);

			p = floppy_controller_write_data_ndma(fdc,floppy_dma->lin + returned_length,128 << ssz);
			if (p < 0 || p > (128 << ssz)) {
				sprintf(tmp,"NDMA write failed %d (%02xh)\n",p,fdc->main_status);
				vga_write(tmp);
				p = 0;
			}

			returned_length += p;
			/* stop on incomplete transfer */
			if (p != (128 << ssz)) break;
		}
	}

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Write Sector (x5h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |           Cylinder
	 *      4 |             Head
	 *      5 |        Sector Number
	 *      6 |         Sector Size
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	/* (if DMA) did we get any data? */
	if (fdc->dma >= 0 && fdc->use_dma) {
		uint32_t dma_len = d8237_read_count(fdc->dma);
		uint8_t status = inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS));

		/* some DMA controllers reset the count back to the original value on terminal count.
		 * so if the DMA controller says terminal count, the true count is zero */
		if (status&D8237_STATUS_TC(fdc->dma)) dma_len = 0;

		if (dma_len > (uint32_t)data_length) dma_len = (uint32_t)data_length;
		returned_length = (unsigned int)((uint32_t)data_length - dma_len);
	}

	sprintf(tmp,"%lu bytes written\n",(unsigned long)returned_length);
	vga_write(tmp);
	vga_write("\n");

	wait_for_enter_or_escape();
}

void do_floppy_read_test(struct floppy_controller *fdc) {
	unsigned int data_length;
	unsigned int returned_length = 0;
	char cmd[10],resp[10];
	uint8_t cyl,head,sect,ssz,nsect;
	int rd,rdo,wd,wdo,x,y,p;

	cyl = current_track;
	head = current_head&1;
	sect = current_sect;
	nsect = current_sectcount;
	ssz = current_sectsize_p2;

	data_length = nsect * (128 << ssz);

	vga_moveto(0,0);
	vga_write_color(0x0E);
	vga_clear();

	sprintf(tmp,"Reading C/H/S/sz/num %u/%u/%u/%u/%u\n",cyl,head,sect,128 << ssz,nsect);
	vga_write(tmp);

	do_spin_up_motor(fdc,fdc->digital_out&3);

	if (floppy_dma == NULL) return;
	if (data_length > floppy_dma->length) return;

#if TARGET_MSDOS == 32
	memset(floppy_dma->lin,0,data_length);
#else
	_fmemset(floppy_dma->lin,0,data_length);
#endif

	floppy_controller_read_status(fdc);
	if (!floppy_controller_can_write_data(fdc) || floppy_controller_busy_in_instruction(fdc))
		do_floppy_controller_reset(fdc);

	if (fdc->dma >= 0 && fdc->use_dma) {
		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma) | D8237_MASK_SET); /* mask */

		outp(d8237_ioport(fdc->dma,D8237_REG_W_WRITE_MODE),
			D8237_MODER_CHANNEL(fdc->dma) |
			D8237_MODER_TRANSFER(D8237_MODER_XFER_WRITE) |
			D8237_MODER_MODESEL(D8237_MODER_MODESEL_SINGLE));

		d8237_write_count(fdc->dma,data_length);
		d8237_write_base(fdc->dma,floppy_dma->phys);

		outp(d8237_ioport(fdc->dma,D8237_REG_W_SINGLE_MASK),D8237_MASK_CHANNEL(fdc->dma)); /* unmask */

		inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS)); /* read status port to clear TC bits */
	}

	/* Read Sector (x6h)
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 | MT  MF  SK   0   0   1   1   0
	 *      1 |  x   x   x   x   x  HD DR1 DR0
	 *      2 |            Cylinder
	 *      3 |              Head
	 *      4 |         Sector Number
	 *      5 |          Sector Size
	 *      6 |    Track length/last sector
	 *      7 |        Length of GAP3
	 *      8 |          Data Length
	 *
	 *         MT = Multi-track
	 *         MF = MFM/FM
	 *         SK = Skip deleted address mark
	 *         HD = Head select (on PC platform, doesn't matter)
	 *    DR1,DR0 = Drive select */

	/* NTS: To ensure we read only one sector, Track length/last sector must equal
	 * Sector Number field. The floppy controller stops reading on error or when
	 * sector number matches. This is very important for the PIO mode of this code,
	 * else we'll never complete reading properly and never get to reading back
	 * the status bytes. */

	wdo = 9;
	cmd[0] = (allow_multitrack?0x80:0x00)/*Multitrack*/ + (mfm_mode?0x40:0x00)/* MFM */ + 0x06/* READ DATA */;
	cmd[1] = (fdc->digital_out&3)/* [1:0] = DR1,DR0 [2:2] = HD */;
	cmd[2] = cyl;	/* cyl=0 */
	cmd[3] = head;	/* head=0 */
	cmd[4] = sect;	/* sector=1 */
	cmd[5] = ssz;	/* sector size=2 (512 bytes) */
	cmd[6] = sect+nsect-1; /* last sector of the track (what sector to stop at). */
	cmd[7] = 0;	/* gap size (??) */
	cmd[8] = 0xFF;	/* DTL (not used) */
	wd = floppy_controller_write_data(fdc,cmd,wdo);
	if (wd < 2) {
		vga_write("Write data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	vga_write("Read in progress\n");

	/* fires an IRQ */
	if (fdc->use_dma) {
		floppy_controller_wait_irq(fdc,10000,1); /* 10 seconds */
		floppy_controller_wait_data_ready_ms(fdc,1000);
	}
	else {
		while (returned_length < data_length) {
			if ((returned_length+(128 << ssz)) > data_length) break;
			floppy_controller_wait_data_ready_ms(fdc,10000);

			p = floppy_controller_read_data_ndma(fdc,floppy_dma->lin + returned_length,128 << ssz);
			if (p < 0 || p > (128 << ssz)) {
				sprintf(tmp,"NDMA read failed %d (%02xh)\n",p,fdc->main_status);
				vga_write(tmp);
				p = 0;
			}

			returned_length += p;
			/* stop on incomplete transfer */
			if (p != (128 << ssz)) break;
		}
	}

	rdo = 7;
	rd = floppy_controller_read_data(fdc,resp,rdo);
	if (rd < 1) {
		vga_write("Read data port failed\n");
		do_floppy_controller_reset(fdc);
		return;
	}

	/* Read Sector (x6h) response
	 *
	 *   Byte |  7   6   5   4   3   2   1   0
	 *   -----+---------------------------------
	 *      0 |              ST0
	 *      1 |              ST1
	 *      2 |              ST2
	 *      3 |           Cylinder
	 *      4 |             Head
	 *      5 |        Sector Number
	 *      6 |         Sector Size
         */

	/* the command SHOULD terminate */
	floppy_controller_wait_data_ready(fdc,20);
	if (!floppy_controller_wait_busy_in_instruction(fdc,1000))
		do_floppy_controller_reset(fdc);

	/* accept ST0..ST2 from response and update */
	if (rd >= 3) {
		fdc->st[0] = resp[0];
		fdc->st[1] = resp[1];
		fdc->st[2] = resp[2];
	}
	if (rd >= 4) {
		fdc->cylinder = resp[3];
	}

	/* (if DMA) did we get any data? */
	if (fdc->dma >= 0 && fdc->use_dma) {
		uint32_t dma_len = d8237_read_count(fdc->dma);
		uint8_t status = inp(d8237_ioport(fdc->dma,D8237_REG_R_STATUS));

		/* some DMA controllers reset the count back to the original value on terminal count.
		 * so if the DMA controller says terminal count, the true count is zero */
		if (status&D8237_STATUS_TC(fdc->dma)) dma_len = 0;

		if (dma_len > (uint32_t)data_length) dma_len = (uint32_t)data_length;
		returned_length = (unsigned int)((uint32_t)data_length - dma_len);
	}

	sprintf(tmp,"%lu bytes received\n",(unsigned long)returned_length);
	vga_write(tmp);
	vga_write("\n");

	vga_write_color(0x0F);
	for (p=0;p == 0 || p < ((returned_length+255)/256);p++) {
		for (y=0;y < 16;y++) {
			vga_moveto(0,6+y);
			for (x=0;x < 16;x++) {
				sprintf(tmp,"%02x ",floppy_dma->lin[((y+(p*16))*16)+x]);
				vga_write(tmp);
			}

			vga_moveto(50,6+y);
			for (x=0;x < 16;x++) {
				tmp[0] = floppy_dma->lin[((y+(p*16))*16)+x];
				if (tmp[0] < 32) tmp[0] = '.';
				tmp[1] = 0;
				vga_write(tmp);
			}
		}

		wait_for_enter_or_escape();
	}
}

void do_floppy_controller_read_tests(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Read tests ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			floppy_controller_read_status(fdc);
			floppy_controller_read_DIR(fdc);
			floppy_controller_read_ps2_status(fdc);

			y = 2;
			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"DOR %02xh DIR %02xh Stat %02xh CCR %02xh cyl=%-3u",
				fdc->digital_out,fdc->digital_in,
				fdc->main_status,fdc->control_cfg,
				fdc->cylinder);
			vga_write(tmp);

			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"ST0..3: %02x %02x %02x %02x PS/2 %02x %02x",
				fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3],
				fdc->ps2_status[0],fdc->ps2_status[1]);
			vga_write(tmp);

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("FDC menu");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("Read sector");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* Read Sector */
				do_floppy_read_test(fdc);
				backredraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 0;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 0)
				select = -1;

			redraw = 1;
		}
	}
}

void do_floppy_controller_write_tests(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Write tests ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			floppy_controller_read_status(fdc);
			floppy_controller_read_DIR(fdc);
			floppy_controller_read_ps2_status(fdc);

			y = 2;
			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"DOR %02xh DIR %02xh Stat %02xh CCR %02xh cyl=%-3u",
				fdc->digital_out,fdc->digital_in,
				fdc->main_status,fdc->control_cfg,
				fdc->cylinder);
			vga_write(tmp);

			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"ST0..3: %02x %02x %02x %02x PS/2 %02x %02x",
				fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3],
				fdc->ps2_status[0],fdc->ps2_status[1]);
			vga_write(tmp);

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("FDC menu");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("Write sector");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* Write Sector */
				do_floppy_write_test(fdc);
				backredraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 0;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 0)
				select = -1;

			redraw = 1;
		}
	}
}

void do_floppy_controller(struct floppy_controller *fdc) {
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	unsigned int x,y;
	char redraw=1;
	int select=-1;
	int c;

	/* and allocate DMA too */
	if (fdc->dma >= 0 && floppy_dma == NULL) {
		uint32_t choice = 32768;

		do {
			floppy_dma = dma_8237_alloc_buffer(choice);
			if (floppy_dma == NULL) choice -= 4096UL;
		} while (floppy_dma == NULL && choice > 4096UL);

		if (floppy_dma == NULL) return;
	}

	/* if the floppy struct says to use interrupts, then do it */
	do_floppy_controller_enable_irq(fdc,fdc->use_dma);

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Floppy controller ");
			sprintf(tmp,"@%X",fdc->base_io);
			vga_write(tmp);
			if (fdc->irq >= 0) {
				sprintf(tmp," IRQ %d",fdc->irq);
				vga_write(tmp);
			}
			if (fdc->dma >= 0) {
				sprintf(tmp," DMA %d",fdc->dma);
				vga_write(tmp);
			}
			if (floppy_dma != NULL) {
				sprintf(tmp," phys=%08lxh len=%04lxh",(unsigned long)floppy_dma->phys,(unsigned long)floppy_dma->length);
				vga_write(tmp);
			}
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			floppy_controller_read_status(fdc);
			floppy_controller_read_DIR(fdc);
			floppy_controller_read_ps2_status(fdc);

			y = 2;
			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"DOR %02xh DIR %02xh Stat %02xh CCR %02xh cyl=%-3u",
				fdc->digital_out,fdc->digital_in,
				fdc->main_status,fdc->control_cfg,
				fdc->cylinder);
			vga_write(tmp);

			vga_moveto(8,y++);
			vga_write_color(0x0F);
			sprintf(tmp,"ST0..3: %02x %02x %02x %02x PS/2 %02x %02x",
				fdc->st[0],fdc->st[1],fdc->st[2],fdc->st[3],
				fdc->ps2_status[0],fdc->ps2_status[1]);
			vga_write(tmp);

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Main menu");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 0) ? 0x70 : 0x0F);
			vga_write("DMA/IRQ: ");
			vga_write(fdc->use_dma ? "Enabled" : "Disabled");
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 1) ? 0x70 : 0x0F);
			vga_write("Reset signal: ");
			vga_write((fdc->digital_out&0x04) ? "Off" : "On");
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 2) ? 0x70 : 0x0F);
			vga_write("Drive A motor: ");
			vga_write((fdc->digital_out&0x10) ? "On" : "Off");
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 3) ? 0x70 : 0x0F);
			vga_write("Drive B motor: ");
			vga_write((fdc->digital_out&0x20) ? "On" : "Off");
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 4) ? 0x70 : 0x0F);
			vga_write("Drive C motor: ");
			vga_write((fdc->digital_out&0x40) ? "On" : "Off");
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 5) ? 0x70 : 0x0F);
			vga_write("Drive D motor: ");
			vga_write((fdc->digital_out&0x80) ? "On" : "Off");
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 6) ? 0x70 : 0x0F);
			vga_write("Drive select: ");
			sprintf(tmp,"%c(%u)",(fdc->digital_out&3)+'A',fdc->digital_out&3);
			vga_write(tmp);
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 7) ? 0x70 : 0x0F);
			vga_write("Transfer rate: ");
			switch (fdc->control_cfg&3) {
				case 0:	vga_write("500kbit/sec"); break;
				case 1:	vga_write("300kbit/sec"); break;
				case 2:	vga_write("250kbit/sec"); break;
				case 3:	vga_write("1mbit/sec"); break;
			};
			vga_write(" (hit enter to toggle)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 8) ? 0x70 : 0x0F);
			vga_write("Position C/H/S/sz/count: ");
			sprintf(tmp,"%u/%u/%u/%u/%u MT=%u %s",current_track,current_head,
				current_sect,128 << current_sectsize_p2,current_sectcount,
				allow_multitrack,
				mfm_mode?"MFM":"FM");

			vga_write(tmp);
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
			y++;

			vga_moveto(8,y++);
			vga_write_color((select == 9) ? 0x70 : 0x0F);
			vga_write("Check drive status (x4h)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 10) ? 0x70 : 0x0F);
			vga_write("Calibrate (x7h)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 11) ? 0x70 : 0x0F);
			vga_write("Seek (xfh)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 12) ? 0x70 : 0x0F);
			vga_write("Seek relative (1xfh)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 13) ? 0x70 : 0x0F);
			vga_write("Read sector ID (xah)");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 14) ? 0x70 : 0x0F);
			vga_write("Read testing >>");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');

			vga_moveto(8,y++);
			vga_write_color((select == 15) ? 0x70 : 0x0F);
			vga_write("Write testing >>");
			while (vga_pos_x < (vga_width-8) && vga_pos_x != 0) vga_writec(' ');
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select == 0) { /* DMA/IRQ enable */
				floppy_controller_enable_dma(fdc,!fdc->use_dma);
				redraw = 1;
			}
			else if (select == 1) { /* reset */
				floppy_controller_set_reset(fdc,!!(fdc->digital_out&0x04)); /* bit is INVERTED 1=normal 0=reset */
				redraw = 1;
			}
			else if (select == 2) { /* Drive A motor */
				floppy_controller_set_motor_state(fdc,0/*A*/,!(fdc->digital_out&0x10));
				redraw = 1;
			}
			else if (select == 3) { /* Drive B motor */
				floppy_controller_set_motor_state(fdc,1/*B*/,!(fdc->digital_out&0x20));
				redraw = 1;
			}
			else if (select == 4) { /* Drive C motor */
				floppy_controller_set_motor_state(fdc,2/*C*/,!(fdc->digital_out&0x40));
				redraw = 1;
			}
			else if (select == 5) { /* Drive D motor */
				floppy_controller_set_motor_state(fdc,3/*D*/,!(fdc->digital_out&0x80));
				redraw = 1;
			}
			else if (select == 6) { /* Drive select */
				floppy_controller_drive_select(fdc,((fdc->digital_out&3)+1)&3);
				redraw = 1;
			}
			else if (select == 7) { /* Transfer rate */
				floppy_controller_set_data_transfer_rate(fdc,((fdc->control_cfg&3)+1)&3);
				redraw = 1;
			}

			else if (select == 9) { /* check drive status */
				do_check_drive_status(fdc);
				redraw = 1;
			}
			else if (select == 10) { /* calibrate drive */
				do_calibrate_drive(fdc);
				redraw = 1;
			}
			else if (select == 11) { /* seek */
				unsigned long track = prompt_track_number();
				if (track != (~0UL) && track < 256) {
					do_seek_drive(fdc,(uint8_t)track);
					redraw = 1;
				}
			}
			else if (select == 12) { /* seek relative */
				signed long track = prompt_signed_track_number();
				if (track >= -255L && track <= 255L) {
					do_seek_drive_rel(fdc,(int)track);
					redraw = 1;
				}
			}
			else if (select == 13) { /* read sector ID */
				do_read_sector_id_demo(fdc);
				backredraw = 1;
			}
			else if (select == 14) { /* read testing */
				do_floppy_controller_read_tests(fdc);
				backredraw = 1;
			}
			else if (select == 15) { /* write testing */
				do_floppy_controller_write_tests(fdc);
				backredraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (--select < -1)
				select = 15;

			redraw = 1;
		}
		else if (c == 0x5000) {
			if (++select > 15)
				select = -1;

			redraw = 1;
		}
	}

	if (floppy_dma != NULL) {
		dma_8237_free_buffer(floppy_dma);
		floppy_dma = NULL;
	}

	do_floppy_controller_enable_irq(fdc,0);
	floppy_controller_enable_dma_otr(fdc,1); /* because BIOSes probably won't */
	p8259_unmask(fdc->irq);
}

void do_main_menu() {
	char redraw=1;
	char backredraw=1;
	VGA_ALPHA_PTR vga;
	struct floppy_controller *floppy;
	unsigned int x,y,i;
	int select=-1;
	int c;

	while (1) {
		if (backredraw) {
			vga = vga_alpha_ram;
			backredraw = 0;
			redraw = 1;

			for (y=0;y < vga_height;y++) {
				for (x=0;x < vga_width;x++) {
					*vga++ = 0x1E00 + 177;
				}
			}

			vga_moveto(0,0);

			vga_write_color(0x1F);
			vga_write("        Floppy controller test program");
			while (vga_pos_x < vga_width && vga_pos_x != 0) vga_writec(' ');
		}

		if (redraw) {
			redraw = 0;

			y = 5;
			vga_moveto(8,y++);
			vga_write_color((select == -1) ? 0x70 : 0x0F);
			vga_write("Exit program");
			y++;

			for (i=0;i < MAX_FLOPPY_CONTROLLER;i++) {
				floppy = floppy_get_controller(i);
				if (floppy != NULL) {
					vga_moveto(8,y++);
					vga_write_color((select == (int)i) ? 0x70 : 0x0F);

					sprintf(tmp,"Controller @ %04X",floppy->base_io);
					vga_write(tmp);

					if (floppy->irq >= 0) {
						sprintf(tmp," IRQ %2d",floppy->irq);
						vga_write(tmp);
					}

					if (floppy->dma >= 0) {
						sprintf(tmp," DMA %2d",floppy->dma);
						vga_write(tmp);
					}
				}
			}
		}

		c = getch();
		if (c == 0) c = getch() << 8;

		if (c == 27) {
			break;
		}
		else if (c == 13) {
			if (select == -1) {
				break;
			}
			else if (select >= 0 && select < MAX_FLOPPY_CONTROLLER) {
				floppy = floppy_get_controller(select);
				if (floppy != NULL) do_floppy_controller(floppy);
				backredraw = redraw = 1;
			}
		}
		else if (c == 0x4800) {
			if (select <= -1)
				select = MAX_FLOPPY_CONTROLLER - 1;
			else
				select--;

			while (select >= 0 && floppy_get_controller(select) == NULL)
				select--;

			redraw = 1;
		}
		else if (c == 0x5000) {
			select++;
			while (select >= 0 && select < MAX_FLOPPY_CONTROLLER && floppy_get_controller(select) == NULL)
				select++;
			if (select >= MAX_FLOPPY_CONTROLLER)
				select = -1;

			redraw = 1;
		}
	}
}

int main(int argc,char **argv) {
	struct floppy_controller *reffdc;
	struct floppy_controller *newfdc;
	int i;

	/* we take a GUI-based approach (kind of) */
	if (!probe_vga()) {
		printf("Cannot init VGA\n");
		return 1;
	}
	if (!probe_8237())
		printf("WARNING: Cannot init 8237 DMA\n");
	/* the floppy code has some timing requirements and we'll use the 8254 to do it */
	/* newer motherboards don't even have a floppy controller and it's probable they'll stop implementing the 8254 at some point too */
	if (!probe_8254()) {
		printf("8254 chip not detected\n");
		return 1;
	}
	if (!probe_8259()) {
		printf("8259 chip not detected\n");
		return 1;
	}
	if (!init_floppy_controller_lib()) {
		printf("Failed to init floppy controller\n");
		return 1;
	}

	printf("Probing standard FDC ports...\n");
	for (i=0;(reffdc = (struct floppy_controller*)floppy_get_standard_isa_port(i)) != NULL;i++) {
		printf("   %3X IRQ %d DMA %d: ",reffdc->base_io,reffdc->irq,reffdc->dma); fflush(stdout);

		if ((newfdc = floppy_controller_probe(reffdc)) != NULL) {
			printf("FOUND. PS/2=%u AT=%u dma=%u DOR/RW=%u DOR=%02xh DIR=%02xh mst=%02xh\n",
				newfdc->ps2_mode,
				newfdc->at_mode,
				newfdc->use_dma,
				newfdc->digital_out_rw,
				newfdc->digital_out,
				newfdc->digital_in,
				newfdc->main_status);
		}
		else {
			printf("\x0D                             \x0D"); fflush(stdout);
		}
	}

	printf("Hit ENTER to continue, ESC to cancel\n");
	i = wait_for_enter_or_escape();
	if (i == 27) {
		free_floppy_controller_lib();
		return 0;
	}

	do_main_menu();
	free_floppy_controller_lib();
	return 0;
}

