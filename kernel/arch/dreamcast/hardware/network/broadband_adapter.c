/* KallistiOS ##version##

   net/broadband_adapter.c

   Copyright (C) 2001,2003,2005 Dan Potter
   Copyright (C) 2004 Vincent Penne
   Copyright (C) 2007, 2008, 2010 Lawrence Sebald

 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <dc/net/broadband_adapter.h>
#include <dc/asic.h>
#include <dc/g2bus.h>
#include <dc/sq.h>
#include <dc/flashrom.h>
#include <arch/irq.h>
#include <arch/cache.h>
#include <kos/net.h>
#include <kos/thread.h>
#include <kos/sem.h>

//#define vid_border_color(r, g, b) (void)0 /* nothing */

/* Configuration definitions */

#define RTL_MEM                 (0x1840000)

#define RX_NOWRAP               1 /* 1 for no wrapping or 0 to use wrapping mode (Ignored for 64Kb buffer length) */
#define RX_MAX_DMA_BURST        6 /* 2^(4+n) bytes from 0-6 (16b - 1Kb) or 7 for unlimited */
#define RX_BUFFER_LEN_SHIFT     1 /* 0 : 8Kb, 1 : 16Kb, 2 : 32Kb, 3 : 64Kb */
#define RX_FIFO_THRESHOLD       0 /* 2^(4+n) bytes from 0-6 (16b - 1Kb) or 7 for none */
#define RX_EARLY_THRESHOLD      0 /* Early RX Threshold multiplier n/16 or 0 for none */

#define RX_CONFIG               (RX_EARLY_THRESHOLD<<24) | (RX_FIFO_THRESHOLD<<13) | \
                                (RX_BUFFER_LEN_SHIFT<<11) | (RX_MAX_DMA_BURST<<8) | \
                                (RX_NOWRAP<<7)
                                
#define TX_MAX_DMA_BURST        6 /* 2^(4+n) bytes from 0-7 (16b - 2Kb) */
#define TX_CONFIG               (TX_MAX_DMA_BURST<<8)

#define RX_BUFFER_LEN           (0x2000<<RX_BUFFER_LEN_SHIFT)

#define TX_BUFFER_OFFSET        (RX_BUFFER_LEN + 0x2000)
#define TX_BUFFER_LEN           (0x800)
#define TX_NB_BUFFERS           4

/* original value */
//#define BBA_ASIC_IRQ ASIC_IRQB

/* testing */
#define BBA_ASIC_IRQ ASIC_IRQ_DEFAULT

/* Use a customized g2_read_block function */
#define FAST_G2_READ

/* DMA transfer will be used only if the amount of bytes exceeds that threshold */
#define DMA_THRESHOLD 128 // looks like a good value

/* Since callbacks will be running with interrupts enabled,
   it might be a good idea to protect bba_tx with a semaphore from inside.
   I'm not sure lwip needs that, but dcplaya does when using both lwip and its
   own dcload syscalls emulation.*/
#define TX_SEMA

/* If this is defined, the dma buffer will be located in P2 area, and no call to
   dcache_inval_range need to be done before receiving data.
   TODO : make some benchmark to see which method is faster */
//#define USE_P2_AREA

/*

Contains a low-level ethernet driver for the "Broadband Adapter", which
is principally a RealTek 8193C chip attached to the G2 external bus
using a PCI bridge chip called "GAPS PCI". GAPS PCI might ought to be
in its own module, but AFAIK this is the only peripheral to use this
chip, and quite possibly will be the only peripheral to ever use it.

Thanks to Andrew Kieschnick for finishing the driver info for the rtl8193c
(mainly the transmit code, and lots of help with the error correction).
Also thanks to the NetBSD sources for some info on register names.

This driver has basically been rewritten since KOS 1.0.x.

*/

/****************************************************************************/
/* GAPS PCI stuff probably ought to be moved to another file... */

/* Detect a GAPS PCI bridge */
static int gaps_detect() {
    char str[16];

    g2_read_block_8((uint8 *)str, 0xa1001400, 16);

    if(!strncmp(str, "GAPSPCI_BRIDGE_2", 16))
        return 0;
    else
        return -1;
}

/* Initialize GAPS PCI bridge */
#define GAPS_BASE 0xa1000000
static int gaps_init() {
    int i;

    /* Make sure we have one */
    if(gaps_detect() < 0) {
        dbglog(DBG_INFO, "bba: no ethernet card found\n");
        return -1;
    }

    /* Initialize the "GAPS" PCI glue controller.
       It ain't pretty but it works. */
    g2_write_32(GAPS_BASE + 0x1418, 0x5a14a501);    /* M */
    i = 10000;

    while(!(g2_read_32(GAPS_BASE + 0x1418) & 1) && i > 0)
        i--;

    if(!(g2_read_32(GAPS_BASE + 0x1418) & 1)) {
        dbglog(DBG_ERROR, "bba: GAPS PCI controller not responding; giving up!\n");
        return -1;
    }

    g2_write_32(GAPS_BASE + 0x1420, 0x01000000);
    g2_write_32(GAPS_BASE + 0x1424, 0x01000000);
    g2_write_32(GAPS_BASE + 0x1428, RTL_MEM);       /* DMA Base */
    g2_write_32(GAPS_BASE + 0x142c, RTL_MEM + 32 * 1024); /* DMA End */
    g2_write_32(GAPS_BASE + 0x1414, 0x00000001);        /* Interrupt enable */
    g2_write_32(GAPS_BASE + 0x1434, 0x00000001);

    /* Configure PCI bridge (very very hacky). If we wanted to be proper,
       we ought to implement a full PCI subsystem. In this case that is
       ridiculous for accessing a single card that will probably never
       change. Considering that the DC is now out of production officially,
       there is a VERY good chance it will never change. */
    g2_write_16(GAPS_BASE + 0x1606, 0xf900);
    g2_write_32(GAPS_BASE + 0x1630, 0x00000000);
    g2_write_8(GAPS_BASE + 0x163c, 0x00);
    g2_write_8(GAPS_BASE + 0x160d, 0xf0);
    g2_read_16(GAPS_BASE + 0x0004);
    g2_write_16(GAPS_BASE + 0x1604, 0x0006);
    g2_write_32(GAPS_BASE + 0x1614, 0x01000000);
    g2_read_8(GAPS_BASE + 0x1650);

    return 0;
}

