/* PPC GNU/Linux native support.

   Copyright (C) 1988-2019 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "observable.h"
#include "frame.h"
#include "inferior.h"
#include "gdbthread.h"
#include "gdbcore.h"
#include "regcache.h"
#include "regset.h"
#include "target.h"
#include "linux-nat.h"
#include <sys/types.h>
#include <signal.h>
#include <sys/user.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include "common/gdb_wait.h"
#include <fcntl.h>
#include <sys/procfs.h>
#include "nat/gdb_ptrace.h"
#include "nat/linux-ptrace.h"
#include "inf-ptrace.h"

/* Prototypes for supply_gregset etc.  */
#include "gregset.h"
#include "ppc-tdep.h"
#include "ppc-linux-tdep.h"

/* Required when using the AUXV.  */
#include "elf/common.h"
#include "auxv.h"

#include "arch/ppc-linux-common.h"
#include "arch/ppc-linux-tdesc.h"
#include "nat/ppc-linux.h"
#include "linux-tdep.h"

/* Similarly for the hardware watchpoint support.  These requests are used
   when the PowerPC HWDEBUG ptrace interface is not available.  */
#ifndef PTRACE_GET_DEBUGREG
#define PTRACE_GET_DEBUGREG    25
#endif
#ifndef PTRACE_SET_DEBUGREG
#define PTRACE_SET_DEBUGREG    26
#endif
#ifndef PTRACE_GETSIGINFO
#define PTRACE_GETSIGINFO    0x4202
#endif

/* These requests are used when the PowerPC HWDEBUG ptrace interface is
   available.  It exposes the debug facilities of PowerPC processors, as well
   as additional features of BookE processors, such as ranged breakpoints and
   watchpoints and hardware-accelerated condition evaluation.  */
#ifndef PPC_PTRACE_GETHWDBGINFO

/* Not having PPC_PTRACE_GETHWDBGINFO defined means that the PowerPC HWDEBUG 
   ptrace interface is not present in ptrace.h, so we'll have to pretty much
   include it all here so that the code at least compiles on older systems.  */
#define PPC_PTRACE_GETHWDBGINFO 0x89
#define PPC_PTRACE_SETHWDEBUG   0x88
#define PPC_PTRACE_DELHWDEBUG   0x87

struct ppc_debug_info
{
        uint32_t version;               /* Only version 1 exists to date.  */
        uint32_t num_instruction_bps;
        uint32_t num_data_bps;
        uint32_t num_condition_regs;
        uint32_t data_bp_alignment;
        uint32_t sizeof_condition;      /* size of the DVC register.  */
        uint64_t features;
};

/* Features will have bits indicating whether there is support for:  */
#define PPC_DEBUG_FEATURE_INSN_BP_RANGE         0x1
#define PPC_DEBUG_FEATURE_INSN_BP_MASK          0x2
#define PPC_DEBUG_FEATURE_DATA_BP_RANGE         0x4
#define PPC_DEBUG_FEATURE_DATA_BP_MASK          0x8

struct ppc_hw_breakpoint
{
        uint32_t version;               /* currently, version must be 1 */
        uint32_t trigger_type;          /* only some combinations allowed */
        uint32_t addr_mode;             /* address match mode */
        uint32_t condition_mode;        /* break/watchpoint condition flags */
        uint64_t addr;                  /* break/watchpoint address */
        uint64_t addr2;                 /* range end or mask */
        uint64_t condition_value;       /* contents of the DVC register */
};

/* Trigger type.  */
#define PPC_BREAKPOINT_TRIGGER_EXECUTE  0x1
#define PPC_BREAKPOINT_TRIGGER_READ     0x2
#define PPC_BREAKPOINT_TRIGGER_WRITE    0x4
#define PPC_BREAKPOINT_TRIGGER_RW       0x6

/* Address mode.  */
#define PPC_BREAKPOINT_MODE_EXACT               0x0
#define PPC_BREAKPOINT_MODE_RANGE_INCLUSIVE     0x1
#define PPC_BREAKPOINT_MODE_RANGE_EXCLUSIVE     0x2
#define PPC_BREAKPOINT_MODE_MASK                0x3

/* Condition mode.  */
#define PPC_BREAKPOINT_CONDITION_NONE   0x0
#define PPC_BREAKPOINT_CONDITION_AND    0x1
#define PPC_BREAKPOINT_CONDITION_EXACT  0x1
#define PPC_BREAKPOINT_CONDITION_OR     0x2
#define PPC_BREAKPOINT_CONDITION_AND_OR 0x3
#define PPC_BREAKPOINT_CONDITION_BE_ALL 0x00ff0000
#define PPC_BREAKPOINT_CONDITION_BE_SHIFT       16
#define PPC_BREAKPOINT_CONDITION_BE(n)  \
        (1<<((n)+PPC_BREAKPOINT_CONDITION_BE_SHIFT))
#endif /* PPC_PTRACE_GETHWDBGINFO */

/* Feature defined on Linux kernel v3.9: DAWR interface, that enables wider
   watchpoint (up to 512 bytes).  */
#ifndef PPC_DEBUG_FEATURE_DATA_BP_DAWR
#define PPC_DEBUG_FEATURE_DATA_BP_DAWR	0x10
#endif /* PPC_DEBUG_FEATURE_DATA_BP_DAWR */

/* Similarly for the general-purpose (gp0 -- gp31)
   and floating-point registers (fp0 -- fp31).  */
#ifndef PTRACE_GETREGS
#define PTRACE_GETREGS 12
#endif
#ifndef PTRACE_SETREGS
#define PTRACE_SETREGS 13
#endif
#ifndef PTRACE_GETFPREGS
#define PTRACE_GETFPREGS 14
#endif
#ifndef PTRACE_SETFPREGS
#define PTRACE_SETFPREGS 15
#endif

/* This oddity is because the Linux kernel defines elf_vrregset_t as
   an array of 33 16 bytes long elements.  I.e. it leaves out vrsave.
   However the PTRACE_GETVRREGS and PTRACE_SETVRREGS requests return
   the vrsave as an extra 4 bytes at the end.  I opted for creating a
   flat array of chars, so that it is easier to manipulate for gdb.

   There are 32 vector registers 16 bytes longs, plus a VSCR register
   which is only 4 bytes long, but is fetched as a 16 bytes
   quantity.  Up to here we have the elf_vrregset_t structure.
   Appended to this there is space for the VRSAVE register: 4 bytes.
   Even though this vrsave register is not included in the regset
   typedef, it is handled by the ptrace requests.

   The layout is like this (where x is the actual value of the vscr reg): */

/* *INDENT-OFF* */
/*
Big-Endian:
   |.|.|.|.|.....|.|.|.|.||.|.|.|x||.|
   <------->     <-------><-------><->
     VR0           VR31     VSCR    VRSAVE
Little-Endian:
   |.|.|.|.|.....|.|.|.|.||X|.|.|.||.|
   <------->     <-------><-------><->
     VR0           VR31     VSCR    VRSAVE
*/
/* *INDENT-ON* */

typedef char gdb_vrregset_t[PPC_LINUX_SIZEOF_VRREGSET];

/* This is the layout of the POWER7 VSX registers and the way they overlap
   with the existing FPR and VMX registers.

                    VSR doubleword 0               VSR doubleword 1
           ----------------------------------------------------------------
   VSR[0]  |             FPR[0]            |                              |
           ----------------------------------------------------------------
   VSR[1]  |             FPR[1]            |                              |
           ----------------------------------------------------------------
           |              ...              |                              |
           |              ...              |                              |
           ----------------------------------------------------------------
   VSR[30] |             FPR[30]           |                              |
           ----------------------------------------------------------------
   VSR[31] |             FPR[31]           |                              |
           ----------------------------------------------------------------
   VSR[32] |                             VR[0]                            |
           ----------------------------------------------------------------
   VSR[33] |                             VR[1]                            |
           ----------------------------------------------------------------
           |                              ...                             |
           |                              ...                             |
           ----------------------------------------------------------------
   VSR[62] |                             VR[30]                           |
           ----------------------------------------------------------------
   VSR[63] |                             VR[31]                           |
          ----------------------------------------------------------------

   VSX has 64 128bit registers.  The first 32 registers overlap with
   the FP registers (doubleword 0) and hence extend them with additional
   64 bits (doubleword 1).  The other 32 regs overlap with the VMX
   registers.  */
typedef char gdb_vsxregset_t[PPC_LINUX_SIZEOF_VSXREGSET];

/* On PPC processors that support the Signal Processing Extension
   (SPE) APU, the general-purpose registers are 64 bits long.
   However, the ordinary Linux kernel PTRACE_PEEKUSER / PTRACE_POKEUSER
   ptrace calls only access the lower half of each register, to allow
   them to behave the same way they do on non-SPE systems.  There's a
   separate pair of calls, PTRACE_GETEVRREGS / PTRACE_SETEVRREGS, that
   read and write the top halves of all the general-purpose registers
   at once, along with some SPE-specific registers.

   GDB itself continues to claim the general-purpose registers are 32
   bits long.  It has unnamed raw registers that hold the upper halves
   of the gprs, and the full 64-bit SIMD views of the registers,
   'ev0' -- 'ev31', are pseudo-registers that splice the top and
   bottom halves together.

   This is the structure filled in by PTRACE_GETEVRREGS and written to
   the inferior's registers by PTRACE_SETEVRREGS.  */
struct gdb_evrregset_t
{
  unsigned long evr[32];
  unsigned long long acc;
  unsigned long spefscr;
};

/* Non-zero if our kernel may support the PTRACE_GETVSXREGS and
   PTRACE_SETVSXREGS requests, for reading and writing the VSX
   POWER7 registers 0 through 31.  Zero if we've tried one of them and
   gotten an error.  Note that VSX registers 32 through 63 overlap
   with VR registers 0 through 31.  */
int have_ptrace_getsetvsxregs = 1;

/* Non-zero if our kernel may support the PTRACE_GETVRREGS and
   PTRACE_SETVRREGS requests, for reading and writing the Altivec
   registers.  Zero if we've tried one of them and gotten an
   error.  */
int have_ptrace_getvrregs = 1;

/* Non-zero if our kernel may support the PTRACE_GETEVRREGS and
   PTRACE_SETEVRREGS requests, for reading and writing the SPE
   registers.  Zero if we've tried one of them and gotten an
   error.  */
int have_ptrace_getsetevrregs = 1;

/* Non-zero if our kernel may support the PTRACE_GETREGS and
   PTRACE_SETREGS requests, for reading and writing the
   general-purpose registers.  Zero if we've tried one of
   them and gotten an error.  */
int have_ptrace_getsetregs = 1;

/* Non-zero if our kernel may support the PTRACE_GETFPREGS and
   PTRACE_SETFPREGS requests, for reading and writing the
   floating-pointers registers.  Zero if we've tried one of
   them and gotten an error.  */
int have_ptrace_getsetfpregs = 1;

struct ppc_linux_nat_target final : public linux_nat_target
{
  /* Add our register access methods.  */
  void fetch_registers (struct regcache *, int) override;
  void store_registers (struct regcache *, int) override;

  /* Add our breakpoint/watchpoint methods.  */
  int can_use_hw_breakpoint (enum bptype, int, int) override;

  int insert_hw_breakpoint (struct gdbarch *, struct bp_target_info *)
    override;

  int remove_hw_breakpoint (struct gdbarch *, struct bp_target_info *)
    override;

  int region_ok_for_hw_watchpoint (CORE_ADDR, LONGEST) override;

  int insert_watchpoint (CORE_ADDR, int, enum target_hw_bp_type,
			 struct expression *) override;

  int remove_watchpoint (CORE_ADDR, int, enum target_hw_bp_type,
			 struct expression *) override;

  int insert_mask_watchpoint (CORE_ADDR, CORE_ADDR, enum target_hw_bp_type)
    override;

  int remove_mask_watchpoint (CORE_ADDR, CORE_ADDR, enum target_hw_bp_type)
    override;

