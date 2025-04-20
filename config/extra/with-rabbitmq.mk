ifneq (,$(wildcard $(OPT)/lib/librabbitmq.a))
FD_HAS_RABBITMQ:=1
CFLAGS+=-DFD_HAS_RABBITMQ=1
LDFLAGS+=$(OPT)/lib/librabbitmq.a
else
$(info "rabbitmq not installed, skipping")
endif