/****************************************************************************/
/* RTL8193C stuff */

/* RTL8139C Config/Status info */
struct {
    uint16  cur_rx;     /* Current Rx read ptr */
    uint16  cur_tx;     /* Current available Tx slot */
    uint8   mac[6];     /* Mac address */
} rtl;

/* 8, 16, and 32 bit access to the PCI I/O space (configured by GAPS) */
#define NIC(ADDR) (0xa1001700 + (ADDR))

/* 8 and 32 bit access to the PCI MEMMAP space (configured by GAPS) */
static uint32 const rtl_mem = 0xa0000000 + RTL_MEM;

/* TX buffer pointers */
static uint32 const txdesc[4] = {
    0xa0000000 + RTL_MEM + TX_BUFFER_OFFSET,
    0xa0000800 + RTL_MEM + TX_BUFFER_OFFSET,
    0xa0001000 + RTL_MEM + TX_BUFFER_OFFSET,
    0xa0001800 + RTL_MEM + TX_BUFFER_OFFSET,
};

/* Is the link stabilized? */
static volatile int link_stable, link_initial;

/* Receive callback */
static eth_rx_callback_t eth_rx_callback;

/* Forward-declaration for IRQ handler */
static void bba_irq_hnd(uint32 code);

/* Reads the MAC address of the BBA into the specified array */
void bba_get_mac(uint8 *arr) {
    memcpy(arr, rtl.mac, 6);
}

/* Set an ethernet packet receive callback */
void bba_set_rx_callback(eth_rx_callback_t cb) {
    eth_rx_callback = cb;
}

/*
  Initializes the BBA

  Returns 0 for success or -1 for failure.
 */
static int bba_hw_init() {
    int i;
    uint32 tmp;

    link_stable = 0;
    link_initial = 0;

    /* Initialize GAPS */
    if(gaps_init() < 0)
        return -1;

    /* Soft-reset the chip */
    g2_write_8(NIC(RT_CHIPCMD), RT_CMD_RESET);

    /* Wait for it to come back */
    i = 100;

    while((g2_read_8(NIC(RT_CHIPCMD)) & RT_CMD_RESET) && i > 0) {
        i--;
        thd_sleep(10);
    }

    if(g2_read_8(NIC(RT_CHIPCMD)) & RT_CMD_RESET) {
        dbglog(DBG_ERROR, "bba: timed out on reset #1\n");
        return -1;
    }

    /* Reset CONFIG1 */
    g2_write_8(NIC(RT_CONFIG1), 0);

    /* Enable auto-negotiation and restart that process */
    /* NIC16(RT_MII_BMCR) = 0x1200; */
    g2_write_16(NIC(RT_MII_BMCR), 0x9200);

    /* Do another reset */
    g2_write_8(NIC(RT_CHIPCMD), RT_CMD_RESET);

    /* Wait for it to come back */
    i = 100;

    while((g2_read_8(NIC(RT_CHIPCMD)) & RT_CMD_RESET) && i > 0) {
        i--;
        thd_sleep(10);
    }

    if(g2_read_8(NIC(RT_CHIPCMD)) & RT_CMD_RESET) {
        dbglog(DBG_ERROR, "bba: timed out on reset #2\n");
        return -1;
    }

    /* Enable writing to the config registers */
    g2_write_8(NIC(RT_CFG9346), 0xc0);

    /* Read MAC address */
    tmp = g2_read_32(NIC(RT_IDR0));
    rtl.mac[0] = tmp & 0xff;
    rtl.mac[1] = (tmp >> 8) & 0xff;
    rtl.mac[2] = (tmp >> 16) & 0xff;
    rtl.mac[3] = (tmp >> 24) & 0xff;
    tmp = g2_read_32(NIC(RT_IDR0 + 4));
    rtl.mac[4] = tmp & 0xff;
    rtl.mac[5] = (tmp >> 8) & 0xff;
    dbglog(DBG_INFO, "bba: MAC Address is %02x:%02x:%02x:%02x:%02x:%02x\n",
           rtl.mac[0], rtl.mac[1], rtl.mac[2],
           rtl.mac[3], rtl.mac[4], rtl.mac[5]);

    /* Enable receive and transmit functions */
    g2_write_8(NIC(RT_CHIPCMD), RT_CMD_RX_ENABLE | RT_CMD_TX_ENABLE);

    /* Set Rx FIFO threashold to 1K, Rx size to 16k+16, 1024 byte DMA burst */
    g2_write_32(NIC(RT_RXCONFIG), RX_CONFIG);

    /* Set Tx 1024 byte DMA burst */
    g2_write_32(NIC(RT_TXCONFIG), TX_CONFIG);

    /* Turn off lan-wake and set the driver-loaded bit */
    g2_write_8(NIC(RT_CONFIG1), (g2_read_8(NIC(RT_CONFIG1)) & ~0x30) | 0x20);

    /* Enable FIFO auto-clear */
    g2_write_8(NIC(RT_CONFIG4), g2_read_8(NIC(RT_CONFIG4)) | 0x80);

    /* Switch back to normal operation mode */
    g2_write_8(NIC(RT_CFG9346), 0);

    /* Setup RX buffer */
    g2_write_32(NIC(RT_RXBUF), RTL_MEM);

    /* Setup TX buffers */
    for(i=0; i<TX_NB_BUFFERS; i++)
        g2_write_32(NIC(RT_TXADDR0 + (i*4)), RTL_MEM + (i* TX_BUFFER_LEN) + TX_BUFFER_OFFSET);

    /* Reset RXMISSED counter */
    g2_write_32(NIC(RT_RXMISSED), 0);

    /* Enable receiving broadcast and physical match packets */
    g2_write_32(NIC(RT_RXCONFIG), g2_read_32(NIC(RT_RXCONFIG)) | 0x0000000a);

    /* Filter out all multicast packets */
    g2_write_32(NIC(RT_MAR0 + 0), 0);
    g2_write_32(NIC(RT_MAR0 + 4), 0);

    /* Disable all multi-interrupts */
    g2_write_16(NIC(RT_MULTIINTR), 0);

    /* Enable G2 interrupts */
    asic_evt_set_handler(ASIC_EVT_EXP_PCI, bba_irq_hnd);
    asic_evt_enable(ASIC_EVT_EXP_PCI, BBA_ASIC_IRQ);

    /* Enable receive interrupts */
    /* XXX need to handle more! */
    g2_write_16(NIC(RT_INTRSTATUS), 0xffff);
    g2_write_16(NIC(RT_INTRMASK), RT_INT_PCIERR |
                RT_INT_TIMEOUT |
                RT_INT_RXFIFO_OVERFLOW |
                RT_INT_RXFIFO_UNDERRUN |    // +link change
                RT_INT_RXBUF_OVERFLOW |
                RT_INT_TX_ERR |
                RT_INT_TX_OK |
                RT_INT_RX_ERR |
                RT_INT_RX_OK);

    /* Enable RX/TX once more */
    g2_write_8(NIC(RT_CHIPCMD), RT_CMD_RX_ENABLE | RT_CMD_TX_ENABLE);

    /* Initialize status vars */
    rtl.cur_tx = 0;
    rtl.cur_rx = 0;

    return 0;
}