  bool stopped_by_watchpoint () override;

  bool stopped_data_address (CORE_ADDR *) override;

  bool watchpoint_addr_within_range (CORE_ADDR, CORE_ADDR, LONGEST) override;

  bool can_accel_watchpoint_condition (CORE_ADDR, LONGEST, int, struct expression *)
    override;

  int masked_watch_num_registers (CORE_ADDR, CORE_ADDR) override;

  int ranged_break_num_registers () override;

  const struct target_desc *read_description ()  override;

  int auxv_parse (gdb_byte **readptr,
		  gdb_byte *endptr, CORE_ADDR *typep, CORE_ADDR *valp)
    override;

  /* Override linux_nat_target low methods.  */
  void low_new_thread (struct lwp_info *lp) override;
};

static ppc_linux_nat_target the_ppc_linux_nat_target;

/* *INDENT-OFF* */
/* registers layout, as presented by the ptrace interface:
PT_R0, PT_R1, PT_R2, PT_R3, PT_R4, PT_R5, PT_R6, PT_R7,
PT_R8, PT_R9, PT_R10, PT_R11, PT_R12, PT_R13, PT_R14, PT_R15,
PT_R16, PT_R17, PT_R18, PT_R19, PT_R20, PT_R21, PT_R22, PT_R23,
PT_R24, PT_R25, PT_R26, PT_R27, PT_R28, PT_R29, PT_R30, PT_R31,
PT_FPR0, PT_FPR0 + 2, PT_FPR0 + 4, PT_FPR0 + 6,
PT_FPR0 + 8, PT_FPR0 + 10, PT_FPR0 + 12, PT_FPR0 + 14,
PT_FPR0 + 16, PT_FPR0 + 18, PT_FPR0 + 20, PT_FPR0 + 22,
PT_FPR0 + 24, PT_FPR0 + 26, PT_FPR0 + 28, PT_FPR0 + 30,
PT_FPR0 + 32, PT_FPR0 + 34, PT_FPR0 + 36, PT_FPR0 + 38,
PT_FPR0 + 40, PT_FPR0 + 42, PT_FPR0 + 44, PT_FPR0 + 46,
PT_FPR0 + 48, PT_FPR0 + 50, PT_FPR0 + 52, PT_FPR0 + 54,
PT_FPR0 + 56, PT_FPR0 + 58, PT_FPR0 + 60, PT_FPR0 + 62,
PT_NIP, PT_MSR, PT_CCR, PT_LNK, PT_CTR, PT_XER, PT_MQ */
/* *INDENT_ON * */

static int
ppc_register_u_addr (struct gdbarch *gdbarch, int regno)
{
  int u_addr = -1;
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  /* NOTE: cagney/2003-11-25: This is the word size used by the ptrace
     interface, and not the wordsize of the program's ABI.  */
  int wordsize = sizeof (long);

  /* General purpose registers occupy 1 slot each in the buffer.  */
  if (regno >= tdep->ppc_gp0_regnum 
      && regno < tdep->ppc_gp0_regnum + ppc_num_gprs)
    u_addr = ((regno - tdep->ppc_gp0_regnum + PT_R0) * wordsize);

  /* Floating point regs: eight bytes each in both 32- and 64-bit
     ptrace interfaces.  Thus, two slots each in 32-bit interface, one
     slot each in 64-bit interface.  */
  if (tdep->ppc_fp0_regnum >= 0
      && regno >= tdep->ppc_fp0_regnum
      && regno < tdep->ppc_fp0_regnum + ppc_num_fprs)
    u_addr = (PT_FPR0 * wordsize) + ((regno - tdep->ppc_fp0_regnum) * 8);

  /* UISA special purpose registers: 1 slot each.  */
  if (regno == gdbarch_pc_regnum (gdbarch))
    u_addr = PT_NIP * wordsize;
  if (regno == tdep->ppc_lr_regnum)
    u_addr = PT_LNK * wordsize;
  if (regno == tdep->ppc_cr_regnum)
    u_addr = PT_CCR * wordsize;
  if (regno == tdep->ppc_xer_regnum)
    u_addr = PT_XER * wordsize;
  if (regno == tdep->ppc_ctr_regnum)
    u_addr = PT_CTR * wordsize;
#ifdef PT_MQ
  if (regno == tdep->ppc_mq_regnum)
    u_addr = PT_MQ * wordsize;
#endif
  if (regno == tdep->ppc_ps_regnum)
    u_addr = PT_MSR * wordsize;
  if (regno == PPC_ORIG_R3_REGNUM)
    u_addr = PT_ORIG_R3 * wordsize;
  if (regno == PPC_TRAP_REGNUM)
    u_addr = PT_TRAP * wordsize;
  if (tdep->ppc_fpscr_regnum >= 0
      && regno == tdep->ppc_fpscr_regnum)
    {
      /* NOTE: cagney/2005-02-08: On some 64-bit GNU/Linux systems the
	 kernel headers incorrectly contained the 32-bit definition of
	 PT_FPSCR.  For the 32-bit definition, floating-point
	 registers occupy two 32-bit "slots", and the FPSCR lives in
	 the second half of such a slot-pair (hence +1).  For 64-bit,
	 the FPSCR instead occupies the full 64-bit 2-word-slot and
	 hence no adjustment is necessary.  Hack around this.  */
      if (wordsize == 8 && PT_FPSCR == (48 + 32 + 1))
	u_addr = (48 + 32) * wordsize;
      /* If the FPSCR is 64-bit wide, we need to fetch the whole 64-bit
	 slot and not just its second word.  The PT_FPSCR supplied when
	 GDB is compiled as a 32-bit app doesn't reflect this.  */
      else if (wordsize == 4 && register_size (gdbarch, regno) == 8
	       && PT_FPSCR == (48 + 2*32 + 1))
	u_addr = (48 + 2*32) * wordsize;
      else
	u_addr = PT_FPSCR * wordsize;
    }
  return u_addr;
}

/* The Linux kernel ptrace interface for POWER7 VSX registers uses the
   registers set mechanism, as opposed to the interface for all the
   other registers, that stores/fetches each register individually.  */
static void
fetch_vsx_registers (struct regcache *regcache, int tid, int regno)
{
  int ret;
  gdb_vsxregset_t regs;
  const struct regset *vsxregset = ppc_linux_vsxregset ();

  ret = ptrace (PTRACE_GETVSXREGS, tid, 0, &regs);
  if (ret < 0)
    {
      if (errno == EIO)
	{
	  have_ptrace_getsetvsxregs = 0;
	  return;
	}
      perror_with_name (_("Unable to fetch VSX registers"));
    }

  vsxregset->supply_regset (vsxregset, regcache, regno, &regs,
			    PPC_LINUX_SIZEOF_VSXREGSET);
}

/* The Linux kernel ptrace interface for AltiVec registers uses the
   registers set mechanism, as opposed to the interface for all the
   other registers, that stores/fetches each register individually.  */
static void
fetch_altivec_registers (struct regcache *regcache, int tid,
			 int regno)
{
  int ret;
  gdb_vrregset_t regs;
  struct gdbarch *gdbarch = regcache->arch ();
  const struct regset *vrregset = ppc_linux_vrregset (gdbarch);

  ret = ptrace (PTRACE_GETVRREGS, tid, 0, &regs);
  if (ret < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getvrregs = 0;
          return;
        }
      perror_with_name (_("Unable to fetch AltiVec registers"));
    }

  vrregset->supply_regset (vrregset, regcache, regno, &regs,
			   PPC_LINUX_SIZEOF_VRREGSET);
}

/* Fetch the top 32 bits of TID's general-purpose registers and the
   SPE-specific registers, and place the results in EVRREGSET.  If we
   don't support PTRACE_GETEVRREGS, then just fill EVRREGSET with
   zeros.

   All the logic to deal with whether or not the PTRACE_GETEVRREGS and
   PTRACE_SETEVRREGS requests are supported is isolated here, and in
   set_spe_registers.  */
static void
get_spe_registers (int tid, struct gdb_evrregset_t *evrregset)
{
  if (have_ptrace_getsetevrregs)
    {
      if (ptrace (PTRACE_GETEVRREGS, tid, 0, evrregset) >= 0)
        return;
      else
        {
          /* EIO means that the PTRACE_GETEVRREGS request isn't supported;
             we just return zeros.  */
          if (errno == EIO)
            have_ptrace_getsetevrregs = 0;
          else
            /* Anything else needs to be reported.  */
            perror_with_name (_("Unable to fetch SPE registers"));
        }
    }

  memset (evrregset, 0, sizeof (*evrregset));
}

/* Supply values from TID for SPE-specific raw registers: the upper
   halves of the GPRs, the accumulator, and the spefscr.  REGNO must
   be the number of an upper half register, acc, spefscr, or -1 to
   supply the values of all registers.  */
static void
fetch_spe_register (struct regcache *regcache, int tid, int regno)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  struct gdb_evrregset_t evrregs;

  gdb_assert (sizeof (evrregs.evr[0])
              == register_size (gdbarch, tdep->ppc_ev0_upper_regnum));
  gdb_assert (sizeof (evrregs.acc)
              == register_size (gdbarch, tdep->ppc_acc_regnum));
  gdb_assert (sizeof (evrregs.spefscr)
              == register_size (gdbarch, tdep->ppc_spefscr_regnum));

  get_spe_registers (tid, &evrregs);

  if (regno == -1)
    {
      int i;

      for (i = 0; i < ppc_num_gprs; i++)
        regcache->raw_supply (tdep->ppc_ev0_upper_regnum + i, &evrregs.evr[i]);
    }
  else if (tdep->ppc_ev0_upper_regnum <= regno
           && regno < tdep->ppc_ev0_upper_regnum + ppc_num_gprs)
    regcache->raw_supply (regno,
			  &evrregs.evr[regno - tdep->ppc_ev0_upper_regnum]);

  if (regno == -1
      || regno == tdep->ppc_acc_regnum)
    regcache->raw_supply (tdep->ppc_acc_regnum, &evrregs.acc);

  if (regno == -1
      || regno == tdep->ppc_spefscr_regnum)
    regcache->raw_supply (tdep->ppc_spefscr_regnum, &evrregs.spefscr);
}

/* Use ptrace to fetch all registers from the register set with note
   type REGSET_ID, size REGSIZE, and layout described by REGSET, from
   process/thread TID and supply their values to REGCACHE.  If ptrace
   returns ENODATA to indicate the regset is unavailable, mark the
   registers as unavailable in REGCACHE.  */

static void
fetch_regset (struct regcache *regcache, int tid,
	      int regset_id, int regsetsize, const struct regset *regset)
{
  void *buf = alloca (regsetsize);
  struct iovec iov;

  iov.iov_base = buf;
  iov.iov_len = regsetsize;

  if (ptrace (PTRACE_GETREGSET, tid, regset_id, &iov) < 0)
    {
      if (errno == ENODATA)
	regset->supply_regset (regset, regcache, -1, NULL, regsetsize);
      else
	perror_with_name (_("Couldn't get register set"));
    }
  else
    regset->supply_regset (regset, regcache, -1, buf, regsetsize);
}

/* Use ptrace to store register REGNUM of the regset with note type
   REGSET_ID, size REGSETSIZE, and layout described by REGSET, from
   REGCACHE back to process/thread TID.  If REGNUM is -1 all registers
   in the set are collected and stored.  */

static void
store_regset (const struct regcache *regcache, int tid, int regnum,
	      int regset_id, int regsetsize, const struct regset *regset)
{
  void *buf = alloca (regsetsize);
  struct iovec iov;

  iov.iov_base = buf;
  iov.iov_len = regsetsize;

  /* Make sure that the buffer that will be stored has up to date values
     for the registers that won't be collected.  */
  if (ptrace (PTRACE_GETREGSET, tid, regset_id, &iov) < 0)
    perror_with_name (_("Couldn't get register set"));

  regset->collect_regset (regset, regcache, regnum, buf, regsetsize);

  if (ptrace (PTRACE_SETREGSET, tid, regset_id, &iov) < 0)
    perror_with_name (_("Couldn't set register set"));
}

