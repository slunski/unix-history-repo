/**************************************************************************
**
**  $Id: pci.c,v 2.12 94/10/11 22:20:37 wolf Oct11 $
**
**  General subroutines for the PCI bus on 80*86 systems.
**  pci_configure ()
**
**  386bsd / FreeBSD
**
**-------------------------------------------------------------------------
**
** Copyright (c) 1994 Wolfgang Stanglmeier.  All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
***************************************************************************
*/

#include <pci.h>
#if NPCI > 0

#ifndef __FreeBSD2__
#if __FreeBSD__ >= 2
#define	__FreeBSD2__
#endif
#endif

/*========================================================
**
**	#includes  and  declarations
**
**========================================================
*/

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/errno.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

#include <i386/isa/isa.h>
#include <i386/isa/isa_device.h>
#include <i386/isa/icu.h>
#include <i386/pci/pcireg.h>

/*
**	Function prototypes missing in system headers
*/

#ifndef __FreeBSD2__
extern	pmap_t pmap_kernel(void);
static	vm_offset_t pmap_mapdev (vm_offset_t paddr, vm_size_t vsize);

/*
 * Type of the first (asm) part of an interrupt handler.
 */
typedef void inthand_t __P((u_int cs, u_int ef, u_int esp, u_int ss));

/*
 * Usual type of the second (C) part of an interrupt handler.  Some bogus
 * ones need the arg to be the interrupt frame (and not a copy of it, which
 * is all that is possible in C).
 */
typedef void inthand2_t __P((int unit));

/*
**	XXX	@FreeBSD2@
**
**	Unfortunately, the mptr argument is _no_ pointer in 2.0 FreeBSD.
**	We would prefer a pointer because it enables us to install
**	new interrupt handlers at any time.
**	In 2.0 FreeBSD later installed interrupt handlers may change
**	the xyz_imask, but this would not be recognized by handlers
**	which are installed before.
*/

static int
register_intr __P((int intr, int device_id, unsigned int flags,
		       inthand2_t *handler, unsigned int * mptr, int unit));
extern unsigned intr_mask[ICU_LEN];

#endif /* !__FreeBSD2__ */

/*========================================================
**
**	Autoconfiguration of pci devices.
**
**	This is reverse to the isa configuration.
**	(1) find a pci device.
**	(2) look for a driver.
**
**========================================================
*/

/*--------------------------------------------------------
**
**	The pci devices can be mapped to any address.
**	As default we start at the last gigabyte.
**
**--------------------------------------------------------
*/

#ifndef PCI_PMEM_START
#define PCI_PMEM_START 0xc0000000
#endif

static	vm_offset_t pci_paddr = PCI_PMEM_START;

/*--------------------------------------------------------
**
**	The pci device interrupt lines should have been
**	assigned by the bios. But if the bios failed to
**	to it, we set it.
**
**--------------------------------------------------------
*/

#ifndef PCI_IRQ
#define PCI_IRQ 0
#endif

static	u_long      pci_irq   = PCI_IRQ;

/*---------------------------------------------------------
**
**	pci_configure ()
**
**	Probe all devices on pci bus and attach them.
**
**	May be called more than once.
**	Any device is attached only once.
**	(Attached devices are remembered in pci_seen.)
**
**---------------------------------------------------------
*/

static void not_supported (pcici_t tag, u_long type);

static unsigned long pci_seen[NPCI];

static int pci_conf_count;

