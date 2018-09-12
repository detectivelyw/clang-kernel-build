	.text
	.file	"bounds.c"
	.globl	foo                     # -- Begin function foo
	.p2align	4, 0x90
	.type	foo,@function
foo:                                    # @foo
# %bb.0:                                # %entry
	#APP
	
.ascii "->NR_PAGEFLAGS $22 __NR_PAGEFLAGS"
	#NO_APP
	#APP
	
.ascii "->MAX_NR_ZONES $4 __MAX_NR_ZONES"
	#NO_APP
	#APP
	
.ascii "->NR_CPUS_BITS $6 ilog2(CONFIG_NR_CPUS)"
	#NO_APP
	#APP
	
.ascii "->SPINLOCK_SIZE $4 sizeof(spinlock_t)"
	#NO_APP
	retq
.Lfunc_end0:
	.size	foo, .Lfunc_end0-foo
                                        # -- End function

	.ident	"clang version 7.0.0 (trunk 338452)"
	.section	".note.GNU-stack","",@progbits
