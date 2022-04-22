


#include <string.h>
#include <inttypes.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_sc.h>

static int64_t sccck(uint64_t va, uint64_t v) {
	int64_t i;
	asm volatile ("sccck %0, %1, %2\n"
				 : "=r" (i)
				 : "r"  (va), "r" (v));
	return i;
}

static uint64_t *scca(int64_t ci) {
	uint64_t *p;
	asm volatile ("scca %0, %1\n"
				 : "=r" (p)
				 : "r"  (ci));
	return p;
}

static uint8_t  *scpa(int64_t ci, uint64_t sd) {
	uint8_t  *p;
	asm volatile ("scpa %0, %1, %2\n"
				 : "=r" (p)
				 : "r"  (ci), "r" (sd));
	return p;
}

static uint32_t *scga(int64_t ci, uint64_t sd) {
	uint32_t *p;
	asm volatile ("scga %0, %1, %2\n"
				 : "=r" (p)
				 : "r"  (ci), "r" (sd));
	return p;
}

static inline int64_t find_cell(uint64_t *desc, uint64_t addr, uint64_t v) {
	int64_t i = sccck(addr, v);
	if (i > 0)
		memcpy(desc, scca(i), 2 * sizeof(uint64_t));
	return i;
}

static inline int _emulate_scprot(uint64_t addr, uint8_t perm, 
														struct sbi_trap_regs *regs, 
														unsigned long *tcause, 
														unsigned long *tval) {
	int64_t ci = sccck(addr, 1);

	/* ChecK: valid address */
	if(unlikely(ci < 0)) {
		*tcause = RISCV_EXCP_SECCELL_ILL_ADDR;
		*tval = addr;
		return -1;
	} else if(unlikely(ci == 0)) {
		/* Check: Valid cell */
		*tcause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
		*tval = 0;
		return -1;
	}

	uint8_t *perms = scpa(ci, 0);
	uint8_t existing_perms = *perms;
  if(unlikely(perm & ~RT_PERMS)) {
		*tcause = RISCV_EXCP_SECCELL_ILL_PERM;
		*tval = (0 << 8) | (uint8_t)perm;
		return -1;
  } else if(unlikely(perm & ~existing_perms)) {
		*tcause = RISCV_EXCP_SECCELL_ILL_PERM;
		*tval = (2 << 8) | (uint8_t)perm;
		return -1;
	}

	*perms = (existing_perms & (~RT_PERMS)) | (perm & RT_PERMS);
	__asm__ __volatile("sfence.vma");

	return 0;
}

int emulate_scprot(ulong insn, struct sbi_trap_regs *regs) {
	uint64_t addr;
	uint8_t perm;
	struct sbi_trap_info trap;

	addr = GET_RS1(insn, regs);
	perm = GET_RS2(insn, regs);

	if(unlikely(_emulate_scprot(addr, perm, regs, &trap.cause, &trap.tval))) {
		trap.epc = regs->mepc;
		trap.tval2 = 0;
		trap.tinst = 0;
    return sbi_trap_redirect(regs, &trap);
	}

	regs->mepc += 4;
	return 0;
}