/* Check whether the kernel provides a register set with number
   REGSET_ID of size REGSETSIZE for process/thread TID.  */

static bool
check_regset (int tid, int regset_id, int regsetsize)
{
  void *buf = alloca (regsetsize);
  struct iovec iov;

  iov.iov_base = buf;
  iov.iov_len = regsetsize;

  if (ptrace (PTRACE_GETREGSET, tid, regset_id, &iov) >= 0
      || errno == ENODATA)
    return true;
  else
    return false;
}

static void
fetch_register (struct regcache *regcache, int tid, int regno)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  /* This isn't really an address.  But ptrace thinks of it as one.  */
  CORE_ADDR regaddr = ppc_register_u_addr (gdbarch, regno);
  int bytes_transferred;
  gdb_byte buf[PPC_MAX_REGISTER_SIZE];

  if (altivec_register_p (gdbarch, regno))
    {
      /* If this is the first time through, or if it is not the first
         time through, and we have comfirmed that there is kernel
         support for such a ptrace request, then go and fetch the
         register.  */
      if (have_ptrace_getvrregs)
       {
         fetch_altivec_registers (regcache, tid, regno);
         return;
       }
     /* If we have discovered that there is no ptrace support for
        AltiVec registers, fall through and return zeroes, because
        regaddr will be -1 in this case.  */
    }
  else if (vsx_register_p (gdbarch, regno))
    {
      if (have_ptrace_getsetvsxregs)
	{
	  fetch_vsx_registers (regcache, tid, regno);
	  return;
	}
    }
  else if (spe_register_p (gdbarch, regno))
    {
      fetch_spe_register (regcache, tid, regno);
      return;
    }
  else if (regno == PPC_DSCR_REGNUM)
    {
      gdb_assert (tdep->ppc_dscr_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_DSCR,
		    PPC_LINUX_SIZEOF_DSCRREGSET,
		    &ppc32_linux_dscrregset);
      return;
    }
  else if (regno == PPC_PPR_REGNUM)
    {
      gdb_assert (tdep->ppc_ppr_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_PPR,
		    PPC_LINUX_SIZEOF_PPRREGSET,
		    &ppc32_linux_pprregset);
      return;
    }
  else if (regno == PPC_TAR_REGNUM)
    {
      gdb_assert (tdep->ppc_tar_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_TAR,
		    PPC_LINUX_SIZEOF_TARREGSET,
		    &ppc32_linux_tarregset);
      return;
    }
  else if (PPC_IS_EBB_REGNUM (regno))
    {
      gdb_assert (tdep->have_ebb);

      fetch_regset (regcache, tid, NT_PPC_EBB,
		    PPC_LINUX_SIZEOF_EBBREGSET,
		    &ppc32_linux_ebbregset);
      return;
    }
  else if (PPC_IS_PMU_REGNUM (regno))
    {
      gdb_assert (tdep->ppc_mmcr0_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_PMU,
		    PPC_LINUX_SIZEOF_PMUREGSET,
		    &ppc32_linux_pmuregset);
      return;
    }
  else if (PPC_IS_TMSPR_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_spr);

      fetch_regset (regcache, tid, NT_PPC_TM_SPR,
		    PPC_LINUX_SIZEOF_TM_SPRREGSET,
		    &ppc32_linux_tm_sprregset);
      return;
    }
  else if (PPC_IS_CKPTGP_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_core);

      const struct regset *cgprregset = ppc_linux_cgprregset (gdbarch);
      fetch_regset (regcache, tid, NT_PPC_TM_CGPR,
		    (tdep->wordsize == 4?
		     PPC32_LINUX_SIZEOF_CGPRREGSET
		     : PPC64_LINUX_SIZEOF_CGPRREGSET),
		    cgprregset);
      return;
    }
  else if (PPC_IS_CKPTFP_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_fpu);

      fetch_regset (regcache, tid, NT_PPC_TM_CFPR,
		    PPC_LINUX_SIZEOF_CFPRREGSET,
		    &ppc32_linux_cfprregset);
      return;
    }
  else if (PPC_IS_CKPTVMX_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_altivec);

      const struct regset *cvmxregset = ppc_linux_cvmxregset (gdbarch);
      fetch_regset (regcache, tid, NT_PPC_TM_CVMX,
		    PPC_LINUX_SIZEOF_CVMXREGSET,
		    cvmxregset);
      return;
    }
  else if (PPC_IS_CKPTVSX_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_vsx);

      fetch_regset (regcache, tid, NT_PPC_TM_CVSX,
		    PPC_LINUX_SIZEOF_CVSXREGSET,
		    &ppc32_linux_cvsxregset);
      return;
    }
  else if (regno == PPC_CPPR_REGNUM)
    {
      gdb_assert (tdep->ppc_cppr_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_TM_CPPR,
		    PPC_LINUX_SIZEOF_CPPRREGSET,
		    &ppc32_linux_cpprregset);
      return;
    }
  else if (regno == PPC_CDSCR_REGNUM)
    {
      gdb_assert (tdep->ppc_cdscr_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_TM_CDSCR,
		    PPC_LINUX_SIZEOF_CDSCRREGSET,
		    &ppc32_linux_cdscrregset);
      return;
    }
  else if (regno == PPC_CTAR_REGNUM)
    {
      gdb_assert (tdep->ppc_ctar_regnum != -1);

      fetch_regset (regcache, tid, NT_PPC_TM_CTAR,
		    PPC_LINUX_SIZEOF_CTARREGSET,
		    &ppc32_linux_ctarregset);
      return;
    }

  if (regaddr == -1)
    {
      memset (buf, '\0', register_size (gdbarch, regno));   /* Supply zeroes */
      regcache->raw_supply (regno, buf);
      return;
    }

  /* Read the raw register using sizeof(long) sized chunks.  On a
     32-bit platform, 64-bit floating-point registers will require two
     transfers.  */
  for (bytes_transferred = 0;
       bytes_transferred < register_size (gdbarch, regno);
       bytes_transferred += sizeof (long))
    {
      long l;

      errno = 0;
      l = ptrace (PTRACE_PEEKUSER, tid, (PTRACE_TYPE_ARG3) regaddr, 0);
      regaddr += sizeof (long);
      if (errno != 0)
	{
          char message[128];
	  xsnprintf (message, sizeof (message), "reading register %s (#%d)",
		     gdbarch_register_name (gdbarch, regno), regno);
	  perror_with_name (message);
	}
      memcpy (&buf[bytes_transferred], &l, sizeof (l));
    }

  /* Now supply the register.  Keep in mind that the regcache's idea
     of the register's size may not be a multiple of sizeof
     (long).  */
  if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_LITTLE)
    {
      /* Little-endian values are always found at the left end of the
         bytes transferred.  */
      regcache->raw_supply (regno, buf);
    }
  else if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
    {
      /* Big-endian values are found at the right end of the bytes
         transferred.  */
      size_t padding = (bytes_transferred - register_size (gdbarch, regno));
      regcache->raw_supply (regno, buf + padding);
    }
  else 
    internal_error (__FILE__, __LINE__,
                    _("fetch_register: unexpected byte order: %d"),
                    gdbarch_byte_order (gdbarch));
}

/* This function actually issues the request to ptrace, telling
   it to get all general-purpose registers and put them into the
   specified regset.
   
   If the ptrace request does not exist, this function returns 0
   and properly sets the have_ptrace_* flag.  If the request fails,
   this function calls perror_with_name.  Otherwise, if the request
   succeeds, then the regcache gets filled and 1 is returned.  */
static int
fetch_all_gp_regs (struct regcache *regcache, int tid)
{
  gdb_gregset_t gregset;

  if (ptrace (PTRACE_GETREGS, tid, 0, (void *) &gregset) < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getsetregs = 0;
          return 0;
        }
      perror_with_name (_("Couldn't get general-purpose registers."));
    }

  supply_gregset (regcache, (const gdb_gregset_t *) &gregset);

  return 1;
}

/* This is a wrapper for the fetch_all_gp_regs function.  It is
   responsible for verifying if this target has the ptrace request
   that can be used to fetch all general-purpose registers at one
   shot.  If it doesn't, then we should fetch them using the
   old-fashioned way, which is to iterate over the registers and
   request them one by one.  */
static void
fetch_gp_regs (struct regcache *regcache, int tid)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  int i;

  if (have_ptrace_getsetregs)
    if (fetch_all_gp_regs (regcache, tid))
      return;

  /* If we've hit this point, it doesn't really matter which
     architecture we are using.  We just need to read the
     registers in the "old-fashioned way".  */
  for (i = 0; i < ppc_num_gprs; i++)
    fetch_register (regcache, tid, tdep->ppc_gp0_regnum + i);
}

/* This function actually issues the request to ptrace, telling
   it to get all floating-point registers and put them into the
   specified regset.
   
   If the ptrace request does not exist, this function returns 0
   and properly sets the have_ptrace_* flag.  If the request fails,
   this function calls perror_with_name.  Otherwise, if the request
   succeeds, then the regcache gets filled and 1 is returned.  */
static int
fetch_all_fp_regs (struct regcache *regcache, int tid)
{
  gdb_fpregset_t fpregs;

  if (ptrace (PTRACE_GETFPREGS, tid, 0, (void *) &fpregs) < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getsetfpregs = 0;
          return 0;
        }
      perror_with_name (_("Couldn't get floating-point registers."));
    }

  supply_fpregset (regcache, (const gdb_fpregset_t *) &fpregs);

  return 1;
}

/* This is a wrapper for the fetch_all_fp_regs function.  It is
   responsible for verifying if this target has the ptrace request
   that can be used to fetch all floating-point registers at one
   shot.  If it doesn't, then we should fetch them using the
   old-fashioned way, which is to iterate over the registers and
   request them one by one.  */
static void
fetch_fp_regs (struct regcache *regcache, int tid)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  int i;

  if (have_ptrace_getsetfpregs)
    if (fetch_all_fp_regs (regcache, tid))
      return;
 
  /* If we've hit this point, it doesn't really matter which
     architecture we are using.  We just need to read the
     registers in the "old-fashioned way".  */
  for (i = 0; i < ppc_num_fprs; i++)
    fetch_register (regcache, tid, tdep->ppc_fp0_regnum + i);
}

