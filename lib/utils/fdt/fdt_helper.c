// SPDX-License-Identifier: BSD-2-Clause
/*
 * fdt_helper.c - Flat Device Tree manipulation helper routines
 * Implement helper routines on top of libfdt for OpenSBI usage
 *
 * Copyright (C) 2020 Bin Meng <bmeng.cn@gmail.com>
 */

#include <libfdt.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi_utils/fdt/fdt_helper.h>

void fdt_cpu_fixup(void *fdt)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	char cpu_node[32] = "";
	int cpu_offset;
	int err;
	u32 i;

	err = fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + 32);
	if (err < 0)
		return;

	/* assume hart ids are continuous */
	for (i = 0; i < sbi_platform_hart_count(plat); i++) {
		sbi_sprintf(cpu_node, "/cpus/cpu@%d", i);
		cpu_offset = fdt_path_offset(fdt, cpu_node);

		if (sbi_platform_hart_invalid(plat, i))
			fdt_setprop_string(fdt, cpu_offset, "status",
					   "disabled");

		memset(cpu_node, 0, sizeof(cpu_node));
	}
}

void fdt_plic_fixup(void *fdt, const char *compat)
{
	u32 *cells;
	int i, cells_count;
	int plic_off;

	plic_off = fdt_node_offset_by_compatible(fdt, 0, compat);
	if (plic_off < 0)
		return;

	cells = (u32 *)fdt_getprop(fdt, plic_off,
				   "interrupts-extended", &cells_count);
	if (!cells)
		return;

	cells_count = cells_count / sizeof(u32);
	if (!cells_count)
		return;

	for (i = 0; i < (cells_count / 2); i++) {
		if (fdt32_to_cpu(cells[2 * i + 1]) == IRQ_M_EXT)
			cells[2 * i + 1] = cpu_to_fdt32(0xffffffff);
	}
}

/**
 * We use PMP to protect OpenSBI firmware to safe-guard it from buggy S-mode
 * software, see pmp_init() in lib/sbi/sbi_hart.c. The protected memory region
 * information needs to be conveyed to S-mode software (e.g.: operating system)
 * via some well-known method.
 *
 * With device tree, this can be done by inserting a child node of the reserved
 * memory node which is used to specify one or more regions of reserved memory.
 *
 * For the reserved memory node bindings, see Linux kernel documentation at
 * Documentation/devicetree/bindings/reserved-memory/reserved-memory.txt
 *
 * Some additional memory spaces may be protected by platform codes via PMP as
 * well, and corresponding child nodes will be inserted.
 */
int fdt_reserved_memory_fixup(void *fdt)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	unsigned long prot, addr, size;
	int na = fdt_address_cells(fdt, 0);
	int ns = fdt_size_cells(fdt, 0);
	fdt32_t addr_high, addr_low;
	fdt32_t size_high, size_low;
	fdt32_t reg[4];
	fdt32_t *val;
	char name[32];
	int parent, subnode;
	int i, j;
	int err;

	if (!sbi_platform_has_pmp(plat))
		return 0;

	/* expand the device tree to accommodate new node */
	err  = fdt_open_into(fdt, fdt, fdt_totalsize(fdt) + 256);
	if (err < 0)
		return err;

	/* try to locate the reserved memory node */
	parent = fdt_path_offset(fdt, "/reserved-memory");
	if (parent < 0) {
		/* if such node does not exist, create one */
		parent = fdt_add_subnode(fdt, 0, "reserved-memory");
		if (parent < 0)
			return parent;

		/*
		 * reserved-memory node has 3 required properties:
		 * - #address-cells: the same value as the root node
		 * - #size-cells: the same value as the root node
		 * - ranges: should be empty
		 */

		err = fdt_setprop_empty(fdt, parent, "ranges");
		if (err < 0)
			return err;

		err = fdt_setprop_u32(fdt, parent, "#size-cells", ns);
		if (err < 0)
			return err;

		err = fdt_setprop_u32(fdt, parent, "#address-cells", na);
		if (err < 0)
			return err;
	}

	/*
	 * We assume the given device tree does not contain any memory region
	 * child node protected by PMP. Normally PMP programming happens at
	 * M-mode firmware. The memory space used by OpenSBI is protected.
	 * Some additional memory spaces may be protected by platform codes.
	 *
	 * With above assumption, we create child nodes directly.
	 */

	for (i = 0, j = 0; i < PMP_COUNT; i++) {
		pmp_get(i, &prot, &addr, &size);
		if (!(prot & PMP_A))
			continue;
		if (!(prot & (PMP_R | PMP_W | PMP_X))) {
			addr_high = (u64)addr >> 32;
			addr_low = addr;
			size_high = (u64)size >> 32;
			size_low = size;

			if (na > 1 && addr_high)
				sbi_snprintf(name, sizeof(name),
					     "mmode_pmp%d@%x,%x", j,
					     addr_high, addr_low);
			else
				sbi_snprintf(name, sizeof(name),
					     "mmode_pmp%d@%x", j,
					     addr_low);

			subnode = fdt_add_subnode(fdt, parent, name);
			if (subnode < 0)
				return subnode;

			/*
			 * Tell operating system not to create a virtual
			 * mapping of the region as part of its standard
			 * mapping of system memory.
			 */
			err = fdt_setprop_empty(fdt, subnode, "no-map");
			if (err < 0)
				return err;

			/* encode the <reg> property value */
			val = reg;
			if (na > 1)
				*val++ = cpu_to_fdt32(addr_high);
			*val++ = cpu_to_fdt32(addr_low);
			if (ns > 1)
				*val++ = cpu_to_fdt32(size_high);
			*val++ = cpu_to_fdt32(size_low);

			err = fdt_setprop(fdt, subnode, "reg", reg,
					  (na + ns) * sizeof(fdt32_t));
			if (err < 0)
				return err;

			j++;
		}
	}

	return 0;
}