static void rx_reset() {
    rtl.cur_rx = g2_read_16(NIC(RT_RXBUFHEAD));
    g2_write_16(NIC(RT_RXBUFTAIL), rtl.cur_rx - 16);

    rtl.cur_rx = 0;
    g2_write_8(NIC(RT_CHIPCMD), RT_CMD_TX_ENABLE);

    //g2_write_32(NIC(RT_RXCONFIG), 0x00000e0a);
    g2_write_32(NIC(RT_RXCONFIG), RX_CONFIG | 0x0000000a);

    while(!(g2_read_8(NIC(RT_CHIPCMD)) & RT_CMD_RX_ENABLE))
        g2_write_8(NIC(RT_CHIPCMD), RT_CMD_TX_ENABLE | RT_CMD_RX_ENABLE);

    //g2_write_32(NIC(RT_RXCONFIG), 0x00000e0a);
    g2_write_32(NIC(RT_RXCONFIG), RX_CONFIG | 0x0000000a);
    g2_write_16(NIC(RT_INTRSTATUS), 0xffff);
}

static void bba_hw_shutdown() {
    /* Disable receiver */
    g2_write_32(NIC(RT_RXCONFIG), 0);

    /* Disable G2 interrupts */
    asic_evt_disable(ASIC_EVT_EXP_PCI, BBA_ASIC_IRQ);
    asic_evt_set_handler(ASIC_EVT_EXP_PCI, NULL);
}


//#define g2_read_block_8(a, b, len) memcpy(a, b, len)
//#define g2_read_block_8(a, b, len) memcpy4(a, b, (len) + 3)
//#define g2_read_block_8(a, b, len) sq_cpy(a, b, (len) + 31)
//#define g2_read_block_8(a, b, len) g2_read_block_32(a, b, ((len)+3) >> 2)
#ifdef FAST_G2_READ
#define g2_read_block_8 my_g2_read_block_8

/*
   G2 bus cycles must not be interrupted by IRQs or G2 DMA.
   The following paired macros will take the necessary precautions.
 */

#define DMAC_CHCR1 *((vuint32 *)0xffa0001c)
#define DMAC_CHCR3 *((vuint32 *)0xffa0003c)

#if 0

/* this version stores also CHCR1 state and stop it, doesn't seem to make
   any difference so I removed it for now */
#define G2_LOCK(OLD1, OLD2) \
    do { \
        OLD1 = irq_disable(); \
        /* suspend any G2 DMA here... */ \
        OLD2 = (DMAC_CHCR3&0xffff) + (DMAC_CHCR1 <<16); \
        DMAC_CHCR3 = (OLD2&0xffff) & ~1; \
        DMAC_CHCR1 = (OLD2>>16) & ~1; \
        while((*(vuint32 *)0xa05f688c) & 0x20) \
            ; \
    } while(0)