static void 
fetch_ppc_registers (struct regcache *regcache, int tid)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  fetch_gp_regs (regcache, tid);
  if (tdep->ppc_fp0_regnum >= 0)
    fetch_fp_regs (regcache, tid);
  fetch_register (regcache, tid, gdbarch_pc_regnum (gdbarch));
  if (tdep->ppc_ps_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_ps_regnum);
  if (tdep->ppc_cr_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_cr_regnum);
  if (tdep->ppc_lr_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_lr_regnum);
  if (tdep->ppc_ctr_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_ctr_regnum);
  if (tdep->ppc_xer_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_xer_regnum);
  if (tdep->ppc_mq_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_mq_regnum);
  if (ppc_linux_trap_reg_p (gdbarch))
    {
      fetch_register (regcache, tid, PPC_ORIG_R3_REGNUM);
      fetch_register (regcache, tid, PPC_TRAP_REGNUM);
    }
  if (tdep->ppc_fpscr_regnum != -1)
    fetch_register (regcache, tid, tdep->ppc_fpscr_regnum);
  if (have_ptrace_getvrregs)
    if (tdep->ppc_vr0_regnum != -1 && tdep->ppc_vrsave_regnum != -1)
      fetch_altivec_registers (regcache, tid, -1);
  if (have_ptrace_getsetvsxregs)
    if (tdep->ppc_vsr0_upper_regnum != -1)
      fetch_vsx_registers (regcache, tid, -1);
  if (tdep->ppc_ev0_upper_regnum >= 0)
    fetch_spe_register (regcache, tid, -1);
  if (tdep->ppc_ppr_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_PPR,
		  PPC_LINUX_SIZEOF_PPRREGSET,
		  &ppc32_linux_pprregset);
  if (tdep->ppc_dscr_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_DSCR,
		  PPC_LINUX_SIZEOF_DSCRREGSET,
		  &ppc32_linux_dscrregset);
  if (tdep->ppc_tar_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_TAR,
		  PPC_LINUX_SIZEOF_TARREGSET,
		  &ppc32_linux_tarregset);
  if (tdep->have_ebb)
    fetch_regset (regcache, tid, NT_PPC_EBB,
		  PPC_LINUX_SIZEOF_EBBREGSET,
		  &ppc32_linux_ebbregset);
  if (tdep->ppc_mmcr0_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_PMU,
		  PPC_LINUX_SIZEOF_PMUREGSET,
		  &ppc32_linux_pmuregset);
  if (tdep->have_htm_spr)
    fetch_regset (regcache, tid, NT_PPC_TM_SPR,
		  PPC_LINUX_SIZEOF_TM_SPRREGSET,
		  &ppc32_linux_tm_sprregset);
  if (tdep->have_htm_core)
    {
      const struct regset *cgprregset = ppc_linux_cgprregset (gdbarch);
      fetch_regset (regcache, tid, NT_PPC_TM_CGPR,
		    (tdep->wordsize == 4?
		     PPC32_LINUX_SIZEOF_CGPRREGSET
		     : PPC64_LINUX_SIZEOF_CGPRREGSET),
		    cgprregset);
    }
  if (tdep->have_htm_fpu)
    fetch_regset (regcache, tid, NT_PPC_TM_CFPR,
		  PPC_LINUX_SIZEOF_CFPRREGSET,
		  &ppc32_linux_cfprregset);
  if (tdep->have_htm_altivec)
    {
      const struct regset *cvmxregset = ppc_linux_cvmxregset (gdbarch);
      fetch_regset (regcache, tid, NT_PPC_TM_CVMX,
		    PPC_LINUX_SIZEOF_CVMXREGSET,
		    cvmxregset);
    }
  if (tdep->have_htm_vsx)
    fetch_regset (regcache, tid, NT_PPC_TM_CVSX,
		  PPC_LINUX_SIZEOF_CVSXREGSET,
		  &ppc32_linux_cvsxregset);
  if (tdep->ppc_cppr_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_TM_CPPR,
		  PPC_LINUX_SIZEOF_CPPRREGSET,
		  &ppc32_linux_cpprregset);
  if (tdep->ppc_cdscr_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_TM_CDSCR,
		  PPC_LINUX_SIZEOF_CDSCRREGSET,
		  &ppc32_linux_cdscrregset);
  if (tdep->ppc_ctar_regnum != -1)
    fetch_regset (regcache, tid, NT_PPC_TM_CTAR,
		  PPC_LINUX_SIZEOF_CTARREGSET,
		  &ppc32_linux_ctarregset);
}

/* Fetch registers from the child process.  Fetch all registers if
   regno == -1, otherwise fetch all general registers or all floating
   point registers depending upon the value of regno.  */
void
ppc_linux_nat_target::fetch_registers (struct regcache *regcache, int regno)
{
  pid_t tid = get_ptrace_pid (regcache->ptid ());

  if (regno == -1)
    fetch_ppc_registers (regcache, tid);
  else 
    fetch_register (regcache, tid, regno);
}

static void
store_vsx_registers (const struct regcache *regcache, int tid, int regno)
{
  int ret;
  gdb_vsxregset_t regs;
  const struct regset *vsxregset = ppc_linux_vsxregset ();

  ret = ptrace (PTRACE_GETVSXREGS, tid, 0, &regs);
  if (ret < 0)
    {
      if (errno == EIO)
	{
	  have_ptrace_getsetvsxregs = 0;
	  return;
	}
      perror_with_name (_("Unable to fetch VSX registers"));
    }

  vsxregset->collect_regset (vsxregset, regcache, regno, &regs,
			     PPC_LINUX_SIZEOF_VSXREGSET);

  ret = ptrace (PTRACE_SETVSXREGS, tid, 0, &regs);
  if (ret < 0)
    perror_with_name (_("Unable to store VSX registers"));
}

static void
store_altivec_registers (const struct regcache *regcache, int tid,
			 int regno)
{
  int ret;
  gdb_vrregset_t regs;
  struct gdbarch *gdbarch = regcache->arch ();
  const struct regset *vrregset = ppc_linux_vrregset (gdbarch);

  ret = ptrace (PTRACE_GETVRREGS, tid, 0, &regs);
  if (ret < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getvrregs = 0;
          return;
        }
      perror_with_name (_("Unable to fetch AltiVec registers"));
    }

  vrregset->collect_regset (vrregset, regcache, regno, &regs,
			    PPC_LINUX_SIZEOF_VRREGSET);

  ret = ptrace (PTRACE_SETVRREGS, tid, 0, &regs);
  if (ret < 0)
    perror_with_name (_("Unable to store AltiVec registers"));
}

/* Assuming TID referrs to an SPE process, set the top halves of TID's
   general-purpose registers and its SPE-specific registers to the
   values in EVRREGSET.  If we don't support PTRACE_SETEVRREGS, do
   nothing.

   All the logic to deal with whether or not the PTRACE_GETEVRREGS and
   PTRACE_SETEVRREGS requests are supported is isolated here, and in
   get_spe_registers.  */
static void
set_spe_registers (int tid, struct gdb_evrregset_t *evrregset)
{
  if (have_ptrace_getsetevrregs)
    {
      if (ptrace (PTRACE_SETEVRREGS, tid, 0, evrregset) >= 0)
        return;
      else
        {
          /* EIO means that the PTRACE_SETEVRREGS request isn't
             supported; we fail silently, and don't try the call
             again.  */
          if (errno == EIO)
            have_ptrace_getsetevrregs = 0;
          else
            /* Anything else needs to be reported.  */
            perror_with_name (_("Unable to set SPE registers"));
        }
    }
}

/* Write GDB's value for the SPE-specific raw register REGNO to TID.
   If REGNO is -1, write the values of all the SPE-specific
   registers.  */
static void
store_spe_register (const struct regcache *regcache, int tid, int regno)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  struct gdb_evrregset_t evrregs;

  gdb_assert (sizeof (evrregs.evr[0])
              == register_size (gdbarch, tdep->ppc_ev0_upper_regnum));
  gdb_assert (sizeof (evrregs.acc)
              == register_size (gdbarch, tdep->ppc_acc_regnum));
  gdb_assert (sizeof (evrregs.spefscr)
              == register_size (gdbarch, tdep->ppc_spefscr_regnum));

  if (regno == -1)
    /* Since we're going to write out every register, the code below
       should store to every field of evrregs; if that doesn't happen,
       make it obvious by initializing it with suspicious values.  */
    memset (&evrregs, 42, sizeof (evrregs));
  else
    /* We can only read and write the entire EVR register set at a
       time, so to write just a single register, we do a
       read-modify-write maneuver.  */
    get_spe_registers (tid, &evrregs);

  if (regno == -1)
    {
      int i;

      for (i = 0; i < ppc_num_gprs; i++)
	regcache->raw_collect (tdep->ppc_ev0_upper_regnum + i,
			       &evrregs.evr[i]);
    }
  else if (tdep->ppc_ev0_upper_regnum <= regno
           && regno < tdep->ppc_ev0_upper_regnum + ppc_num_gprs)
    regcache->raw_collect (regno,
			   &evrregs.evr[regno - tdep->ppc_ev0_upper_regnum]);

  if (regno == -1
      || regno == tdep->ppc_acc_regnum)
    regcache->raw_collect (tdep->ppc_acc_regnum,
			   &evrregs.acc);

  if (regno == -1
      || regno == tdep->ppc_spefscr_regnum)
    regcache->raw_collect (tdep->ppc_spefscr_regnum,
			   &evrregs.spefscr);

  /* Write back the modified register set.  */
  set_spe_registers (tid, &evrregs);
}

static void
store_register (const struct regcache *regcache, int tid, int regno)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  /* This isn't really an address.  But ptrace thinks of it as one.  */
  CORE_ADDR regaddr = ppc_register_u_addr (gdbarch, regno);
  int i;
  size_t bytes_to_transfer;
  gdb_byte buf[PPC_MAX_REGISTER_SIZE];

  if (altivec_register_p (gdbarch, regno))
    {
      store_altivec_registers (regcache, tid, regno);
      return;
    }
  else if (vsx_register_p (gdbarch, regno))
    {
      store_vsx_registers (regcache, tid, regno);
      return;
    }
  else if (spe_register_p (gdbarch, regno))
    {
      store_spe_register (regcache, tid, regno);
      return;
    }
  else if (regno == PPC_DSCR_REGNUM)
    {
      gdb_assert (tdep->ppc_dscr_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_DSCR,
		    PPC_LINUX_SIZEOF_DSCRREGSET,
		    &ppc32_linux_dscrregset);
      return;
    }
  else if (regno == PPC_PPR_REGNUM)
    {
      gdb_assert (tdep->ppc_ppr_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_PPR,
		    PPC_LINUX_SIZEOF_PPRREGSET,
		    &ppc32_linux_pprregset);
      return;
    }
  else if (regno == PPC_TAR_REGNUM)
    {
      gdb_assert (tdep->ppc_tar_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_TAR,
		    PPC_LINUX_SIZEOF_TARREGSET,
		    &ppc32_linux_tarregset);
      return;
    }
  else if (PPC_IS_EBB_REGNUM (regno))
    {
      gdb_assert (tdep->have_ebb);

      store_regset (regcache, tid, regno, NT_PPC_EBB,
		    PPC_LINUX_SIZEOF_EBBREGSET,
		    &ppc32_linux_ebbregset);
      return;
    }
  else if (PPC_IS_PMU_REGNUM (regno))
    {
      gdb_assert (tdep->ppc_mmcr0_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_PMU,
		    PPC_LINUX_SIZEOF_PMUREGSET,
		    &ppc32_linux_pmuregset);
      return;
    }
  else if (PPC_IS_TMSPR_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_spr);

      store_regset (regcache, tid, regno, NT_PPC_TM_SPR,
		    PPC_LINUX_SIZEOF_TM_SPRREGSET,
		    &ppc32_linux_tm_sprregset);
      return;
    }
  else if (PPC_IS_CKPTGP_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_core);

      const struct regset *cgprregset = ppc_linux_cgprregset (gdbarch);
      store_regset (regcache, tid, regno, NT_PPC_TM_CGPR,
		    (tdep->wordsize == 4?
		     PPC32_LINUX_SIZEOF_CGPRREGSET
		     : PPC64_LINUX_SIZEOF_CGPRREGSET),
		    cgprregset);
      return;
    }
  else if (PPC_IS_CKPTFP_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_fpu);

      store_regset (regcache, tid, regno, NT_PPC_TM_CFPR,
		    PPC_LINUX_SIZEOF_CFPRREGSET,
		    &ppc32_linux_cfprregset);
      return;
    }
  else if (PPC_IS_CKPTVMX_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_altivec);

      const struct regset *cvmxregset = ppc_linux_cvmxregset (gdbarch);
      store_regset (regcache, tid, regno, NT_PPC_TM_CVMX,
		    PPC_LINUX_SIZEOF_CVMXREGSET,
		    cvmxregset);
      return;
    }
  else if (PPC_IS_CKPTVSX_REGNUM (regno))
    {
      gdb_assert (tdep->have_htm_vsx);

      store_regset (regcache, tid, regno, NT_PPC_TM_CVSX,
		    PPC_LINUX_SIZEOF_CVSXREGSET,
		    &ppc32_linux_cvsxregset);
      return;
    }
  else if (regno == PPC_CPPR_REGNUM)
    {
      gdb_assert (tdep->ppc_cppr_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_TM_CPPR,
		    PPC_LINUX_SIZEOF_CPPRREGSET,
		    &ppc32_linux_cpprregset);
      return;
    }
  else if (regno == PPC_CDSCR_REGNUM)
    {
      gdb_assert (tdep->ppc_cdscr_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_TM_CDSCR,
		    PPC_LINUX_SIZEOF_CDSCRREGSET,
		    &ppc32_linux_cdscrregset);
      return;
    }
  else if (regno == PPC_CTAR_REGNUM)
    {
      gdb_assert (tdep->ppc_ctar_regnum != -1);

      store_regset (regcache, tid, regno, NT_PPC_TM_CTAR,
		    PPC_LINUX_SIZEOF_CTARREGSET,
		    &ppc32_linux_ctarregset);
      return;
    }

  if (regaddr == -1)
    return;

  /* First collect the register.  Keep in mind that the regcache's
     idea of the register's size may not be a multiple of sizeof
     (long).  */
  memset (buf, 0, sizeof buf);
  bytes_to_transfer = align_up (register_size (gdbarch, regno), sizeof (long));
  if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_LITTLE)
    {
      /* Little-endian values always sit at the left end of the buffer.  */
      regcache->raw_collect (regno, buf);
    }
  else if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
    {
      /* Big-endian values sit at the right end of the buffer.  */
      size_t padding = (bytes_to_transfer - register_size (gdbarch, regno));
      regcache->raw_collect (regno, buf + padding);
    }

  for (i = 0; i < bytes_to_transfer; i += sizeof (long))
    {
      long l;

      memcpy (&l, &buf[i], sizeof (l));
      errno = 0;
      ptrace (PTRACE_POKEUSER, tid, (PTRACE_TYPE_ARG3) regaddr, l);
      regaddr += sizeof (long);

      if (errno == EIO 
          && (regno == tdep->ppc_fpscr_regnum
	      || regno == PPC_ORIG_R3_REGNUM
	      || regno == PPC_TRAP_REGNUM))
	{
	  /* Some older kernel versions don't allow fpscr, orig_r3
	     or trap to be written.  */
	  continue;
	}

      if (errno != 0)
	{
          char message[128];
	  xsnprintf (message, sizeof (message), "writing register %s (#%d)",
		     gdbarch_register_name (gdbarch, regno), regno);
	  perror_with_name (message);
	}
    }
}

