/* NE2K driver for the TOS kernel
 * divya and thomas, csc 720 spring 2011
 */
#include "kernel.h"

#define NE2K_BASE_ADDR	0x300

/* register address assignments, a description of these can be found in
 * docs/DP8390D.pdf */
#define NE2K_REG_CR	0x00

/* page 0 (PS1,PS0) = (0,0) */
/* read: */
#define NE2K_REG_CLDA0	0x01
#define NE2K_REG_CLDA1	0x02
#define NE2K_REG_BNRY	0x03
#define NE2K_REG_TSR	0x04
#define NE2K_REG_NCR	0x05
#define NE2K_REG_FIFO	0x06
#define NE2K_REG_ISR	0x07
#define NE2K_REG_CRDA0	0x08
#define NE2K_REG_CRDA1	0x09
#define NE2K_REG_RSR	0x0C
#define NE2K_REG_CNTR0	0x0D
#define NE2K_REG_CNTR1	0x0E
#define NE2K_REG_CNTR2	0x0F

/* write: */
#define NE2K_REG_PSTART	0x01
#define NE2K_REG_PSTOP	0x02
#define NE2K_REG_TPSR	0x04
#define NE2K_REG_TBCR0	0x05
#define NE2K_REG_TBCR1	0x06
#define NE2K_REG_RSAR0	0x08
#define NE2K_REG_RSAR1	0x09
#define NE2K_REG_RBCR0	0x0A
#define NE2K_REG_RBCR1	0x0B
#define NE2K_REG_RCR	0x0C
#define NE2K_REG_TCR	0x0D
#define NE2K_REG_DCR	0x0E
#define NE2K_REG_IMR	0x0F

/* page 1 (PS1,PS0) = (0,1) */
#define NE2K_REG_PAR0	0x01
#define NE2K_REG_PAR1	0x02
#define NE2K_REG_PAR2	0x03
#define NE2K_REG_PAR3	0x04
#define NE2K_REG_PAR4	0x05
#define NE2K_REG_PAR5	0x06
#define NE2K_REG_CURR	0x07
#define NE2K_REG_MAR0	0x08
#define NE2K_REG_MAR1	0x09
#define NE2K_REG_MAR2	0x0A
#define NE2K_REG_MAR3	0x0B
#define NE2K_REG_MAR4	0x0C
#define NE2K_REG_MAR5	0x0D
#define NE2K_REG_MAR6	0x0E
#define NE2K_REG_MAR7	0x0F

/* register command sets */
/* command register */
#define NE2K_CR_STP	1 << 0
#define NE2K_CR_STA	1 << 1
#define NE2K_CR_TXP	1 << 2
#define NE2K_CR_RD0	1 << 3
#define NE2K_CR_RD1	1 << 4
#define NE2K_CR_RD2	1 << 5
#define NE2K_CR_PS0	1 << 6
#define NE2K_CR_PS1	1 << 7

/* data configuration */
#define NE2K_DCR_WTS	1 << 0
#define NE2K_DCR_LS		1 << 3
#define NE2K_DCR_FT1	1 << 6

/* transmission control */
#define NE2K_TCR_LB0	1 << 1

#define NE2K_CR_P0		0x00
#define NE2K_CR_P1		NE2K_CR_PS0
#define NE2K_CR_PMSK	0x03 << 6

/* check these values */
#define NE2K_ASIC_OFFSET	0x10
#define NE2K_NOVELL_RESET	0x0F
#define NE2K_NOVELL_DATA	0x00

/* local buffer usage (high bytes) */
#define NE2K_TXPAGE_START		0x20
#define NE2K_PAGE_START_ADDR	0x26
#define NE2K_PAGE_STOP_ADDR		0x40

#define NE2K_IRQ	0x09

unsigned short htons(unsigned short s) {
	int r;
	r = s << 8;
	r |= s >> 8;
	return r;
}


/* these functions seem sort of redundant.. */

/* write byte to given register, switch page before calling this */
int ne2k_reg_write(struct ne2k_phy *phy, unsigned char reg,
				                        unsigned char byte) {

	outportb(phy->nicaddr + reg, byte);
	return 0;
}

/* read register contents */
unsigned char ne2k_reg_read(struct ne2k_phy *phy,
							unsigned char reg) {

	return inportb(phy->nicaddr + reg);
}

/* read len number of bytes from NIC buffer memory at src,
 * NIC should be initialized and running before calling this.
 * stolen almost verbatim from sanos */