#define G2_UNLOCK(OLD1, OLD2) \
    do { \
        /* resume any G2 DMA here... */ \
        DMAC_CHCR3 = (OLD2&0xffff); \
        DMAC_CHCR1 = (OLD2>>16); \
        irq_restore(OLD1); \
    } while(0)

#else

#define G2_LOCK(OLD1, OLD2) \
    do { \
        OLD1 = irq_disable(); \
        /* suspend any G2 DMA here... */ \
        OLD2 = DMAC_CHCR3; \
        DMAC_CHCR3 = OLD2 & ~1; \
        while((*(vuint32 *)0xa05f688c) & 0x20) \
            ; \
    } while(0)

#define G2_UNLOCK(OLD1, OLD2) \
    do { \
        /* resume any G2 DMA here... */ \
        DMAC_CHCR3 = OLD2; \
        irq_restore(OLD1); \
    } while(0)
#endif

static void g2_read_block_8(uint8 *dst, uint8 *src, int len) {
    if(len <= 0)
        return;

    uint32 old1, old2;

    G2_LOCK(old1, old2);

    /* This is in case dst is not multiple of 4, which never happens here */
    /*     while ( (((uint32)dst)&3) ) { */
    /*       *dst++ = *src++; */
    /*       if (!--len) */
    /*  return; */
    /*     } */

    uint32 * d = (uint32 *) dst;
    uint32 * s = (uint32 *) src;
    len = (len + 3) >> 2;

    while(len & 7) {
        *d++ = *s++;
        --len;
    }

    if(!len)
        return;

    len >>= 3;

    do {
        d[0] = *s++;
        d[1] = *s++;
        d[2] = *s++;
        d[3] = *s++;
        d[4] = *s++;
        d[5] = *s++;
        d[6] = *s++;
        d[7] = *s++;
        d += 8;
    }
    while(--len);

    G2_UNLOCK(old1, old2);
}
#endif


#define RXBSZ    (64*1024) /* must be a power of two */
#define MAX_PKTS (RXBSZ / 32)
static struct pkt {
    int pkt_size;
    uint8 * rxbuff;
} rx_pkt[MAX_PKTS];

#define BEFORE 0 // 32*1024
#define AFTER  0 // (BEFORE + 32*1024)

static uint8 rxbuff[RXBSZ + 2 * 1600 + AFTER] __attribute__((aligned(32)));
static uint32 rxbuff_pos;
static int rxin;
static int rxout;
static int dma_used;

static uint32 rx_size;

static kthread_t * bba_rx_thread;
static semaphore_t bba_rx_sema;
static int bba_rx_exit_thread;
static semaphore_t bba_rx_sema2;

static void bba_rx();

#ifdef TX_SEMA
static semaphore_t tx_sema;
#endif

static uint8 * next_dst;
static uint8 * next_src;
static int next_len;

static void rx_finish_enq(int room) {
    /* Tell the chip where we are for overflow checking */
    rtl.cur_rx = (rtl.cur_rx + rx_size + 4 + 3) & ~3;
    g2_write_16(NIC(RT_RXBUFTAIL), (rtl.cur_rx - 16) & (RX_BUFFER_LEN - 1));

    if(room > 0 && (((rxin + 1) % MAX_PKTS) != rxout)) {
        rxin = (rxin + 1) % MAX_PKTS;
        sem_signal(&bba_rx_sema);
        thd_schedule(1, 0);
    }
}

static void bba_dma_cb(ptr_t p) {
    (void)p;

    if(next_len) {
        g2_dma_transfer(next_dst, next_src, next_len, 0,
                        bba_dma_cb, 0,  /* callback */
                        1,  /* dir = 1, we're *reading* from the g2 bus */
                        BBA_DMA_MODE, BBA_DMA_G2CHN, BBA_DMA_SHCHN);
        next_len = 0;
    }
    else {
        rx_finish_enq(1);

        dma_used = 0;

        bba_rx();
    }
}

static int bba_copy_dma(uint8 * dst, uint32 s, int len) {
    uint8 *src = (uint8 *) s;

    if(len <= 0)
        return 1;

    if(len > DMA_THRESHOLD && !irq_inside_int()) {
        uint32 add;

        /*
           This way all will be nicely 32 bytes aligned (assuming that dst and src have
           same alignement initially and that we don't care about the beginning of dst
           buffer)
        */
        add = ((uint32) src) & 31;
        len += add;
        src -= add;
        dst -= add;

#ifndef USE_P2_AREA
        /*
           used to be a call to dcache_inval_range, but for some strange reasons, I need to
           make a full flush now ...
        */
        dcache_flush_range((uint32) dst, len);
#endif

        if(!dma_used) {
            dma_used = 1;
            g2_dma_transfer(dst, src, len, 0,
                            bba_dma_cb, 0,  /* callback */
                            1,  /* dir = 1, we're *reading* from the g2 bus */
                            BBA_DMA_MODE, BBA_DMA_G2CHN, BBA_DMA_SHCHN);
        }
        else {
            next_dst = dst;
            next_src = src;
            next_len = len;
        }

        return 0;
    }
    else {
        g2_read_block_8(dst, src, len);
        return !dma_used;
    }
}
#undef g2_read_block_8

/* Utility function to copy out a some data from the ring buffer into an SH-4
   buffer. This is done to make sure the buffers don't overflow. */