void pci_configure()
{
	u_char	device,last_device;
	u_short	bus;
	pcici_t	tag;
	pcidi_t type;
	u_long	data;
	int	unit;
	int	pci_mechanism;
	int	pciint;
	int	irq;
	char*	name=0;
	int	newdev=0;

	struct pci_driver *drp=0;
	struct pci_device *dvp;

	/*
	**	check pci bus present
	*/

	pci_mechanism = pci_conf_mode ();
	if (!pci_mechanism) return;
	last_device = pci_mechanism==1 ? 31 : 15;

	/*
	**	hello world ..
	*/


	for (bus=0;bus<NPCI;bus++) {
#ifndef PCI_QUIET
	    printf ("pci%d: scanning device 0..%d, mechanism=%d.\n",
		bus, last_device, pci_mechanism);
#endif
	    for (device=0; device<=last_device; device ++) {

		if (pci_seen[bus] & (1ul << device))
			continue;

		tag = pcitag (bus, device, 0);
		type = pci_conf_read (tag, PCI_ID_REG);

		if ((!type) || (type==0xfffffffful)) continue;

		/*
		**	lookup device in ioconfiguration:
		*/

		for (dvp = pci_devtab; dvp->pd_name; dvp++) {
			drp = dvp->pd_driver;
			if (!drp)
				continue;
			if ((name=(*drp->probe)(tag, type)))
				break;
		};

		if (!dvp->pd_name) {
#ifndef PCI_QUIET
			if (pci_conf_count)
				continue;
			printf("pci%d:%d: ", bus, device);
			not_supported (tag, type);
#endif
			continue;
		};

		pci_seen[bus] |= (1ul << device);
		/*
		**	Get and increment the unit.
		*/

		unit = (*drp->count)++;

		/*
		**	ignore device ?
		*/

		if (!*name) continue;

		/*
		**	Announce this device
		*/

		newdev++;
		printf ("%s%d <%s>", dvp->pd_name, unit, name);

		/*
		**	Get the int pin number (pci interrupt number a-d)
		**	from the pci configuration space.
		*/

		data = pci_conf_read (tag, PCI_INTERRUPT_REG);
		pciint = PCI_INTERRUPT_PIN_EXTRACT(data);

		if (pciint) {

			printf (" int %c", 0x60+pciint);

			/*
			**	If the interrupt line register is not set,
			**	set it now from PCI_IRQ.
			*/

			if (!(PCI_INTERRUPT_LINE_EXTRACT(data))) {

				irq = pci_irq & 0x0f;
				pci_irq >>= 4;

				data = PCI_INTERRUPT_LINE_INSERT(data, irq);
				printf (" (config)");
				pci_conf_write (tag, PCI_INTERRUPT_REG, data);
			};

			irq = PCI_INTERRUPT_LINE_EXTRACT(data);

			/*
			**	If it's zero, the isa irq number is unknown,
			**	and we cannot bind the pci interrupt to isa.
			*/

			if (irq)
				printf (" irq %d", irq);
			else
				printf (" not bound");
		};

		/*
		**	enable memory access
		*/

		data = (pci_conf_read (tag, PCI_COMMAND_STATUS_REG)
			& 0xffff) | PCI_COMMAND_MEM_ENABLE;

		pci_conf_write (tag, (u_char) PCI_COMMAND_STATUS_REG, data);

		/*
		**	attach device
		**	may produce additional log messages,
		**	i.e. when installing subdevices.
		*/

		printf (" on pci%d:%d\n", bus, device);

		(*drp->attach) (tag, unit);
	    };
	};

#ifndef PCI_QUIET
	if (newdev)
		printf ("pci uses physical addresses from 0x%lx to 0x%lx\n",
			(u_long)PCI_PMEM_START, (u_long)pci_paddr);
#endif
	pci_conf_count++;
}

/*-----------------------------------------------------------------------
**
**	Map device into port space.
**
**	PCI-Specification:  6.2.5.1: address maps
**
**-----------------------------------------------------------------------
*/

int pci_map_port (pcici_t tag, u_long reg, u_short* pa)
{
	/*
	**	@MAPIO@ not yet implemented.
	*/
	printf ("pci_map_port failed: not yet implemented\n");
	return (0);
}

/*-----------------------------------------------------------------------
**
**	Map device into virtual and physical space
**
**	PCI-Specification:  6.2.5.1: address maps
**
**-----------------------------------------------------------------------
*/