/* This function actually issues the request to ptrace, telling
   it to store all general-purpose registers present in the specified
   regset.
   
   If the ptrace request does not exist, this function returns 0
   and properly sets the have_ptrace_* flag.  If the request fails,
   this function calls perror_with_name.  Otherwise, if the request
   succeeds, then the regcache is stored and 1 is returned.  */
static int
store_all_gp_regs (const struct regcache *regcache, int tid, int regno)
{
  gdb_gregset_t gregset;

  if (ptrace (PTRACE_GETREGS, tid, 0, (void *) &gregset) < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getsetregs = 0;
          return 0;
        }
      perror_with_name (_("Couldn't get general-purpose registers."));
    }

  fill_gregset (regcache, &gregset, regno);

  if (ptrace (PTRACE_SETREGS, tid, 0, (void *) &gregset) < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getsetregs = 0;
          return 0;
        }
      perror_with_name (_("Couldn't set general-purpose registers."));
    }

  return 1;
}

/* This is a wrapper for the store_all_gp_regs function.  It is
   responsible for verifying if this target has the ptrace request
   that can be used to store all general-purpose registers at one
   shot.  If it doesn't, then we should store them using the
   old-fashioned way, which is to iterate over the registers and
   store them one by one.  */
static void
store_gp_regs (const struct regcache *regcache, int tid, int regno)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  int i;

  if (have_ptrace_getsetregs)
    if (store_all_gp_regs (regcache, tid, regno))
      return;

  /* If we hit this point, it doesn't really matter which
     architecture we are using.  We just need to store the
     registers in the "old-fashioned way".  */
  for (i = 0; i < ppc_num_gprs; i++)
    store_register (regcache, tid, tdep->ppc_gp0_regnum + i);
}

/* This function actually issues the request to ptrace, telling
   it to store all floating-point registers present in the specified
   regset.
   
   If the ptrace request does not exist, this function returns 0
   and properly sets the have_ptrace_* flag.  If the request fails,
   this function calls perror_with_name.  Otherwise, if the request
   succeeds, then the regcache is stored and 1 is returned.  */
static int
store_all_fp_regs (const struct regcache *regcache, int tid, int regno)
{
  gdb_fpregset_t fpregs;

  if (ptrace (PTRACE_GETFPREGS, tid, 0, (void *) &fpregs) < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getsetfpregs = 0;
          return 0;
        }
      perror_with_name (_("Couldn't get floating-point registers."));
    }

  fill_fpregset (regcache, &fpregs, regno);

  if (ptrace (PTRACE_SETFPREGS, tid, 0, (void *) &fpregs) < 0)
    {
      if (errno == EIO)
        {
          have_ptrace_getsetfpregs = 0;
          return 0;
        }
      perror_with_name (_("Couldn't set floating-point registers."));
    }

  return 1;
}

/* This is a wrapper for the store_all_fp_regs function.  It is
   responsible for verifying if this target has the ptrace request
   that can be used to store all floating-point registers at one
   shot.  If it doesn't, then we should store them using the
   old-fashioned way, which is to iterate over the registers and
   store them one by one.  */
static void
store_fp_regs (const struct regcache *regcache, int tid, int regno)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  int i;

  if (have_ptrace_getsetfpregs)
    if (store_all_fp_regs (regcache, tid, regno))
      return;

  /* If we hit this point, it doesn't really matter which
     architecture we are using.  We just need to store the
     registers in the "old-fashioned way".  */
  for (i = 0; i < ppc_num_fprs; i++)
    store_register (regcache, tid, tdep->ppc_fp0_regnum + i);
}

static void
store_ppc_registers (const struct regcache *regcache, int tid)
{
  struct gdbarch *gdbarch = regcache->arch ();
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
 
  store_gp_regs (regcache, tid, -1);
  if (tdep->ppc_fp0_regnum >= 0)
    store_fp_regs (regcache, tid, -1);
  store_register (regcache, tid, gdbarch_pc_regnum (gdbarch));
  if (tdep->ppc_ps_regnum != -1)
    store_register (regcache, tid, tdep->ppc_ps_regnum);
  if (tdep->ppc_cr_regnum != -1)
    store_register (regcache, tid, tdep->ppc_cr_regnum);
  if (tdep->ppc_lr_regnum != -1)
    store_register (regcache, tid, tdep->ppc_lr_regnum);
  if (tdep->ppc_ctr_regnum != -1)
    store_register (regcache, tid, tdep->ppc_ctr_regnum);
  if (tdep->ppc_xer_regnum != -1)
    store_register (regcache, tid, tdep->ppc_xer_regnum);
  if (tdep->ppc_mq_regnum != -1)
    store_register (regcache, tid, tdep->ppc_mq_regnum);
  if (tdep->ppc_fpscr_regnum != -1)
    store_register (regcache, tid, tdep->ppc_fpscr_regnum);
  if (ppc_linux_trap_reg_p (gdbarch))
    {
      store_register (regcache, tid, PPC_ORIG_R3_REGNUM);
      store_register (regcache, tid, PPC_TRAP_REGNUM);
    }
  if (have_ptrace_getvrregs)
    if (tdep->ppc_vr0_regnum != -1 && tdep->ppc_vrsave_regnum != -1)
      store_altivec_registers (regcache, tid, -1);
  if (have_ptrace_getsetvsxregs)
    if (tdep->ppc_vsr0_upper_regnum != -1)
      store_vsx_registers (regcache, tid, -1);
  if (tdep->ppc_ev0_upper_regnum >= 0)
    store_spe_register (regcache, tid, -1);
  if (tdep->ppc_ppr_regnum != -1)
    store_regset (regcache, tid, -1, NT_PPC_PPR,
		  PPC_LINUX_SIZEOF_PPRREGSET,
		  &ppc32_linux_pprregset);
  if (tdep->ppc_dscr_regnum != -1)
    store_regset (regcache, tid, -1, NT_PPC_DSCR,
		  PPC_LINUX_SIZEOF_DSCRREGSET,
		  &ppc32_linux_dscrregset);
  if (tdep->ppc_tar_regnum != -1)
    store_regset (regcache, tid, -1, NT_PPC_TAR,
		  PPC_LINUX_SIZEOF_TARREGSET,
		  &ppc32_linux_tarregset);

  if (tdep->ppc_mmcr0_regnum != -1)
    store_regset (regcache, tid, -1, NT_PPC_PMU,
		  PPC_LINUX_SIZEOF_PMUREGSET,
		  &ppc32_linux_pmuregset);

  if (tdep->have_htm_spr)
    store_regset (regcache, tid, -1, NT_PPC_TM_SPR,
		  PPC_LINUX_SIZEOF_TM_SPRREGSET,
		  &ppc32_linux_tm_sprregset);

  /* Because the EBB and checkpointed HTM registers can be
     unavailable, attempts to store them here would cause this
     function to fail most of the time, so we ignore them.  */
}

/* The cached DABR value, to install in new threads.
   This variable is used when the PowerPC HWDEBUG ptrace
   interface is not available.  */
static long saved_dabr_value;

/* Global structure that will store information about the available
   features provided by the PowerPC HWDEBUG ptrace interface.  */
static struct ppc_debug_info hwdebug_info;

/* Global variable that holds the maximum number of slots that the
   kernel will use.  This is only used when PowerPC HWDEBUG ptrace interface
   is available.  */
static size_t max_slots_number = 0;

struct hw_break_tuple
{
  long slot;
  struct ppc_hw_breakpoint *hw_break;
};

/* This is an internal VEC created to store information about *points inserted
   for each thread.  This is used when PowerPC HWDEBUG ptrace interface is
   available.  */
typedef struct thread_points
  {
    /* The TID to which this *point relates.  */
    int tid;
    /* Information about the *point, such as its address, type, etc.

       Each element inside this vector corresponds to a hardware
       breakpoint or watchpoint in the thread represented by TID.  The maximum
       size of these vector is MAX_SLOTS_NUMBER.  If the hw_break element of
       the tuple is NULL, then the position in the vector is free.  */
    struct hw_break_tuple *hw_breaks;
  } *thread_points_p;
DEF_VEC_P (thread_points_p);

VEC(thread_points_p) *ppc_threads = NULL;

/* The version of the PowerPC HWDEBUG kernel interface that we will use, if
   available.  */
#define PPC_DEBUG_CURRENT_VERSION 1

/* Returns non-zero if we support the PowerPC HWDEBUG ptrace interface.  */
static int
have_ptrace_hwdebug_interface (void)
{
  static int have_ptrace_hwdebug_interface = -1;

  if (have_ptrace_hwdebug_interface == -1)
    {
      int tid;

      tid = inferior_ptid.lwp ();
      if (tid == 0)
	tid = inferior_ptid.pid ();

      /* Check for kernel support for PowerPC HWDEBUG ptrace interface.  */
      if (ptrace (PPC_PTRACE_GETHWDBGINFO, tid, 0, &hwdebug_info) >= 0)
	{
	  /* Check whether PowerPC HWDEBUG ptrace interface is functional and
	     provides any supported feature.  */
	  if (hwdebug_info.features != 0)
	    {
	      have_ptrace_hwdebug_interface = 1;
	      max_slots_number = hwdebug_info.num_instruction_bps
	        + hwdebug_info.num_data_bps
	        + hwdebug_info.num_condition_regs;
	      return have_ptrace_hwdebug_interface;
	    }
	}
      /* Old school interface and no PowerPC HWDEBUG ptrace support.  */
      have_ptrace_hwdebug_interface = 0;
      memset (&hwdebug_info, 0, sizeof (struct ppc_debug_info));
    }

  return have_ptrace_hwdebug_interface;
}