/* XXX Could probably use a memcpy8 here, even */
static int  bba_copy_packet(uint8 * dst, uint32 src, int len) {

#if !RX_NOWRAP

    if((src + len) < RX_BUFFER_LEN) {
#endif
        /* Straight copy is ok */
        return bba_copy_dma(dst, rtl_mem + src, len);
#if !RX_NOWRAP
    }
    else {
        /* Have to copy around the ring end */
        bba_copy_dma(dst, rtl_mem + src, RX_BUFFER_LEN - src);

        return bba_copy_dma(dst + (RX_BUFFER_LEN - src),
                            rtl_mem, len - (RX_BUFFER_LEN - src));
    }

#endif
}

static int rx_enq(int ring_offset, size_t pkt_size) {
    /* If there's no one to receive it, don't bother. */
    if(eth_rx_callback) {
        if(rxin != rxout &&
                (((rx_pkt[rxout].rxbuff - (rxbuff + 32 + BEFORE)) - rxbuff_pos) & (RXBSZ - 1)) < pkt_size + 2048) {
            /*       printf("diff %d, %d\n", (( (rx_pkt[rxout].rxbuff - rxbuff) - rxbuff_pos ) & (RXBSZ-1)), */
            /*       pkt_size); */
            //dbglog(DBG_KDEBUG, "rx_enq: lagging\n");
            return -1;
        }

        /* Receive buffer: temporary space to copy out received data */

#ifdef USE_P2_AREA
        rx_pkt[rxin].rxbuff = rxbuff + 32 + BEFORE + (rxbuff_pos | 0xa0000000) + (ring_offset & 31);
#else
        rx_pkt[rxin].rxbuff = rxbuff + 32 + BEFORE + rxbuff_pos + (ring_offset & 31);
#endif


        rxbuff_pos = (rxbuff_pos + pkt_size + 63) & (RXBSZ - 32);

        rx_pkt[rxin].pkt_size = pkt_size;
        return bba_copy_packet(rx_pkt[rxin].rxbuff, ring_offset, pkt_size);
    }
    else
        return 1;
}

/* Transmit a single packet */
#ifdef TX_SEMA
static int bba_rtx(const uint8 * pkt, int len, int wait)
#else
static int bba_tx(const uint8 * pkt, int len, int wait)
#endif
{
    /*
       int i;

    printf("Transmitting packet:\r\n"); for (i=0; i<len; i++) { printf("%02x ", pkt[i]); if (i
       && !(i % 16)) printf("\r\n"); } printf("\r\n");
    */

    //wait = BBA_TX_WAIT;
    if(!link_stable) {
        if(wait == BBA_TX_WAIT) {
            while(!link_stable)
                ;
        }
        else
            return BBA_TX_AGAIN;
    }

    /* Wait till it's clear to transmit */
    if(wait == BBA_TX_WAIT) {
        while(!(g2_read_32(NIC(RT_TXSTATUS0 + 4 * rtl.cur_tx)) & 0x2000)) {
            if(g2_read_32(NIC(RT_TXSTATUS0 + 4 * rtl.cur_tx)) & 0x40000000)
                g2_write_32(NIC(RT_TXSTATUS0 + 4 * rtl.cur_tx),
                            g2_read_32(NIC(RT_TXSTATUS0 + 4 * rtl.cur_tx)) | 1);
        }
    }
    else {
        if(!(g2_read_32(NIC(RT_TXSTATUS0 + 4 * rtl.cur_tx)) & 0x2000)) {
            return BBA_TX_AGAIN;
        }
    }

    /* Copy the packet out to RTL memory */
    /* XXX could use store queues or memcpy8 here */

    //g2_write_block_8(pkt, txdesc[rtl.cur_tx], len);

    /* Check alignment of the packet, if its 32-bit aligned, use
       g2_write_block_32, if its 16-bit aligned, use g2_write_block_16,
       otherwise, use g2_write_block_8. */
    if(!((uint32)pkt & 0x03)) {
        g2_write_block_32((uint32 *) pkt, txdesc[rtl.cur_tx], (len + 3) >> 2);
    }
    else if(!((uint32)pkt & 0x01)) {
        g2_write_block_16((uint16 *) pkt, txdesc[rtl.cur_tx], (len + 1) >> 1);
    }
    else {
        g2_write_block_8(pkt, txdesc[rtl.cur_tx], len);
    }

    /* All packets must be at least 60 bytes, pad them with null bytes if
       they are not already of an appropriate size. */
    if(len < 60) {
        g2_memset_8(txdesc[rtl.cur_tx] + len, 0, 60 - len);
        len = 60;
    }

    /* Transmit from the current TX buffer */
    g2_write_32(NIC(RT_TXSTATUS0 + 4 * rtl.cur_tx), len);

    /* Go to the next TX buffer */
    rtl.cur_tx = (rtl.cur_tx + 1) % TX_NB_BUFFERS;

    return BBA_TX_OK;
}

#ifdef TX_SEMA
int bba_tx(const uint8 * pkt, int len, int wait) {
    int res;

    if(irq_inside_int()) {
        /*     printf("bba_tx called from an irq !\n"); */
        /*     return 0; */
        //return bba_rtx(pkt, len, wait);
        if(sem_trywait(&tx_sema)) {
            //printf("bba_tx called from an irq while a thread was running it !\n");
            return BBA_TX_OK;   /* sorry guys ... */
        }
    }
    else
        sem_wait(&tx_sema);

    res = bba_rtx(pkt, len, wait);
    sem_signal(&tx_sema);

    return res;
}
#endif

