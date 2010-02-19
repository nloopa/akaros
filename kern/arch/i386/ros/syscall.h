#ifndef _ROS_ARCH_SYSCALL_H
#define _ROS_ARCH_SYSCALL_H

#define T_SYSCALL	0x80

#ifndef ROS_KERNEL

// TODO: fix sysenter to take all 5 params
static inline intreg_t syscall_sysenter(uint16_t num, intreg_t a1,
                                 intreg_t a2, intreg_t a3,
                                 intreg_t a4, intreg_t a5)
{
	intreg_t ret;
	asm volatile ("  pushl %%ebp;        "
	              "  pushl %%esi;        "
	              "  movl %%esp, %%ebp;  "
	              "  leal 1f, %%esi;     "
	              "  sysenter;           "
	              "1:                    "
	              "  popl %%esi;         "
	              "  popl %%ebp;         "
	              : "=a" (ret)
	              : "a" (num),
	                "d" (a1),
	                "c" (a2),
	                "b" (a3),
	                "D" (a4)
	              : "cc", "memory");
	return ret;
}

static inline intreg_t syscall_trap(uint16_t num, intreg_t a1,
                             intreg_t a2, intreg_t a3,
                             intreg_t a4, intreg_t a5)
{
	uint32_t ret;

	// Generic system call: pass system call number in AX,
	// up to five parameters in DX, CX, BX, DI, SI.
	// Interrupt kernel with T_SYSCALL.
	//
	// The "volatile" tells the assembler not to optimize
	// this instruction away just because we don't use the
	// return value.
	//
	// The last clause tells the assembler that this can
	// potentially change the condition codes and arbitrary
	// memory locations.

	asm volatile("int %1"
	             : "=a" (ret)
	             : "i" (T_SYSCALL),
	               "a" (num),
	               "d" (a1),
	               "c" (a2),
	               "b" (a3),
	               "D" (a4),
	               "S" (a5)
	             : "cc", "memory");
	return ret;
}

static inline long __attribute__((always_inline))
__ros_syscall(long _num, long _a0, long _a1, long _a2, long _a3, long _a4)
{
	#ifndef SYSCALL_TRAP
		return syscall_sysenter(_num, _a1, _a2, _a3, _a4, _a5);
	#else
		return syscall_trap(_num, _a1, _a2, _a3, _a4, _a5);
	#endif
}

#endif

#endif