void fdt_fixups(void *fdt)
{
	fdt_plic_fixup(fdt, "riscv,plic0");

	fdt_reserved_memory_fixup(fdt);
}

static int fdt_get_node_addr_size(void *fdt, int node, unsigned long *addr,
				  unsigned long *size)
{
	int parent, len, i;
	int cell_addr, cell_size;
	const fdt32_t *prop_addr, *prop_size;
	uint64_t temp = 0;

	parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return parent;
	cell_addr = fdt_address_cells(fdt, parent);
	if (cell_addr < 1)
		return SBI_ENODEV;

	cell_size = fdt_size_cells(fdt, parent);
	if (cell_size < 0)
		return SBI_ENODEV;

	prop_addr = fdt_getprop(fdt, node, "reg", &len);
	if (!prop_addr)
		return SBI_ENODEV;
	prop_size = prop_addr + cell_addr;

	if (addr) {
		for (i = 0; i < cell_addr; i++)
			temp = (temp << 32) | fdt32_to_cpu(*prop_addr++);
		*addr = temp;
	}
	temp = 0;

	if (size) {
		for (i = 0; i < cell_size; i++)
			temp = (temp << 32) | fdt32_to_cpu(*prop_size++);
		*size = temp;
	}

	return 0;
}

int fdt_parse_uart8250(void *fdt, struct platform_uart_data *uart,
		   const char *compatible)
{
	int nodeoffset, len, rc;
	fdt32_t *val;
	unsigned long reg_addr, reg_size;

	/**
	 * TODO: We don't know how to handle multiple nodes with the same
	 * compatible sring. Just return the first node for now.
	 */

	nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (nodeoffset < 0)
		return nodeoffset;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, &reg_addr, &reg_size);
	if (rc < 0 || !reg_addr || !reg_size)
		return SBI_ENODEV;
	uart->addr = reg_addr;

	/**
	 * UART address is mandaotry. clock-frequency and current-speed may not
	 * be present. Don't return error.
	 */
	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "clock-frequency", &len);
	if (len > 0 && val)
		uart->freq = fdt32_to_cpu(*val);

	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "current-speed", &len);
	if (len > 0 && val)
		uart->baud = fdt32_to_cpu(*val);

	return 0;
}

int fdt_parse_plic(void *fdt, struct platform_plic_data *plic,
		   const char *compatible)
{
	int nodeoffset, len, rc;
	const fdt32_t *val;
	unsigned long reg_addr, reg_size;

	nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (nodeoffset < 0)
		return nodeoffset;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, &reg_addr, &reg_size);
	if (rc < 0 || !reg_addr || !reg_size)
		return SBI_ENODEV;
	plic->addr = reg_addr;

	val = fdt_getprop(fdt, nodeoffset, "riscv,ndev", &len);
	if (len > 0)
		plic->num_src = fdt32_to_cpu(*val);

	return 0;
}

int fdt_parse_clint(void *fdt, unsigned long *clint_addr,
		    const char *compatible)
{
	int nodeoffset, rc;

	nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (nodeoffset < 0)
		return nodeoffset;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, clint_addr, NULL);
	if (rc < 0 || !clint_addr)
		return SBI_ENODEV;

	return 0;
}