int pci_map_mem (pcici_t tag, u_long reg, vm_offset_t* va, vm_offset_t* pa)
{
	u_long data;
	vm_size_t vsize;
	vm_offset_t vaddr;

	/*
	**	sanity check
	*/

        if (reg < PCI_MAP_REG_START || reg >= PCI_MAP_REG_END || (reg & 3)) {
        	printf ("pci_map_mem failed: bad register=0x%x\n",
               		(unsigned)reg);
                return (0);
	};

	/*
	**	get size and type of memory
	**
	**	type is in the lowest four bits.
	**	If device requires 2^n bytes, the next
	**	n-4 bits are read as 0.
	*/

	pci_conf_write (tag, reg, 0xfffffffful);
	data = pci_conf_read (tag, reg);

	switch (data & 0x0f) {

	case PCI_MAP_MEMORY_TYPE_32BIT:	/* 32 bit non cachable */
		break;

	default:	/* unknown */
                printf ("pci_map_mem failed: bad memory type=0x%x\n",
               		(unsigned) data);
                return (0);
	};

	/*
	**	mask out the type,
	**	and round up to a page size
	*/

	vsize = round_page (-(data &  PCI_MAP_MEMORY_ADDRESS_MASK));

	if (!vsize) return (0);

	/*
	**	align physical address to virtual size
	*/

	if ((data = pci_paddr % vsize))
		pci_paddr += vsize - data;

	vaddr = (vm_offset_t) pmap_mapdev (pci_paddr, vsize);
	

	if (!vaddr) return (0);

#ifndef PCI_QUIET
	/*
	**	display values.
	*/

	printf ("\treg%d: virtual=0x%lx physical=0x%lx\n",
		(unsigned) reg, (u_long)vaddr, (u_long)pci_paddr);
#endif

	/*
	**	return them to the driver
	*/

	*va = vaddr;
	*pa = pci_paddr;

	/*
	**	set device address
	*/

	pci_conf_write (tag, reg, pci_paddr);

	/*
	**	and don't forget to increment pci_paddr
	*/

	pci_paddr += vsize;

	return (1);
}

/*-----------------------------------------------------------------------
**
**	Map pci interrupts to isa interrupts.
**
**-----------------------------------------------------------------------
*/

static	unsigned int	pci_int_mask [16];

int pci_map_int (pcici_t tag, int(*func)(), void* arg, unsigned* maskptr)
{
	int irq;
	unsigned mask;

	irq = PCI_INTERRUPT_LINE_EXTRACT(
		pci_conf_read (tag, PCI_INTERRUPT_REG));

	if (irq >= 16 || irq <= 0) {
		printf ("pci_map_int failed: no int line set.\n");
		return (0);
	}

	mask = 1ul << irq;

	if (!maskptr)
		maskptr = &pci_int_mask[irq];

	INTRMASK (*maskptr, mask);

	register_intr(
		irq,		/* isa irq	*/
		0,		/* deviced??	*/
		0,		/* flags?	*/
		(inthand2_t*) func, /* handler	*/
#ifdef __FreeBSD2__
		*maskptr,	/* mask		*/
#else
		maskptr,	/* mask pointer	*/
#endif
		(int) arg);	/* handler arg	*/

#ifdef __FreeBSD2__
	/*
	**	XXX See comment at beginning of file.
	**
	**	Have to update all the interrupt masks ... Grrrrr!!!
	*/
	{
		unsigned * mp = &intr_mask[0];
		/*
		**	update the isa interrupt masks.
		*/
		for (mp=&intr_mask[0]; mp<&intr_mask[ICU_LEN]; mp++)
			if (*mp & *maskptr)
				*mp |= mask;
		/*
		**	update the pci interrupt masks.
		*/
		for (mp=&pci_int_mask[0]; mp<&pci_int_mask[16]; mp++)
			if (*mp & *maskptr)
				*mp |= mask;
	};
#endif

	INTREN (mask);

	return (1);
}

/*-----------------------------------------------------------
**
**	Display of unknown devices.
**
**-----------------------------------------------------------
*/
struct vt {
	u_short	ident;
	char*	name;
};

static struct vt VendorTable[] = {
	{0x1002, "ATI TECHNOLOGIES INC"},
	{0x1011, "DIGITAL EQUIPMENT CORPORATION"},
	{0x101A, "NCR"},
	{0x102B, "MATROX"},
	{0x1045, "OPTI"},
	{0x5333, "S3 INC."},
	{0x8086, "INTEL CORPORATION"},
	{0,0}
};

static const char *const majclasses[] = {
	"old", "storage", "network", "display",
	"multimedia", "memory", "bridge" 
};