void bba_lock() {
    //sem_wait(&bba_rx_sema2);
    //asic_evt_disable(ASIC_EVT_EXP_PCI, BBA_ASIC_IRQ);
}

void bba_unlock() {
    //asic_evt_enable(ASIC_EVT_EXP_PCI, BBA_ASIC_IRQ);
    //sem_signal(&bba_rx_sema2);
}

static int bcolor;
static void *bba_rx_threadfunc(void *dummy) {
    (void)dummy;

    while(!bba_rx_exit_thread) {
        //sem_wait_timed(&bba_rx_sema, 500);
        sem_wait(&bba_rx_sema);

        if(bba_rx_exit_thread)
            break;

        bcolor = 255;
        //vid_border_color(255, 255, 0);
        bba_lock();

        if(rxout != rxin) {

            /* Call the callback to process it */
            eth_rx_callback(rx_pkt[rxout].rxbuff, rx_pkt[rxout].pkt_size);

            rxout = (rxout + 1) % MAX_PKTS;
        }

        bcolor = 0;
        //vid_border_color(0, 0, 0);
        bba_unlock();
    }

    bba_rx_exit_thread = 0;

    printf("bba_rx_thread exiting ...\n");
    return NULL;
}

static void bba_rx() {
    uint32 rx_status;
    size_t pkt_size, ring_offset;

    //vid_border_color(255, 0, 255);
    while(!(g2_read_8(NIC(RT_CHIPCMD)) & 1)) {
        /* Get frame size and status */
        ring_offset = rtl.cur_rx % RX_BUFFER_LEN;
        rx_status = g2_read_32(rtl_mem + ring_offset);
        rx_size = (rx_status >> 16) & 0xffff;
        pkt_size = rx_size - 4;

        if(rx_size == 0xfff0) {
            //dbglog(DBG_KDEBUG, "bba: early receive triggered\n");
            break;
        }

        /*     if ( ( ( g2_read_16(NIC(RT_RXBUFHEAD)) - ring_offset ) & (RX_BUFFER_LEN-1)) < */
        /*   ( (rx_size+4+3) & (RX_BUFFER_LEN-3-1) )) { */
        /*       //dbglog(DBG_KDEBUG, "bba: oops\n"); */
        /*       break; */
        /*     } */

        if((rx_status & 1) && (pkt_size <= 1514)) {
            /* Add it to the rx queue */
            int res = rx_enq(ring_offset + 4, pkt_size);

            if(!res)
                break;  /* will be finished in the dma callback */
            else
                rx_finish_enq(res);
        }
        else {
            if(!(rx_status & 1)) {
                dbglog(DBG_KDEBUG, "bba: frame receive error, status is %08lx; skipping\n", rx_status);
            }

            dbglog(DBG_KDEBUG, "bba: bogus packet receive detected; skipping packet\n");
            rx_reset();
            break;
        }
    }

    //vid_border_color(bcolor, bcolor, 0);
}

/* Ethernet IRQ handler */
static void bba_irq_hnd(uint32 code) {
    int intr, hnd;

    (void)code;

    //vid_border_color(0, 255, 0);
    /* Acknowledge 8193 interrupt, except RX ACK bits. We'll handle
       those in the RX int handler. */
    intr = g2_read_16(NIC(RT_INTRSTATUS));
    g2_write_16(NIC(RT_INTRSTATUS), intr & ~RT_INT_RX_ACK);

    /* Do processing */
    hnd = 0;

    if(intr & RT_INT_RX_ACK) {

        if(!dma_used) {
            bba_rx();
        }

        /* so that the irq is not called again and again */
        g2_write_16(NIC(RT_INTRSTATUS), RT_INT_RX_ACK);

        hnd = 1;
    }

    if(intr & RT_INT_TX_OK) {
        hnd = 1;
    }

    if(intr & RT_INT_LINK_CHANGE) {
        // Get the MII media status reg.
        uint32 bmsr = g2_read_16(NIC(RT_MII_BMSR));

        // If this is the first time, force a renegotiation.
        if(!link_initial) {
            bmsr &= ~(RT_MII_LINK | RT_MII_AN_COMPLETE);
            dbglog(DBG_INFO, "bba: initial link change, redoing auto-neg\n");
        }

        // This should really be a bit more complete, but this
        // should be sufficient.

        // Is our link there?
        if(bmsr & RT_MII_LINK) {
            // We must have just finished an auto-negotiation.
            dbglog(DBG_INFO, "bba: link stable\n");

            // The link is back.
            link_stable = 1;
        }
        else {
            if(link_initial)
                dbglog(DBG_INFO, "bba: link lost\n");

            // Do an auto-negotiation.
            g2_write_16(NIC(RT_MII_BMCR),
                        RT_MII_RESET |
                        RT_MII_AN_ENABLE |
                        RT_MII_AN_START);

            // The link is gone.
            link_stable = 0;
        }

        // We've done our initial link interrupt now.
        link_initial = 1;

        hnd = 1;
    }

    if(intr & RT_INT_RXBUF_OVERFLOW) {
        dbglog(DBG_KDEBUG, "bba: RX overrun\n");
        rx_reset();
        hnd = 1;
    }

    // DMA complete ? doesn't look like, then what ? anyway, ignore it for now ...
    if(intr == 0) {
        hnd = 1;
    }

    if(!hnd) {
        dbglog(DBG_KDEBUG, "bba: spurious interrupt, status is %08x\n", intr);
    }

    //vid_border_color(0, 0, 0);
}