int
ppc_linux_nat_target::can_use_hw_breakpoint (enum bptype type, int cnt, int ot)
{
  int total_hw_wp, total_hw_bp;

  if (have_ptrace_hwdebug_interface ())
    {
      /* When PowerPC HWDEBUG ptrace interface is available, the number of
	 available hardware watchpoints and breakpoints is stored at the
	 hwdebug_info struct.  */
      total_hw_bp = hwdebug_info.num_instruction_bps;
      total_hw_wp = hwdebug_info.num_data_bps;
    }
  else
    {
      /* When we do not have PowerPC HWDEBUG ptrace interface, we should
	 consider having 1 hardware watchpoint and no hardware breakpoints.  */
      total_hw_bp = 0;
      total_hw_wp = 1;
    }

  if (type == bp_hardware_watchpoint || type == bp_read_watchpoint
      || type == bp_access_watchpoint || type == bp_watchpoint)
    {
      if (cnt + ot > total_hw_wp)
	return -1;
    }
  else if (type == bp_hardware_breakpoint)
    {
      if (total_hw_bp == 0)
	{
	  /* No hardware breakpoint support. */
	  return 0;
	}
      if (cnt > total_hw_bp)
	return -1;
    }

  if (!have_ptrace_hwdebug_interface ())
    {
      int tid;
      ptid_t ptid = inferior_ptid;

      /* We need to know whether ptrace supports PTRACE_SET_DEBUGREG
	 and whether the target has DABR.  If either answer is no, the
	 ptrace call will return -1.  Fail in that case.  */
      tid = ptid.lwp ();
      if (tid == 0)
	tid = ptid.pid ();

      if (ptrace (PTRACE_SET_DEBUGREG, tid, 0, 0) == -1)
	return 0;
    }

  return 1;
}

int
ppc_linux_nat_target::region_ok_for_hw_watchpoint (CORE_ADDR addr, LONGEST len)
{
  /* Handle sub-8-byte quantities.  */
  if (len <= 0)
    return 0;

  /* The PowerPC HWDEBUG ptrace interface tells if there are alignment
     restrictions for watchpoints in the processors.  In that case, we use that
     information to determine the hardcoded watchable region for
     watchpoints.  */
  if (have_ptrace_hwdebug_interface ())
    {
      int region_size;
      /* Embedded DAC-based processors, like the PowerPC 440 have ranged
	 watchpoints and can watch any access within an arbitrary memory
	 region. This is useful to watch arrays and structs, for instance.  It
         takes two hardware watchpoints though.  */
      if (len > 1
	  && hwdebug_info.features & PPC_DEBUG_FEATURE_DATA_BP_RANGE
	  && linux_get_hwcap (current_top_target ()) & PPC_FEATURE_BOOKE)
	return 2;
      /* Check if the processor provides DAWR interface.  */
      if (hwdebug_info.features & PPC_DEBUG_FEATURE_DATA_BP_DAWR)
	/* DAWR interface allows to watch up to 512 byte wide ranges which
	   can't cross a 512 byte boundary.  */
	region_size = 512;
      else
	region_size = hwdebug_info.data_bp_alignment;
      /* Server processors provide one hardware watchpoint and addr+len should
         fall in the watchable region provided by the ptrace interface.  */
      if (region_size
	  && (addr + len > (addr & ~(region_size - 1)) + region_size))
	return 0;
    }
  /* addr+len must fall in the 8 byte watchable region for DABR-based
     processors (i.e., server processors).  Without the new PowerPC HWDEBUG 
     ptrace interface, DAC-based processors (i.e., embedded processors) will
     use addresses aligned to 4-bytes due to the way the read/write flags are
     passed in the old ptrace interface.  */
  else if (((linux_get_hwcap (current_top_target ()) & PPC_FEATURE_BOOKE)
	   && (addr + len) > (addr & ~3) + 4)
	   || (addr + len) > (addr & ~7) + 8)
    return 0;

  return 1;
}

/* This function compares two ppc_hw_breakpoint structs field-by-field.  */
static int
hwdebug_point_cmp (struct ppc_hw_breakpoint *a, struct ppc_hw_breakpoint *b)
{
  return (a->trigger_type == b->trigger_type
	  && a->addr_mode == b->addr_mode
	  && a->condition_mode == b->condition_mode
	  && a->addr == b->addr
	  && a->addr2 == b->addr2
	  && a->condition_value == b->condition_value);
}

/* This function can be used to retrieve a thread_points by the TID of the
   related process/thread.  If nothing has been found, and ALLOC_NEW is 0,
   it returns NULL.  If ALLOC_NEW is non-zero, a new thread_points for the
   provided TID will be created and returned.  */
static struct thread_points *
hwdebug_find_thread_points_by_tid (int tid, int alloc_new)
{
  int i;
  struct thread_points *t;

  for (i = 0; VEC_iterate (thread_points_p, ppc_threads, i, t); i++)
    if (t->tid == tid)
      return t;

  t = NULL;

  /* Do we need to allocate a new point_item
     if the wanted one does not exist?  */
  if (alloc_new)
    {
      t = XNEW (struct thread_points);
      t->hw_breaks = XCNEWVEC (struct hw_break_tuple, max_slots_number);
      t->tid = tid;
      VEC_safe_push (thread_points_p, ppc_threads, t);
    }

  return t;
}

/* This function is a generic wrapper that is responsible for inserting a
   *point (i.e., calling `ptrace' in order to issue the request to the
   kernel) and registering it internally in GDB.  */
static void
hwdebug_insert_point (struct ppc_hw_breakpoint *b, int tid)
{
  int i;
  long slot;
  gdb::unique_xmalloc_ptr<ppc_hw_breakpoint> p (XDUP (ppc_hw_breakpoint, b));
  struct hw_break_tuple *hw_breaks;
  struct thread_points *t;

  errno = 0;
  slot = ptrace (PPC_PTRACE_SETHWDEBUG, tid, 0, p.get ());
  if (slot < 0)
    perror_with_name (_("Unexpected error setting breakpoint or watchpoint"));

  /* Everything went fine, so we have to register this *point.  */
  t = hwdebug_find_thread_points_by_tid (tid, 1);
  gdb_assert (t != NULL);
  hw_breaks = t->hw_breaks;

  /* Find a free element in the hw_breaks vector.  */
  for (i = 0; i < max_slots_number; i++)
    if (hw_breaks[i].hw_break == NULL)
      {
	hw_breaks[i].slot = slot;
	hw_breaks[i].hw_break = p.release ();
	break;
      }

  gdb_assert (i != max_slots_number);
}

/* This function is a generic wrapper that is responsible for removing a
   *point (i.e., calling `ptrace' in order to issue the request to the
   kernel), and unregistering it internally at GDB.  */
static void
hwdebug_remove_point (struct ppc_hw_breakpoint *b, int tid)
{
  int i;
  struct hw_break_tuple *hw_breaks;
  struct thread_points *t;

  t = hwdebug_find_thread_points_by_tid (tid, 0);
  gdb_assert (t != NULL);
  hw_breaks = t->hw_breaks;

  for (i = 0; i < max_slots_number; i++)
    if (hw_breaks[i].hw_break && hwdebug_point_cmp (hw_breaks[i].hw_break, b))
      break;

  gdb_assert (i != max_slots_number);

  /* We have to ignore ENOENT errors because the kernel implements hardware
     breakpoints/watchpoints as "one-shot", that is, they are automatically
     deleted when hit.  */
  errno = 0;
  if (ptrace (PPC_PTRACE_DELHWDEBUG, tid, 0, hw_breaks[i].slot) < 0)
    if (errno != ENOENT)
      perror_with_name (_("Unexpected error deleting "
			  "breakpoint or watchpoint"));

  xfree (hw_breaks[i].hw_break);
  hw_breaks[i].hw_break = NULL;
}

/* Return the number of registers needed for a ranged breakpoint.  */

int
ppc_linux_nat_target::ranged_break_num_registers ()
{
  return ((have_ptrace_hwdebug_interface ()
	   && hwdebug_info.features & PPC_DEBUG_FEATURE_INSN_BP_RANGE)?
	  2 : -1);
}

/* Insert the hardware breakpoint described by BP_TGT.  Returns 0 for
   success, 1 if hardware breakpoints are not supported or -1 for failure.  */

int
ppc_linux_nat_target::insert_hw_breakpoint (struct gdbarch *gdbarch,
					    struct bp_target_info *bp_tgt)
{
  struct lwp_info *lp;
  struct ppc_hw_breakpoint p;

  if (!have_ptrace_hwdebug_interface ())
    return -1;

  p.version = PPC_DEBUG_CURRENT_VERSION;
  p.trigger_type = PPC_BREAKPOINT_TRIGGER_EXECUTE;
  p.condition_mode = PPC_BREAKPOINT_CONDITION_NONE;
  p.addr = (uint64_t) (bp_tgt->placed_address = bp_tgt->reqstd_address);
  p.condition_value = 0;

  if (bp_tgt->length)
    {
      p.addr_mode = PPC_BREAKPOINT_MODE_RANGE_INCLUSIVE;

      /* The breakpoint will trigger if the address of the instruction is
	 within the defined range, as follows: p.addr <= address < p.addr2.  */
      p.addr2 = (uint64_t) bp_tgt->placed_address + bp_tgt->length;
    }
  else
    {
      p.addr_mode = PPC_BREAKPOINT_MODE_EXACT;
      p.addr2 = 0;
    }

  ALL_LWPS (lp)
    hwdebug_insert_point (&p, lp->ptid.lwp ());

  return 0;
}

int
ppc_linux_nat_target::remove_hw_breakpoint (struct gdbarch *gdbarch,
					    struct bp_target_info *bp_tgt)
{
  struct lwp_info *lp;
  struct ppc_hw_breakpoint p;

  if (!have_ptrace_hwdebug_interface ())
    return -1;

  p.version = PPC_DEBUG_CURRENT_VERSION;
  p.trigger_type = PPC_BREAKPOINT_TRIGGER_EXECUTE;
  p.condition_mode = PPC_BREAKPOINT_CONDITION_NONE;
  p.addr = (uint64_t) bp_tgt->placed_address;
  p.condition_value = 0;

  if (bp_tgt->length)
    {
      p.addr_mode = PPC_BREAKPOINT_MODE_RANGE_INCLUSIVE;

      /* The breakpoint will trigger if the address of the instruction is within
	 the defined range, as follows: p.addr <= address < p.addr2.  */
      p.addr2 = (uint64_t) bp_tgt->placed_address + bp_tgt->length;
    }
  else
    {
      p.addr_mode = PPC_BREAKPOINT_MODE_EXACT;
      p.addr2 = 0;
    }

  ALL_LWPS (lp)
    hwdebug_remove_point (&p, lp->ptid.lwp ());

  return 0;
}

static int
get_trigger_type (enum target_hw_bp_type type)
{
  int t;

  if (type == hw_read)
    t = PPC_BREAKPOINT_TRIGGER_READ;
  else if (type == hw_write)
    t = PPC_BREAKPOINT_TRIGGER_WRITE;
  else
    t = PPC_BREAKPOINT_TRIGGER_READ | PPC_BREAKPOINT_TRIGGER_WRITE;

  return t;
}

/* Insert a new masked watchpoint at ADDR using the mask MASK.
   RW may be hw_read for a read watchpoint, hw_write for a write watchpoint
   or hw_access for an access watchpoint.  Returns 0 on success and throws
   an error on failure.  */

int
ppc_linux_nat_target::insert_mask_watchpoint (CORE_ADDR addr,  CORE_ADDR mask,
					      target_hw_bp_type rw)
{
  struct lwp_info *lp;
  struct ppc_hw_breakpoint p;

  gdb_assert (have_ptrace_hwdebug_interface ());

  p.version = PPC_DEBUG_CURRENT_VERSION;
  p.trigger_type = get_trigger_type (rw);
  p.addr_mode = PPC_BREAKPOINT_MODE_MASK;
  p.condition_mode = PPC_BREAKPOINT_CONDITION_NONE;
  p.addr = addr;
  p.addr2 = mask;
  p.condition_value = 0;

  ALL_LWPS (lp)
    hwdebug_insert_point (&p, lp->ptid.lwp ());

  return 0;
}

