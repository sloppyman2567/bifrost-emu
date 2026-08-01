/* vdso.S — minimal AArch64 vDSO for bifrost-emu.
 *
 * A vDSO is a small shared object the kernel maps into the guest and
 * exposes via AT_SYSINFO_EHDR. glibc/musl look up gettimeofday /
 * clock_gettime / clock_getres / __kernel_rt_sigreturn in it and call
 * those instead of a syscall.
 *
 * Each function here is a tiny stub that simply performs the real
 * syscall (mov x8,#num; svc #0; ret). This keeps the vDSO functionally
 * correct regardless of which path libc takes: if libc uses the vDSO,
 * the stub traps to the emulator's syscall handler; if it falls back to
 * a direct syscall, the same handler runs. Either way the result is
 * correct.
 *
 * AArch64 syscall numbers: gettimeofday=169, clock_gettime=113,
 * clock_getres=114, rt_sigreturn=139.
 */
	.text
	.globl gettimeofday
	.type gettimeofday, %function
gettimeofday:
	mov	x8, #169
	svc	#0
	ret
	.size gettimeofday, .-gettimeofday

	.globl clock_gettime
	.type clock_gettime, %function
clock_gettime:
	mov	x8, #113
	svc	#0
	ret
	.size clock_gettime, .-clock_gettime

	.globl clock_getres
	.type clock_getres, %function
clock_getres:
	mov	x8, #114
	svc	#0
	ret
	.size clock_getres, .-clock_getres

	.globl __kernel_rt_sigreturn
	.type __kernel_rt_sigreturn, %function
__kernel_rt_sigreturn:
	mov	x8, #139
	svc	#0
	ret
	.size __kernel_rt_sigreturn, .-__kernel_rt_sigreturn
