/*-
 * Copyright (c) 1998,1999 S�ren Schmidt
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification, immediately at the beginning of the file.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *	$Id: ata-all.h,v 1.3 1999/03/05 09:43:30 sos Exp $
 */

/* ATA register defines */
#define ATA_DATA			0x00	/* data register */
#define	ATA_ERROR			0x01	/* (R) error register */
#define ATA_PRECOMP			0x01	/* (W) precompensation */
#define ATA_COUNT			0x02	/* sector count */
#define		ATA_I_CMD		0x01	/* cmd (1) | data (0) */
#define		ATA_I_IN		0x02	/* read (1) | write (0) */
#define		ATA_I_RELEASE		0x04	/* released bus (1) */

#define	ATA_SECTOR			0x03	/* sector # */
#define	ATA_CYL_LSB			0x04	/* cylinder# LSB */
#define	ATA_CYL_MSB			0x05	/* cylinder# MSB */
#define ATA_DRIVE			0x06	/* Sector/Drive/Head register */
#define		ATA_D_IBM		0xa0	/* 512 byte sectors, ECC */

#define ATA_CMD				0x07	/* command register */
#define		ATA_C_ATA_IDENTIFY	0xec	/* get ATA params */
#define		ATA_C_ATAPI_IDENTIFY	0xa1	/* get ATAPI params*/
#define		ATA_C_READ		0x20	/* read command */
#define		ATA_C_WRITE		0x30	/* write command */
#define		ATA_C_READ_MULTI	0xc4	/* read multi command */
#define		ATA_C_WRITE_MULTI	0xc5	/* write multi command */
#define		ATA_C_SET_MULTI		0xc6	/* set multi size command */
#define		ATA_C_PACKET_CMD	0xa0	/* set multi size command */

#define ATA_STATUS			0x07	/* status register */
#define		ATA_S_ERROR		0x01	/* error */
#define		ATA_S_INDEX		0x02	/* index */
#define		ATA_S_CORR		0x04	/* data corrected */
#define		ATA_S_DRQ		0x08	/* data request */
#define		ATA_S_DSC		0x10	/* drive Seek Completed */
#define		ATA_S_DWF		0x20	/* drive write fault */
#define		ATA_S_DRDY		0x40	/* drive ready */
#define		ATA_S_BSY		0x80	/* busy */

#define ATA_ALTPORT			0x206	/* alternate Status register */
#define 	ATA_A_IDS		0x02	/* disable interrupts */
#define		ATA_A_RESET		0x04	/* RESET controller */
#define 	ATA_A_4BIT		0x08	/* 4 head bits */

/* Misc defines */
#define	ATA_MASTER			0x00
#define	ATA_SLAVE			0x10
#define	ATA_IOSIZE			0x08
#define ATA_OP_FINISHED			0x00
#define ATA_OP_CONTINUES		0x01

/* Devices types */
#define ATA_ATA_MASTER			0x01
#define ATA_ATA_SLAVE			0x02
#define ATA_ATAPI_MASTER		0x04
#define ATA_ATAPI_SLAVE			0x08

/* Structure describing an ATA device */
struct ata_softc {
    u_int32_t			unit;		/* this instance's number */
    u_int32_t			ioaddr;		/* port addr */
    u_int32_t			altioaddr;	/* alternate port addr */
    void    			*dmacookie;	/* handle for DMA services */
    int32_t			flags;		/* controller flags */
#define		ATA_F_SLAVE_ONLY	0x0001

    int32_t			devices;	/* what is present */
    u_int8_t			status;		/* last controller status */
    u_int8_t			error;		/* last controller error */
    int32_t			active;		/* active processing request */
#define		ATA_IDLE		0x0
#define		ATA_IMMEDIATE		0x0
#define		ATA_WAIT_INTR		0x1
#define		ATA_IGNORE_INTR		0x2
#define		ATA_ACTIVE_ATA		0x3
#define		ATA_ACTIVE_ATAPI	0x4

    struct buf_queue_head       ata_queue;      /* head of ATA queue */
    TAILQ_HEAD(, atapi_request) atapi_queue;    /* head of ATAPI queue */
};

#define MAXATA	8

extern struct ata_softc *atadevices[];
 
/* public prototypes */
void ata_start(struct ata_softc *);
int32_t ata_wait(struct ata_softc *, u_int8_t);
int32_t ata_command(struct ata_softc *, int32_t, u_int32_t, u_int32_t, u_int32_t, u_int32_t, u_int32_t, int32_t);
void bswap(int8_t *, int32_t);
void btrim(int8_t *, int32_t);
void bpack(int8_t *, int8_t *, int32_t);