/* Remove a masked watchpoint at ADDR with the mask MASK.
   RW may be hw_read for a read watchpoint, hw_write for a write watchpoint
   or hw_access for an access watchpoint.  Returns 0 on success and throws
   an error on failure.  */

int
ppc_linux_nat_target::remove_mask_watchpoint (CORE_ADDR addr, CORE_ADDR mask,
					      target_hw_bp_type rw)
{
  struct lwp_info *lp;
  struct ppc_hw_breakpoint p;

  gdb_assert (have_ptrace_hwdebug_interface ());

  p.version = PPC_DEBUG_CURRENT_VERSION;
  p.trigger_type = get_trigger_type (rw);
  p.addr_mode = PPC_BREAKPOINT_MODE_MASK;
  p.condition_mode = PPC_BREAKPOINT_CONDITION_NONE;
  p.addr = addr;
  p.addr2 = mask;
  p.condition_value = 0;

  ALL_LWPS (lp)
    hwdebug_remove_point (&p, lp->ptid.lwp ());

  return 0;
}

/* Check whether we have at least one free DVC register.  */
static int
can_use_watchpoint_cond_accel (void)
{
  struct thread_points *p;
  int tid = inferior_ptid.lwp ();
  int cnt = hwdebug_info.num_condition_regs, i;

  if (!have_ptrace_hwdebug_interface () || cnt == 0)
    return 0;

  p = hwdebug_find_thread_points_by_tid (tid, 0);

  if (p)
    {
      for (i = 0; i < max_slots_number; i++)
	if (p->hw_breaks[i].hw_break != NULL
	    && (p->hw_breaks[i].hw_break->condition_mode
		!= PPC_BREAKPOINT_CONDITION_NONE))
	  cnt--;

      /* There are no available slots now.  */
      if (cnt <= 0)
	return 0;
    }

  return 1;
}

/* Calculate the enable bits and the contents of the Data Value Compare
   debug register present in BookE processors.

   ADDR is the address to be watched, LEN is the length of watched data
   and DATA_VALUE is the value which will trigger the watchpoint.
   On exit, CONDITION_MODE will hold the enable bits for the DVC, and
   CONDITION_VALUE will hold the value which should be put in the
   DVC register.  */
static void
calculate_dvc (CORE_ADDR addr, LONGEST len, CORE_ADDR data_value,
	       uint32_t *condition_mode, uint64_t *condition_value)
{
  LONGEST i, num_byte_enable;
  int align_offset, num_bytes_off_dvc, rightmost_enabled_byte;
  CORE_ADDR addr_end_data, addr_end_dvc;

  /* The DVC register compares bytes within fixed-length windows which
     are word-aligned, with length equal to that of the DVC register.
     We need to calculate where our watch region is relative to that
     window and enable comparison of the bytes which fall within it.  */

  align_offset = addr % hwdebug_info.sizeof_condition;
  addr_end_data = addr + len;
  addr_end_dvc = (addr - align_offset
		  + hwdebug_info.sizeof_condition);
  num_bytes_off_dvc = (addr_end_data > addr_end_dvc)?
			 addr_end_data - addr_end_dvc : 0;
  num_byte_enable = len - num_bytes_off_dvc;
  /* Here, bytes are numbered from right to left.  */
  rightmost_enabled_byte = (addr_end_data < addr_end_dvc)?
			      addr_end_dvc - addr_end_data : 0;

  *condition_mode = PPC_BREAKPOINT_CONDITION_AND;
  for (i = 0; i < num_byte_enable; i++)
    *condition_mode
      |= PPC_BREAKPOINT_CONDITION_BE (i + rightmost_enabled_byte);

  /* Now we need to match the position within the DVC of the comparison
     value with where the watch region is relative to the window
     (i.e., the ALIGN_OFFSET).  */

  *condition_value = ((uint64_t) data_value >> num_bytes_off_dvc * 8
		      << rightmost_enabled_byte * 8);
}

/* Return the number of memory locations that need to be accessed to
   evaluate the expression which generated the given value chain.
   Returns -1 if there's any register access involved, or if there are
   other kinds of values which are not acceptable in a condition
   expression (e.g., lval_computed or lval_internalvar).  */
static int
num_memory_accesses (const std::vector<value_ref_ptr> &chain)
{
  int found_memory_cnt = 0;

  /* The idea here is that evaluating an expression generates a series
     of values, one holding the value of every subexpression.  (The
     expression a*b+c has five subexpressions: a, b, a*b, c, and
     a*b+c.)  GDB's values hold almost enough information to establish
     the criteria given above --- they identify memory lvalues,
     register lvalues, computed values, etcetera.  So we can evaluate
     the expression, and then scan the chain of values that leaves
     behind to determine the memory locations involved in the evaluation
     of an expression.

     However, I don't think that the values returned by inferior
     function calls are special in any way.  So this function may not
     notice that an expression contains an inferior function call.
     FIXME.  */

  for (const value_ref_ptr &iter : chain)
    {
      struct value *v = iter.get ();

      /* Constants and values from the history are fine.  */
      if (VALUE_LVAL (v) == not_lval || deprecated_value_modifiable (v) == 0)
	continue;
      else if (VALUE_LVAL (v) == lval_memory)
	{
	  /* A lazy memory lvalue is one that GDB never needed to fetch;
	     we either just used its address (e.g., `a' in `a.b') or
	     we never needed it at all (e.g., `a' in `a,b').  */
	  if (!value_lazy (v))
	    found_memory_cnt++;
	}
      /* Other kinds of values are not fine.  */
      else
	return -1;
    }

  return found_memory_cnt;
}

/* Verifies whether the expression COND can be implemented using the
   DVC (Data Value Compare) register in BookE processors.  The expression
   must test the watch value for equality with a constant expression.
   If the function returns 1, DATA_VALUE will contain the constant against
   which the watch value should be compared and LEN will contain the size
   of the constant.  */
static int
check_condition (CORE_ADDR watch_addr, struct expression *cond,
		 CORE_ADDR *data_value, LONGEST *len)
{
  int pc = 1, num_accesses_left, num_accesses_right;
  struct value *left_val, *right_val;
  std::vector<value_ref_ptr> left_chain, right_chain;

  if (cond->elts[0].opcode != BINOP_EQUAL)
    return 0;

  fetch_subexp_value (cond, &pc, &left_val, NULL, &left_chain, 0);
  num_accesses_left = num_memory_accesses (left_chain);

  if (left_val == NULL || num_accesses_left < 0)
    return 0;

  fetch_subexp_value (cond, &pc, &right_val, NULL, &right_chain, 0);
  num_accesses_right = num_memory_accesses (right_chain);

  if (right_val == NULL || num_accesses_right < 0)
    return 0;

  if (num_accesses_left == 1 && num_accesses_right == 0
      && VALUE_LVAL (left_val) == lval_memory
      && value_address (left_val) == watch_addr)
    {
      *data_value = value_as_long (right_val);

      /* DATA_VALUE is the constant in RIGHT_VAL, but actually has
	 the same type as the memory region referenced by LEFT_VAL.  */
      *len = TYPE_LENGTH (check_typedef (value_type (left_val)));
    }
  else if (num_accesses_left == 0 && num_accesses_right == 1
	   && VALUE_LVAL (right_val) == lval_memory
	   && value_address (right_val) == watch_addr)
    {
      *data_value = value_as_long (left_val);

      /* DATA_VALUE is the constant in LEFT_VAL, but actually has
	 the same type as the memory region referenced by RIGHT_VAL.  */
      *len = TYPE_LENGTH (check_typedef (value_type (right_val)));
    }
  else
    return 0;

  return 1;
}

/* Return non-zero if the target is capable of using hardware to evaluate
   the condition expression, thus only triggering the watchpoint when it is
   true.  */
bool
ppc_linux_nat_target::can_accel_watchpoint_condition (CORE_ADDR addr,
						      LONGEST len,
						      int rw,
						      struct expression *cond)
{
  CORE_ADDR data_value;

  return (have_ptrace_hwdebug_interface ()
	  && hwdebug_info.num_condition_regs > 0
	  && check_condition (addr, cond, &data_value, &len));
}

/* Set up P with the parameters necessary to request a watchpoint covering
   LEN bytes starting at ADDR and if possible with condition expression COND
   evaluated by hardware.  INSERT tells if we are creating a request for
   inserting or removing the watchpoint.  */

static void
create_watchpoint_request (struct ppc_hw_breakpoint *p, CORE_ADDR addr,
			   LONGEST len, enum target_hw_bp_type type,
			   struct expression *cond, int insert)
{
  if (len == 1
      || !(hwdebug_info.features & PPC_DEBUG_FEATURE_DATA_BP_RANGE))
    {
      int use_condition;
      CORE_ADDR data_value;

      use_condition = (insert? can_use_watchpoint_cond_accel ()
			: hwdebug_info.num_condition_regs > 0);
      if (cond && use_condition && check_condition (addr, cond,
						    &data_value, &len))
	calculate_dvc (addr, len, data_value, &p->condition_mode,
		       &p->condition_value);
      else
	{
	  p->condition_mode = PPC_BREAKPOINT_CONDITION_NONE;
	  p->condition_value = 0;
	}

      p->addr_mode = PPC_BREAKPOINT_MODE_EXACT;
      p->addr2 = 0;
    }
  else
    {
      p->addr_mode = PPC_BREAKPOINT_MODE_RANGE_INCLUSIVE;
      p->condition_mode = PPC_BREAKPOINT_CONDITION_NONE;
      p->condition_value = 0;

      /* The watchpoint will trigger if the address of the memory access is
	 within the defined range, as follows: p->addr <= address < p->addr2.

	 Note that the above sentence just documents how ptrace interprets
	 its arguments; the watchpoint is set to watch the range defined by
	 the user _inclusively_, as specified by the user interface.  */
      p->addr2 = (uint64_t) addr + len;
    }

  p->version = PPC_DEBUG_CURRENT_VERSION;
  p->trigger_type = get_trigger_type (type);
  p->addr = (uint64_t) addr;
}

int
ppc_linux_nat_target::insert_watchpoint (CORE_ADDR addr, int len,
					 enum target_hw_bp_type type,
					 struct expression *cond)
{
  struct lwp_info *lp;
  int ret = -1;

  if (have_ptrace_hwdebug_interface ())
    {
      struct ppc_hw_breakpoint p;

      create_watchpoint_request (&p, addr, len, type, cond, 1);

      ALL_LWPS (lp)
	hwdebug_insert_point (&p, lp->ptid.lwp ());

      ret = 0;
    }
  else
    {
      long dabr_value;
      long read_mode, write_mode;

      if (linux_get_hwcap (current_top_target ()) & PPC_FEATURE_BOOKE)
	{
	  /* PowerPC 440 requires only the read/write flags to be passed
	     to the kernel.  */
	  read_mode = 1;
	  write_mode = 2;
	}
      else
	{
	  /* PowerPC 970 and other DABR-based processors are required to pass
	     the Breakpoint Translation bit together with the flags.  */
	  read_mode = 5;
	  write_mode = 6;
	}

      dabr_value = addr & ~(read_mode | write_mode);
      switch (type)
	{
	  case hw_read:
	    /* Set read and translate bits.  */
	    dabr_value |= read_mode;
	    break;
	  case hw_write:
	    /* Set write and translate bits.  */
	    dabr_value |= write_mode;
	    break;
	  case hw_access:
	    /* Set read, write and translate bits.  */
	    dabr_value |= read_mode | write_mode;
	    break;
	}

      saved_dabr_value = dabr_value;

      ALL_LWPS (lp)
	if (ptrace (PTRACE_SET_DEBUGREG, lp->ptid.lwp (), 0,
		    saved_dabr_value) < 0)
	  return -1;

      ret = 0;
    }

  return ret;
}