void ne2k_read_mem(struct ne2k_phy *phy, unsigned short src, void *dst,
										 unsigned short len) {
	/* align words */
	if (len & 1) len++;
	/* finish DMA */
	ne2k_reg_write(phy, NE2K_REG_CR, NE2K_CR_RD2 | NE2K_CR_STA);

	/* start reading at src */
	ne2k_reg_write(phy, NE2K_REG_RSAR0, src);
	ne2k_reg_write(phy, NE2K_REG_RSAR1, src >> 8);

	/* for len bytes */
	ne2k_reg_write(phy, NE2K_REG_RBCR0, len);
	ne2k_reg_write(phy, NE2K_REG_RBCR1, len >> 8);

	/* select remote DMA read */
	ne2k_reg_write(phy, NE2K_REG_CR, NE2K_CR_RD0 | NE2K_CR_STA);

	/* do 16-bit DMA read */
	insw(phy->asicaddr, dst, len >> 1);
}

/* return current page ne2k is on */
unsigned char ne2k_reg_get_page(struct ne2k_phy *phy) {

	unsigned char page = ne2k_reg_read(phy, NE2K_REG_CR) & NE2K_CR_PMSK;
	return page >> 6;
}

/* is this function really necessary? Performs a safe page switch */
int ne2k_reg_sw_page(struct ne2k_phy *phy, int pagenum) {

	unsigned char page = ne2k_reg_read(phy, NE2K_REG_CR);
	int err;

	switch (pagenum) {
		case 0:
			page |= NE2K_CR_P0;
			page &= ~NE2K_CR_P1;
			break;
		case 1:
			page |= NE2K_CR_P1;
			page &= ~NE2K_CR_P0;
			break;
		default:
			kprintf("ne2k: page not implemented\n");
			goto err_out;
	}

	if (err = ne2k_reg_write(phy, NE2K_REG_CR, page))
		goto err_out;

	return 0;
err_out:
	return err;
}

/* print registers for current page */
void ne2k_reg_hexdump(struct ne2k_phy *phy) {

	int i;
	int page = ne2k_reg_get_page(phy);
	kprintf("\nne2k page #%d: ", page);
	for (i = 0; i <= 0x0F; i++) {
		kprintf("\n%02X %02X ", i, ne2k_reg_read(phy, i));
	}
}

void ne2k_handle_irq() {

	kprintf("\n***********IRQ SERVICED!***********");

}

void ne2k_isr() {

	asm ("push %eax; push %ecx; push %edx");
	asm ("push %ebx; push %ebp; push %esi; push %edi");

	ne2k_handle_irq();

	asm ("movb $0x20,%al");
	asm ("outb %al,$0x20");
	asm ("pop %edi; pop %esi; pop %ebp; pop %ebx");
	asm ("pop %edx; pop %ecx; pop %eax");
	asm ("iret");
}

/* try to detect presence of ne2k at phy->nicaddr, copied from ne2k.c
 * in sanos (jbox.dk/sanos) */
static int ne2k_probe(struct ne2k_phy *phy)
{
	unsigned char byte;

	/* reset */
	byte = inportb(phy->asicaddr + NE2K_NOVELL_RESET);
	outportb(phy->asicaddr + NE2K_NOVELL_RESET, byte);
	outportb(phy->nicaddr + NE2K_REG_CR, NE2K_CR_RD2 | NE2K_CR_STP);

	sleep(50);

	// Test for a generic DP8390 NIC
	byte = inportb(phy->nicaddr + NE2K_REG_CR);
	byte &= NE2K_CR_RD2 | NE2K_CR_TXP | NE2K_CR_STA | NE2K_CR_STP;
	if (byte != (NE2K_CR_RD2 | NE2K_CR_STP)) return 1;

	byte = inportb(phy->nicaddr + NE2K_REG_ISR);
	byte &= 0x80;	//NE2K_ISR_RST
	if (byte != 0x80) return 1;

	return 0;
}


/* the init procedure is described on p. 29 of the datasheet, as well as the
 * driver reference implementation in docs/writingdriversfortheDP8390.pdf */