void not_supported (pcici_t tag, u_long type)
{
	u_char	reg;
	u_long	data;
	struct vt * vp;

	/*
	**	lookup the names.
	*/

	for (vp=VendorTable; vp->ident; vp++)
		if (vp->ident == (type & 0xffff))
			break;

	/*
	**	and display them.
	*/

	if (vp->ident) printf (vp->name);
		else   printf ("vendor=0x%lx", type & 0xffff);

	printf (", device=0x%lx", type >> 16);

	data = (pci_conf_read(tag, PCI_CLASS_REG) >> 24) & 0xff;
	if (data < sizeof(majclasses) / sizeof(majclasses[0]))
		printf(", class=%s", majclasses[data]);

	printf (" [not supported]\n");

	for (reg=PCI_MAP_REG_START; reg<PCI_MAP_REG_END; reg+=4) {
		data = pci_conf_read (tag, reg);
		if (!data) continue;
		switch (data&7) {

		case 1:
		case 5:
			printf ("	map(%x): io(%lx)\n",
				reg, data & ~3);
			break;
		case 0:
			printf ("	map(%x): mem32(%lx)\n",
				reg, data & ~7);
			break;
		case 2:
			printf ("	map(%x): mem20(%lx)\n",
				reg, data & ~7);
			break;
		case 4:
			printf ("	map(%x): mem64(%lx)\n",
				reg, data & ~7);
			break;
		}
	}
}

#ifndef __FreeBSD2__
/*-----------------------------------------------------------
**
**	Mapping of physical to virtual memory
**
**-----------------------------------------------------------
*/

extern vm_map_t kernel_map;

static	vm_offset_t pmap_mapdev (vm_offset_t paddr, vm_size_t vsize)
{
	vm_offset_t	vaddr,value;
	u_long		result;

	vaddr  = vm_map_min (kernel_map);

	result = vm_map_find (kernel_map, (void*)0, (vm_offset_t) 0,
				&vaddr, vsize, TRUE);

	if (result != KERN_SUCCESS) {
		printf (" vm_map_find failed(%d)\n", result);
		return (0);
	};

	/*
	**	map physical
	*/

	value = vaddr;
	while (vsize >= NBPG) {
		pmap_enter (pmap_kernel(), vaddr, paddr,
				VM_PROT_READ|VM_PROT_WRITE, TRUE);
		vaddr += NBPG;
		paddr += NBPG;
		vsize -= NBPG;
	};
	return (value);
}

/*------------------------------------------------------------
**
**	Emulate the register_intr() function of FreeBSD 2.0
**
**	requires a patch:
**	FreeBSD 2.0:	"/sys/i386/isa/vector.s"
**	386bsd0.1:	"/sys/i386/isa/icu.s"
**	386bsd1.0:	Please ask Jesus Monroy Jr.
**
**------------------------------------------------------------
*/

#include <machine/segments.h>

int		pci_int_unit [16];
inthand2_t*    (pci_int_hdlr [16]);
unsigned int *	pci_int_mptr [16];
unsigned int	pci_int_count[16];

extern void
	Vpci3(), Vpci4(), Vpci5(), Vpci6(), Vpci7(), Vpci8(), Vpci9(),
	Vpci10(), Vpci11(), Vpci12(), Vpci13(), Vpci14(), Vpci15();

static inthand_t* pci_int_glue[16] = {
	0, 0, 0, Vpci3, Vpci4, Vpci5, Vpci6, Vpci7, Vpci8,
	Vpci9, Vpci10, Vpci11, Vpci12, Vpci13, Vpci14, Vpci15 };

static int
register_intr __P((int intr, int device_id, unsigned int flags,
		       inthand2_t *handler, unsigned int* mptr, int unit))
{
	if (intr >= 16 || intr <= 2)
		return (EINVAL);
	if (pci_int_hdlr [intr])
		return (EBUSY);

	pci_int_hdlr [intr] = handler;
	pci_int_unit [intr] = unit;
	pci_int_mptr [intr] = mptr;

	setidt(NRSVIDT + intr, pci_int_glue[intr], SDT_SYS386IGT, SEL_KPL);
	return (0);
}
#endif /* __FreeBSD2__ */
#endif /* NPCI */