int
ppc_linux_nat_target::remove_watchpoint (CORE_ADDR addr, int len,
					 enum target_hw_bp_type type,
					 struct expression *cond)
{
  struct lwp_info *lp;
  int ret = -1;

  if (have_ptrace_hwdebug_interface ())
    {
      struct ppc_hw_breakpoint p;

      create_watchpoint_request (&p, addr, len, type, cond, 0);

      ALL_LWPS (lp)
	hwdebug_remove_point (&p, lp->ptid.lwp ());

      ret = 0;
    }
  else
    {
      saved_dabr_value = 0;
      ALL_LWPS (lp)
	if (ptrace (PTRACE_SET_DEBUGREG, lp->ptid.lwp (), 0,
		    saved_dabr_value) < 0)
	  return -1;

      ret = 0;
    }

  return ret;
}

void
ppc_linux_nat_target::low_new_thread (struct lwp_info *lp)
{
  int tid = lp->ptid.lwp ();

  if (have_ptrace_hwdebug_interface ())
    {
      int i;
      struct thread_points *p;
      struct hw_break_tuple *hw_breaks;

      if (VEC_empty (thread_points_p, ppc_threads))
	return;

      /* Get a list of breakpoints from any thread.  */
      p = VEC_last (thread_points_p, ppc_threads);
      hw_breaks = p->hw_breaks;

      /* Copy that thread's breakpoints and watchpoints to the new thread.  */
      for (i = 0; i < max_slots_number; i++)
	if (hw_breaks[i].hw_break)
	  {
	    /* Older kernels did not make new threads inherit their parent
	       thread's debug state, so we always clear the slot and replicate
	       the debug state ourselves, ensuring compatibility with all
	       kernels.  */

	    /* The ppc debug resource accounting is done through "slots".
	       Ask the kernel the deallocate this specific *point's slot.  */
	    ptrace (PPC_PTRACE_DELHWDEBUG, tid, 0, hw_breaks[i].slot);

	    hwdebug_insert_point (hw_breaks[i].hw_break, tid);
	  }
    }
  else
    ptrace (PTRACE_SET_DEBUGREG, tid, 0, saved_dabr_value);
}

static void
ppc_linux_thread_exit (struct thread_info *tp, int silent)
{
  int i;
  int tid = tp->ptid.lwp ();
  struct hw_break_tuple *hw_breaks;
  struct thread_points *t = NULL, *p;

  if (!have_ptrace_hwdebug_interface ())
    return;

  for (i = 0; VEC_iterate (thread_points_p, ppc_threads, i, p); i++)
    if (p->tid == tid)
      {
	t = p;
	break;
      }

  if (t == NULL)
    return;

  VEC_unordered_remove (thread_points_p, ppc_threads, i);

  hw_breaks = t->hw_breaks;

  for (i = 0; i < max_slots_number; i++)
    if (hw_breaks[i].hw_break)
      xfree (hw_breaks[i].hw_break);

  xfree (t->hw_breaks);
  xfree (t);
}

bool
ppc_linux_nat_target::stopped_data_address (CORE_ADDR *addr_p)
{
  siginfo_t siginfo;

  if (!linux_nat_get_siginfo (inferior_ptid, &siginfo))
    return false;

  if (siginfo.si_signo != SIGTRAP
      || (siginfo.si_code & 0xffff) != 0x0004 /* TRAP_HWBKPT */)
    return false;

  if (have_ptrace_hwdebug_interface ())
    {
      int i;
      struct thread_points *t;
      struct hw_break_tuple *hw_breaks;
      /* The index (or slot) of the *point is passed in the si_errno field.  */
      int slot = siginfo.si_errno;

      t = hwdebug_find_thread_points_by_tid (inferior_ptid.lwp (), 0);

      /* Find out if this *point is a hardware breakpoint.
	 If so, we should return 0.  */
      if (t)
	{
	  hw_breaks = t->hw_breaks;
	  for (i = 0; i < max_slots_number; i++)
	   if (hw_breaks[i].hw_break && hw_breaks[i].slot == slot
	       && hw_breaks[i].hw_break->trigger_type
		    == PPC_BREAKPOINT_TRIGGER_EXECUTE)
	     return false;
	}
    }

  *addr_p = (CORE_ADDR) (uintptr_t) siginfo.si_addr;
  return true;
}

bool
ppc_linux_nat_target::stopped_by_watchpoint ()
{
  CORE_ADDR addr;
  return stopped_data_address (&addr);
}

bool
ppc_linux_nat_target::watchpoint_addr_within_range (CORE_ADDR addr,
						    CORE_ADDR start,
						    LONGEST length)
{
  int mask;

  if (have_ptrace_hwdebug_interface ()
      && linux_get_hwcap (current_top_target ()) & PPC_FEATURE_BOOKE)
    return start <= addr && start + length >= addr;
  else if (linux_get_hwcap (current_top_target ()) & PPC_FEATURE_BOOKE)
    mask = 3;
  else
    mask = 7;

  addr &= ~mask;

  /* Check whether [start, start+length-1] intersects [addr, addr+mask].  */
  return start <= addr + mask && start + length - 1 >= addr;
}

/* Return the number of registers needed for a masked hardware watchpoint.  */

int
ppc_linux_nat_target::masked_watch_num_registers (CORE_ADDR addr, CORE_ADDR mask)
{
  if (!have_ptrace_hwdebug_interface ()
	   || (hwdebug_info.features & PPC_DEBUG_FEATURE_DATA_BP_MASK) == 0)
    return -1;
  else if ((mask & 0xC0000000) != 0xC0000000)
    {
      warning (_("The given mask covers kernel address space "
		 "and cannot be used.\n"));

      return -2;
    }
  else
    return 2;
}

void
ppc_linux_nat_target::store_registers (struct regcache *regcache, int regno)
{
  pid_t tid = get_ptrace_pid (regcache->ptid ());

  if (regno >= 0)
    store_register (regcache, tid, regno);
  else
    store_ppc_registers (regcache, tid);
}

/* Functions for transferring registers between a gregset_t or fpregset_t
   (see sys/ucontext.h) and gdb's regcache.  The word size is that used
   by the ptrace interface, not the current program's ABI.  Eg. if a
   powerpc64-linux gdb is being used to debug a powerpc32-linux app, we
   read or write 64-bit gregsets.  This is to suit the host libthread_db.  */

void
supply_gregset (struct regcache *regcache, const gdb_gregset_t *gregsetp)
{
  const struct regset *regset = ppc_linux_gregset (sizeof (long));

  ppc_supply_gregset (regset, regcache, -1, gregsetp, sizeof (*gregsetp));
}

void
fill_gregset (const struct regcache *regcache,
	      gdb_gregset_t *gregsetp, int regno)
{
  const struct regset *regset = ppc_linux_gregset (sizeof (long));

  if (regno == -1)
    memset (gregsetp, 0, sizeof (*gregsetp));
  ppc_collect_gregset (regset, regcache, regno, gregsetp, sizeof (*gregsetp));
}

void
supply_fpregset (struct regcache *regcache, const gdb_fpregset_t * fpregsetp)
{
  const struct regset *regset = ppc_linux_fpregset ();

  ppc_supply_fpregset (regset, regcache, -1,
		       fpregsetp, sizeof (*fpregsetp));
}

void
fill_fpregset (const struct regcache *regcache,
	       gdb_fpregset_t *fpregsetp, int regno)
{
  const struct regset *regset = ppc_linux_fpregset ();

  ppc_collect_fpregset (regset, regcache, regno,
			fpregsetp, sizeof (*fpregsetp));
}

int
ppc_linux_nat_target::auxv_parse (gdb_byte **readptr,
				  gdb_byte *endptr, CORE_ADDR *typep,
				  CORE_ADDR *valp)
{
  int tid = inferior_ptid.lwp ();
  if (tid == 0)
    tid = inferior_ptid.pid ();

  int sizeof_auxv_field = ppc_linux_target_wordsize (tid);

  enum bfd_endian byte_order = gdbarch_byte_order (target_gdbarch ());
  gdb_byte *ptr = *readptr;

  if (endptr == ptr)
    return 0;

  if (endptr - ptr < sizeof_auxv_field * 2)
    return -1;

  *typep = extract_unsigned_integer (ptr, sizeof_auxv_field, byte_order);
  ptr += sizeof_auxv_field;
  *valp = extract_unsigned_integer (ptr, sizeof_auxv_field, byte_order);
  ptr += sizeof_auxv_field;

  *readptr = ptr;
  return 1;
}

const struct target_desc *
ppc_linux_nat_target::read_description ()
{
  int tid = inferior_ptid.lwp ();
  if (tid == 0)
    tid = inferior_ptid.pid ();

  if (have_ptrace_getsetevrregs)
    {
      struct gdb_evrregset_t evrregset;

      if (ptrace (PTRACE_GETEVRREGS, tid, 0, &evrregset) >= 0)
        return tdesc_powerpc_e500l;

      /* EIO means that the PTRACE_GETEVRREGS request isn't supported.
	 Anything else needs to be reported.  */
      else if (errno != EIO)
	perror_with_name (_("Unable to fetch SPE registers"));
    }

  struct ppc_linux_features features = ppc_linux_no_features;

  features.wordsize = ppc_linux_target_wordsize (tid);

  CORE_ADDR hwcap = linux_get_hwcap (current_top_target ());
  CORE_ADDR hwcap2 = linux_get_hwcap2 (current_top_target ());

  if (have_ptrace_getsetvsxregs
      && (hwcap & PPC_FEATURE_HAS_VSX))
    {
      gdb_vsxregset_t vsxregset;

      if (ptrace (PTRACE_GETVSXREGS, tid, 0, &vsxregset) >= 0)
	features.vsx = true;

      /* EIO means that the PTRACE_GETVSXREGS request isn't supported.
	 Anything else needs to be reported.  */
      else if (errno != EIO)
	perror_with_name (_("Unable to fetch VSX registers"));
    }

  if (have_ptrace_getvrregs
      && (hwcap & PPC_FEATURE_HAS_ALTIVEC))
    {
      gdb_vrregset_t vrregset;

      if (ptrace (PTRACE_GETVRREGS, tid, 0, &vrregset) >= 0)
        features.altivec = true;

      /* EIO means that the PTRACE_GETVRREGS request isn't supported.
	 Anything else needs to be reported.  */
      else if (errno != EIO)
	perror_with_name (_("Unable to fetch AltiVec registers"));
    }

  if (hwcap & PPC_FEATURE_CELL)
    features.cell = true;

  features.isa205 = ppc_linux_has_isa205 (hwcap);

  if ((hwcap2 & PPC_FEATURE2_DSCR)
      && check_regset (tid, NT_PPC_PPR, PPC_LINUX_SIZEOF_PPRREGSET)
      && check_regset (tid, NT_PPC_DSCR, PPC_LINUX_SIZEOF_DSCRREGSET))
    {
      features.ppr_dscr = true;
      if ((hwcap2 & PPC_FEATURE2_ARCH_2_07)
	  && (hwcap2 & PPC_FEATURE2_TAR)
	  && (hwcap2 & PPC_FEATURE2_EBB)
	  && check_regset (tid, NT_PPC_TAR, PPC_LINUX_SIZEOF_TARREGSET)
	  && check_regset (tid, NT_PPC_EBB, PPC_LINUX_SIZEOF_EBBREGSET)
	  && check_regset (tid, NT_PPC_PMU, PPC_LINUX_SIZEOF_PMUREGSET))
	{
	  features.isa207 = true;
	  if ((hwcap2 & PPC_FEATURE2_HTM)
	      && check_regset (tid, NT_PPC_TM_SPR,
			       PPC_LINUX_SIZEOF_TM_SPRREGSET))
	    features.htm = true;
	}
    }

  return ppc_linux_match_description (features);
}

void
_initialize_ppc_linux_nat (void)
{
  linux_target = &the_ppc_linux_nat_target;

  gdb::observers::thread_exit.attach (ppc_linux_thread_exit);

  /* Register the target.  */
  add_inf_child_target (linux_target);
}