int ne2k_start(struct ne2k_phy *phy) {

	int err;

	if((err = ne2k_probe(phy)))
		goto err_out;

	kprintf("ne2k: probe successful\n");

	/* 1) stop mode 0x21, abort DMA and stop card */
	ne2k_reg_write(phy, NE2K_REG_CR, NE2K_CR_RD2 | NE2K_CR_STP);
	/* 2) init DCR,  FIFO rx threshold 8 bytes, normal loopback (off),
	 * and 16-bit wide DMA transfers */
	ne2k_reg_write(phy, NE2K_REG_DCR, NE2K_DCR_FT1 | NE2K_DCR_LS | NE2K_DCR_WTS);
	/* 3) clear RBCR0 and RBCR1 */
	ne2k_reg_write(phy, NE2K_REG_RBCR0, 0x00);
	ne2k_reg_write(phy, NE2K_REG_RBCR1, 0x00);
	/* 4) init rx configuration register */
	ne2k_reg_write(phy, NE2K_REG_RCR, 0x00);
	/* 5) place NIC in internal loopback mode 1 */
	ne2k_reg_write(phy, NE2K_REG_TCR, NE2K_TCR_LB0);
	/* set tx buffer page start addr */
	ne2k_reg_write(phy, NE2K_REG_TPSR, NE2K_TXPAGE_START);
	/* 6) init recv buffer ring, BNRY, PSTART, and PSTOP */
	ne2k_reg_write(phy, NE2K_REG_PSTART, NE2K_PAGE_START_ADDR);
	ne2k_reg_write(phy, NE2K_REG_BNRY, NE2K_PAGE_START_ADDR);
	ne2k_reg_write(phy, NE2K_REG_PSTOP, NE2K_PAGE_STOP_ADDR);
	/* 7) clear ISR */
	ne2k_reg_write(phy, NE2K_REG_ISR, 0xFF);
	/* 8) init IMR */
	ne2k_reg_write(phy, NE2K_REG_IMR, 0x0B);
	/* 9) switch to page 1 and init PAR0-5, MAR0-7, and CURR */
	ne2k_reg_sw_page(phy, 1);
	ne2k_reg_write(phy, NE2K_REG_CURR, NE2K_PAGE_START_ADDR);
	/* 10) put NIC in START mode, back in page 0 */
	ne2k_reg_write(phy, NE2K_REG_CR, NE2K_CR_RD2 | NE2K_CR_STA);
	/* 11) init TCR in normal mode */
	ne2k_reg_write(phy, NE2K_REG_TCR, 0x00);

	/* install interrupt handler */
	init_idt_entry(NE2K_IRQ + 0x60, ne2k_isr);

	return 0;
err_out:
	return err;
}

int ne2k_get_attr(struct ne2k_phy *phy) {

	/* MAC is stored in 16 bytes, 2 identical bytes per word */
	char macbuf[16];
	int i;
	int page = ne2k_reg_get_page(phy);
	ne2k_read_mem(phy, NE2K_NOVELL_DATA, (void *) macbuf, 16);

	/* read mac into phy and card registers */
	ne2k_reg_sw_page(phy, 1);		/* switch back later ? */
	for (i = 0; i < ETH_ALEN; i++) {
		phy->macaddr.byte[i] = macbuf[i * 2];
		/* move this into ne2k_conf() later */
		ne2k_reg_write(phy, NE2K_REG_PAR0 + i, macbuf[i * 2]);
	}
	ne2k_reg_sw_page(phy, page);
	return 0;
}

/* turn on ne2k and fill in phy */
int ne2k_init(struct ne2k_phy *phy) {

	int err;
	phy->nicaddr = NE2K_BASE_ADDR;
	phy->asicaddr = phy->nicaddr + NE2K_ASIC_OFFSET;

	/* turn on the card */
	if(err = ne2k_start(phy)) {
		kprintf("ne2k: failed to bring up interface: %d", err);
		goto err_out;
	}

	/* read attributes */
	if (err = ne2k_get_attr(phy)) {
		kprintf("ne2k: failed to get attributes: %d", err);
		goto err_out;
	}

	return 0;
err_out:
	return err;
}


void ne2k_print_mac(WINDOW* wnd, struct ne2k_phy *phy) {

	int i;
	for (i = 0; i < ETH_ALEN; i++) {
		wprintf(wnd, "%02X", phy->macaddr.byte[i]);
		if (i != ETH_ALEN - 1)
			wprintf(wnd, ":");
	}
	int page = ne2k_reg_get_page(phy);
	ne2k_reg_sw_page(phy, 1);
	ne2k_reg_hexdump(phy);
	ne2k_reg_sw_page(phy, page);
}

void ne2k_process(PROCESS self, PARAM param) {

	while (1) {
		/* driver stuff */
	}
}

void init_ne2k() {

	int err;
	if (err = ne2k_init(&ne2k_phy)) {
		kprintf("ne2k: couldn't bring up card! error %d\n", err);
		return;
	}

	/* give NE2000 priority 1 for now since the scheduler isn't preemptive
	 * and we busy wait */
	create_process(ne2k_process, 1, 0, "NE2000");
	resign();
}