/****************************************************************************/
/* Netcore interface */

netif_t bba_if;

static void set_ipv6_lladdr() {
    /* Set up the IPv6 link-local address. This is done in accordance with
       Section 4/5 of RFC 2464 based on the MAC Address of the adapter. */
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[0]  = 0xFE;
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[1]  = 0x80;
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[8]  = bba_if.mac_addr[0] ^ 0x02;
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[9]  = bba_if.mac_addr[1];
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[10] = bba_if.mac_addr[2];
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[11] = 0xFF;
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[12] = 0xFE;
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[13] = bba_if.mac_addr[3];
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[14] = bba_if.mac_addr[4];
    bba_if.ip6_lladdr.__s6_addr.__s6_addr8[15] = bba_if.mac_addr[5];
}

/* They only ever made one GAPS peripheral, so this should suffice */
static int bba_if_detect(netif_t *self) {
    (void)self;

    if(bba_if.flags & NETIF_DETECTED)
        return 0;

    if(gaps_detect() < 0)
        return -1;

    bba_if.flags |= NETIF_DETECTED;
    return 0;
}

static int bba_if_init(netif_t *self) {
    (void)self;

    if(bba_if.flags & NETIF_INITIALIZED)
        return 0;

    if(bba_hw_init() < 0)
        return -1;

    memcpy(bba_if.mac_addr, rtl.mac, 6);
    set_ipv6_lladdr();
    bba_if.flags |= NETIF_INITIALIZED;
    return 0;
}

static int bba_if_shutdown(netif_t *self) {
    (void)self;

    if(!(bba_if.flags & NETIF_INITIALIZED))
        return 0;

    bba_hw_shutdown();

    bba_if.flags &= ~(NETIF_INITIALIZED | NETIF_RUNNING);
    return 0;
}

static int bba_if_start(netif_t *self) {
    int i;

    (void)self;

    if(!(bba_if.flags & NETIF_INITIALIZED))
        return -1;

    if(bba_if.flags & NETIF_RUNNING)
        return 0;

    // Start the BBA RX thread.
    assert(bba_rx_thread == NULL);
    sem_init(&bba_rx_sema, 0);
    sem_init(&bba_rx_sema2, 1);
    bba_rx_thread = thd_create(0, bba_rx_threadfunc, 0);
    bba_rx_thread->prio = 1;
    thd_set_label(bba_rx_thread, "BBA-rx-thd");

    /* We need something like this to get DHCP to work (since it doesn't
       know anything about an activated and yet not-yet-receiving network
       adapter =) */
    /* Spin until the link is stabilized */
    i = 1000;

    while(!link_stable && i > 0) {
        i--;
        thd_sleep(10);
    }

    if(!link_stable) {
        dbglog(DBG_ERROR, "bba: timed out waiting for link to stabilize\n");
        return -1;
    }

    bba_if.flags |= NETIF_RUNNING;
    return 0;
}

static int bba_if_stop(netif_t *self) {
    (void)self;

    if(!(bba_if.flags & NETIF_RUNNING))
        return 0;

    /* VP : Shutdown rx thread */
    assert(bba_rx_thread != NULL);
    bba_rx_exit_thread = 1;
    sem_signal(&bba_rx_sema);
    sem_signal(&bba_rx_sema2);
    thd_join(bba_rx_thread, NULL);
    sem_destroy(&bba_rx_sema);
    sem_destroy(&bba_rx_sema2);

    bba_rx_thread = NULL;

    bba_if.flags &= ~NETIF_RUNNING;
    return 0;
}

static int bba_if_tx(netif_t *self, const uint8 *data, int len, int blocking) {
    (void)self;

    if(!(bba_if.flags & NETIF_RUNNING))
        return -1;

    if(bba_tx(data, len, blocking) != BBA_TX_OK)
        return -1;

    return 0;
}

/* We'll auto-commit for now */
static int bba_if_tx_commit(netif_t *self) {
    (void)self;
    return 0;
}

static int bba_if_rx_poll(netif_t *self) {
    int intr;

    (void)self;

    intr = g2_read_16(NIC(RT_INTRSTATUS));

    if(intr & RT_INT_RX_ACK) {
        bba_rx();

        /* so that the irq is not called */
        g2_write_16(NIC(RT_INTRSTATUS), RT_INT_RX_ACK);
    }

    if(rxout != rxin) {
        /* Call the callback to process it */
        eth_rx_callback(rx_pkt[rxout].rxbuff, rx_pkt[rxout].pkt_size);

        rxout = (rxout + 1) % MAX_PKTS;
    }

    return 0;
}

/* Don't need to hook anything here yet */
static int bba_if_set_flags(netif_t *self, uint32 flags_and, uint32 flags_or) {
    (void)self;
    bba_if.flags = (bba_if.flags & flags_and) | flags_or;
    return 0;
}