int emulate_scinval(ulong insn, struct sbi_trap_regs *regs) {
	int64_t ci;
	struct sbi_trap_info trap;
	uint64_t sd, addr, usid;
	uint8_t *ptable;
	uint32_t M;

	addr = GET_RS1(insn, regs);
	usid = csr_read(CSR_USID);

	ptable = (uint8_t *)(uintptr_t)((csr_read(CSR_SATP) & SATP64_PPN) << 12);
	M = ((uint32_t *)ptable)[2];

	ci = sccck(addr, 1);

	/* ChecK: valid address */
	if (unlikely(ci < 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_ADDR;
		trap.tval = addr;
		return sbi_trap_redirect(regs, &trap);
	} else if (unlikely(ci == 0)) {
	/* Check: Already inValid cell */
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
		trap.tval = 0;
		return sbi_trap_redirect(regs, &trap);
	}

	for(sd = 1; sd < M; sd++) {
		if(sd != usid) {
			uint8_t pperm = *scpa(ci, sd);
			if(unlikely((pperm & RT_PERMS) != 0)) {
				trap.epc = regs->mepc;
				trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
				trap.tval = 2;
				return sbi_trap_redirect(regs, &trap);
			}

			uint32_t gperm = *scga(ci, sd);
			if(unlikely(gperm != G(SDINV, 0))) {
				trap.epc = regs->mepc;
				trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
				trap.tval = 3;
				return sbi_trap_redirect(regs, &trap);
			}
		}
	}

	*scpa(ci, 0) = RT_D | RT_A;
	*scpa(ci, (uint64_t)(-1l)) &= ~RT_V;

	scca(ci)[1] &= ~(1ul << 63);
	__asm__ __volatile("sfence.vma");

	regs->mepc += 4;
	return 0;
}

int emulate_screval(ulong insn, struct sbi_trap_regs *regs) {
	int64_t ci;
	struct sbi_trap_info trap;
	uint64_t addr, perm;

	addr = GET_RS1(insn, regs);
	perm = GET_RS2(insn, regs);

  if(unlikely(perm & ~RT_PERMS)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (0 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
	} else if (unlikely(!perm)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (1 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
  }

	ci = sccck(addr, 0);
	/* ChecK: valid address */
	if (unlikely(ci < 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_ADDR;
		trap.tval = addr;
		return sbi_trap_redirect(regs, &trap);
	} else if (unlikely(ci == 0)) { 
		trap.epc = regs->mepc;
		/* Check: Already Valid cell */
	  trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
		trap.tval = 1;
		return sbi_trap_redirect(regs, &trap);
	}

	*scpa(ci, 0) = RT_D | RT_A | perm | RT_V;
	*scpa(ci, (uint64_t)(-1l)) |= RT_V;

	scca(ci)[1] |= (1ul << 63);
	__asm__ __volatile("sfence.vma");

	regs->mepc += 4;
	return 0;
}

int emulate_scgrant(ulong insn, struct sbi_trap_regs *regs) {
	int64_t ci;
	uint64_t addr, sdtgt;
	uint8_t *ptable, perm, existing_perms;
	uint32_t M;
	struct sbi_trap_info trap;

	addr = GET_RS1(insn, regs);
	ci = sccck(addr, 1);
	/* ChecK: valid address */
	if (unlikely(ci < 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_ADDR;
		trap.tval = addr;
		return sbi_trap_redirect(regs, &trap);
	} else if (unlikely(ci == 0)) {
	/* Check: For Valid cell */
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
		trap.tval = 0;
		return sbi_trap_redirect(regs, &trap);
	}

	sdtgt = GET_RS2(insn, regs);
	ptable = (uint8_t *)(uintptr_t)((csr_read(CSR_SATP) & SATP64_PPN) << 12);
	M = ((uint32_t *)ptable)[2];
	if(unlikely((sdtgt == 0) || (sdtgt > M))) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_INV_SDID;
		trap.tval = sdtgt;
		return sbi_trap_redirect(regs, &trap);
	}

	perm = IMM_S(insn);
	if(unlikely(perm & ~RT_PERMS)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (0 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
  } else if(unlikely((perm & RT_PERMS) == 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (1 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
	}
	existing_perms = *scpa(ci, 0);
	if(unlikely(perm & ~existing_perms)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (2 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
	}

	*scga(ci, 0) = G(sdtgt, perm);
	__asm__ __volatile("sfence.vma");

	regs->mepc += 4;
	return 0;
}

int emulate_screcv(ulong insn, struct sbi_trap_regs *regs) {
	int64_t ci;
	uint64_t addr, sdsrc, usid;
	uint8_t *ptable, perm, existing_perms;
	uint32_t M, grant;
	struct sbi_trap_info trap;

	addr = GET_RS1(insn, regs);
	ci = sccck(addr, 1);
	/* ChecK: valid address */
	if (unlikely(ci < 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_ADDR;
		trap.tval = addr;
		return sbi_trap_redirect(regs, &trap);
	} else if (unlikely(ci == 0)) {
		trap.epc = regs->mepc;
		/* Check: For Valid cell */
		trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
		trap.tval = 0;
		return sbi_trap_redirect(regs, &trap);
	}

	sdsrc = GET_RS2(insn, regs);
	ptable = (uint8_t *)(uintptr_t)((csr_read(CSR_SATP) & SATP64_PPN) << 12);
	M = ((uint32_t *)ptable)[2];
	if(unlikely((sdsrc == 0) || (sdsrc > M))) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_INV_SDID;
		trap.tval = sdsrc;
		return sbi_trap_redirect(regs, &trap);
	}

	perm = IMM_S(insn);
	if(unlikely(perm & ~RT_PERMS)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (0 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
  } else if(unlikely((perm & RT_PERMS) == 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (1 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
	}

	grant = *scga(ci, sdsrc);
	existing_perms = *scpa(ci, 0);

	usid = csr_read(CSR_USID);
	if(unlikely((grant >> 4) != usid)){
		trap.epc = regs->mepc;
		// trap.cause;
		// trap.tval;
		return sbi_trap_redirect(regs, &trap);
	}
	if(unlikely(perm & ~(grant & RT_PERMS))) {
		trap.epc = regs->mepc;
		// trap.cause;
		// trap.tval;
		return sbi_trap_redirect(regs, &trap);
	}

	if(perm == (grant & RT_PERMS))
		*scga(ci, sdsrc) = G(SDINV, 0);
	else
		*scga(ci, sdsrc) = G(sdsrc, ((grant & RT_PERMS) & ~perm));
	*scpa(ci, 0) = existing_perms | perm | RT_V;
	__asm__ __volatile("sfence.vma");

	regs->mepc += 4;
	return 0;
}

int emulate_sctfer(ulong insn, struct sbi_trap_regs *regs) {
	uint64_t addr;
	uint32_t grantinsn;
	struct sbi_trap_info trap;

	addr = GET_RS1(insn, regs);

	grantinsn = (insn & ~(MASK_TFER)) | MATCH_GRANT;
	emulate_scgrant(grantinsn, regs);

	if(_emulate_scprot(addr, 0, regs, &trap.cause, &trap.tval)){
		trap.epc = regs->mepc;
    return sbi_trap_redirect(regs, &trap);
	}

	__asm__ __volatile("sfence.vma");
	regs->mepc += 4;
	return 0;
}

int emulate_scexcl(ulong insn, struct sbi_trap_regs *regs) {
	uint64_t sd, addr, usid;
	uint32_t M, gperm;
	uint8_t *ptable, perm, existing_perms, pperm;
	int64_t ci;
	struct sbi_trap_info trap;

	addr = GET_RS1(insn, regs);
	ci = sccck(addr, 1);
	/* ChecK: valid address */
	if (unlikely(ci < 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_ADDR;
		trap.tval = addr;
		return sbi_trap_redirect(regs, &trap);
	} else if (unlikely(ci == 0)) {
	/* Check: For Valid cell */
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_INV_CELL_STATE;
		trap.tval = 0;
		return sbi_trap_redirect(regs, &trap);
	}

	perm = GET_RS2(insn, regs);
	existing_perms = *scpa(ci, 0);
  if(unlikely(perm & ~RT_PERMS)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (0 << 8) | (uint8_t)perm;
		return -1;
  } else if(unlikely((perm & RT_PERMS) == 0)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (1 << 8) | (uint8_t)perm;
		return sbi_trap_redirect(regs, &trap);
	} else if(unlikely(perm & ~existing_perms)) {
		trap.epc = regs->mepc;
		trap.cause = RISCV_EXCP_SECCELL_ILL_PERM;
		trap.tval = (2 << 8) | (uint8_t)perm;
		return -1;
	}

	ptable = (uint8_t *)(uintptr_t)((csr_read(CSR_SATP) & SATP64_PPN) << 12);
	M = ((uint32_t *)ptable)[2];
	usid = csr_read(CSR_USID);
	for(sd = 1; sd < M; sd++) {
		if(sd != usid) {
			pperm = *scpa(ci, sd);
			gperm = *scga(ci, sd);
			if(((pperm & RT_PERMS) != 0) || (gperm != G(SDINV, 0)))
				break;
		}
	}
	if (sd == M) /* Case: Exclusive */
		SET_RD(insn, regs, 0);
	else
		SET_RD(insn, regs, 1);

	regs->mepc += 4;
	return 0;
}
