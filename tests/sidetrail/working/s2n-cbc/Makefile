include ../../lib/ct-verif.mk

cflags +=-D__TIMING_CONTRACTS__
#the overriden .h
cflags += -I$(CURDIR)
#original .h
cflags += -I$(CURDIR)/../../../../
#/api .h that haven't fixed that
cflags += -I$(CURDIR)/../../../../api/
cflags += -fPIC -std=c99 -fgnu89-inline
extras += crypto/s2n_hash.c
extras += crypto/s2n_hmac.c
extras += error/s2n_errno.c
extras += tls/s2n_cbc.c
extras += utils/s2n_ensure.c
extras += utils/s2n_result.c
extras += utils/s2n_safety.c
goals  += simple_cbc_wrapper@cbc.c
full_self_comp += true
smtlog += p.smt2
shadowingArgs += "self_comp_mode=full,branchcond_assertion=assert_never,memory_assertion=assert_never"
timing += true
#printModel += true