static int bba_if_set_mc(netif_t *self, const uint8 *list, int count) {
    uint32 old;

    (void)self;

    if(count == 0) {
        /* Clear the multicast address filter */
        g2_write_32(NIC(RT_MAR0 + 0), 0);
        g2_write_32(NIC(RT_MAR0 + 4), 0);

        /* Disable multicast reception */
        old = g2_read_32(NIC(RT_RXCONFIG));
        g2_write_32(NIC(RT_RXCONFIG), old & ~0x00000004);
    }
    else {
        int i, pos;
        uint32 tmp;
        uint32 mar[2] = { 0, 0 };

        /* Go through each entry and add the value to the filter */
        for(i = 0, pos = 0; i < count; ++i, pos += 6) {
            tmp = net_crc32be(list + pos, 6) >> 26;
            mar[tmp >> 5] |= (1 << (tmp & 0x1F));
        }

        /* Set the multicast address filter */
        g2_write_32(NIC(RT_MAR0 + 0), mar[0]);
        g2_write_32(NIC(RT_MAR0 + 4), mar[1]);

        /* Enable multicast reception */
        old = g2_read_32(NIC(RT_RXCONFIG));
        g2_write_32(NIC(RT_RXCONFIG), old | 0x00000004);
    }

    return 0;
}

/* We'll take packets from the interrupt handler and push them into netcore */
static void bba_if_netinput(uint8 *pkt, int pktsize) {
    net_input(&bba_if, pkt, pktsize);
}

/* Set ISP configuration from the flashrom, as long as we're configured staticly */
static void bba_set_ispcfg() {
    flashrom_ispcfg_t isp;
    uint32 fields = FLASHROM_ISP_IP | FLASHROM_ISP_NETMASK |
                    FLASHROM_ISP_BROADCAST | FLASHROM_ISP_GATEWAY;

    if(flashrom_get_ispcfg(&isp) == -1)
        return;

    if((isp.valid_fields & fields) != fields)
        return;

    if(isp.method != FLASHROM_ISP_STATIC)
        return;

    memcpy(bba_if.ip_addr, isp.ip, 4);
    memcpy(bba_if.netmask, isp.nm, 4);
    memcpy(bba_if.gateway, isp.gw, 4);
    memcpy(bba_if.broadcast, isp.bc, 4);
}

/* Initialize */
int bba_init() {
    /* Use the netcore callback */
    bba_set_rx_callback(bba_if_netinput);

#ifdef TX_SEMA
    sem_init(&tx_sema, 1);
#endif

    /* VP : Initialize rx thread */
    // Note: The thread itself is not created here, but when we actually
    // activate the adapter. This way we don't have a spare thread
    // laying around unless it's actually needed.
    bba_rx_thread = NULL;

    /* Setup the structure */
    bba_if.name = "bba";
    bba_if.descr = "Broadband Adapter (HIT-0400)";
    bba_if.index = 0;
    bba_if.dev_id = 0;
    bba_if.flags = NETIF_NO_FLAGS;
    bba_get_mac(bba_if.mac_addr);
    memset(bba_if.ip_addr, 0, sizeof(bba_if.ip_addr));
    memset(bba_if.netmask, 0, sizeof(bba_if.netmask));
    memset(bba_if.gateway, 0, sizeof(bba_if.gateway));
    memset(bba_if.broadcast, 0, sizeof(bba_if.broadcast));
    memset(bba_if.dns, 0, sizeof(bba_if.dns));
    bba_if.mtu = 1500; /* The Ethernet v2 MTU */
    memset(&bba_if.ip6_lladdr, 0, sizeof(bba_if.ip6_lladdr));
    bba_if.ip6_addrs = NULL;
    bba_if.ip6_addr_count = 0;
    memset(&bba_if.ip6_gateway, 0, sizeof(bba_if.ip6_gateway));
    bba_if.mtu6 = 0;
    bba_if.hop_limit = 0;
    bba_if.if_detect = bba_if_detect;
    bba_if.if_init = bba_if_init;
    bba_if.if_shutdown = bba_if_shutdown;
    bba_if.if_start = bba_if_start;
    bba_if.if_stop = bba_if_stop;
    bba_if.if_tx = bba_if_tx;
    bba_if.if_tx_commit = bba_if_tx_commit;
    bba_if.if_rx_poll = bba_if_rx_poll;
    bba_if.if_set_flags = bba_if_set_flags;
    bba_if.if_set_mc = bba_if_set_mc;

    /* Attempt to set up our IP address et al from the flashrom */
    bba_set_ispcfg();

#if 0

    /* Try to detect/init us */
    if(bba_if.if_detect(&bba_if) < 0) {
        printf("net_bba: can't detect broadband adapter\n");
        return -1;
    }

    if(bba_if.if_init(&bba_if) < 0) {
        printf("net_bba: can't init broadband adapter\n");
        return -1;
    }

    if(bba_if.if_start(&bba_if) < 0) {
        printf("net_bba: can't start broadband adapter\n");
        return -1;
    }

#endif

    /* Append it to the chain */
    return net_reg_device(&bba_if);
}

/* Shutdown */
int bba_shutdown() {
#if 1
    /* Unregister us (if neccessary) */
    net_unreg_device(&bba_if);
#endif

    /* Shutdown hardware */
    bba_if.if_stop(&bba_if);
    bba_if.if_shutdown(&bba_if);

#ifdef TX_SEMA
    sem_destroy(&tx_sema);
#endif

    return 0;
}


#if 0
int module_init(int argc, char **argv) {
    printf("net_bba: initializing\n");

    if(bba_init() < 0)
        return -1;

    printf("net_bba: done initializing\n");
    return 0;
}

int module_shutdown() {
    printf("net_bba: exiting\n");

    if(bba_shutdown() < 0)
        return -1;

    return 0;
}

#include <sys/module.h>
int main(int argc, char **argv) {
    return kos_do_module(module_init, module_shutdown, argc, argv);
}
#endif
