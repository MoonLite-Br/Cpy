CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -std=gnu11
LDLIBS  += -lm -pthread
LIBS    := $(wildcard lib/*.cpi)
SRC     := $(sort $(wildcard src/*.c))
BIN     := bin/cpy

# https:// support (src/ssl_mod.c) needs OpenSSL's dev headers + libs.
# Auto-detected: `pkg install openssl` on Termux, `apt install libssl-dev` /
# `dnf install openssl-devel` on Linux, then a clean rebuild picks it up.
# With neither, cpy still builds fine -- urllib just can't do https://.
HAVE_OPENSSL := $(shell mkdir -p build 2>/dev/null; printf '#include <openssl/ssl.h>\nint main(void){SSL_CTX_new(TLS_client_method());return 0;}\n' > build/.ssl_check.c 2>/dev/null; $(CC) build/.ssl_check.c -lssl -lcrypto -o build/.ssl_check 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_OPENSSL),1)
CFLAGS += -DCPY_HAVE_OPENSSL
LDLIBS += -lssl -lcrypto
endif

# Generators (src/gen.c) use ucontext.h userspace coroutines when available
# -- a real stack switch in userspace instead of an OS thread + semaphore
# pair per generator, which is what makes generator-heavy code fast. Present
# on glibc and on modern Android/Termux (Bionic); auto-detected the same
# way as OpenSSL above. Falls back to the original thread-based engine
# (functionally identical, just slower) wherever it's missing.
HAVE_UCONTEXT := $(shell mkdir -p build 2>/dev/null; printf '#include <ucontext.h>\nstatic ucontext_t a, b;\nstatic void f(void){swapcontext(&a,&b);}\nint main(void){static char st[65536]; getcontext(&a); a.uc_stack.ss_sp=st; a.uc_stack.ss_size=sizeof st; a.uc_link=&b; makecontext(&a,f,0); return 0;}\n' > build/.uctx_check.c 2>/dev/null; $(CC) build/.uctx_check.c -o build/.uctx_check 2>/dev/null && echo 1 || echo 0)
# `make FORCE_SIGALT=1` pretends ucontext is missing (to test the Android/Termux path on Linux).
ifdef FORCE_SIGALT
HAVE_UCONTEXT := 0
endif

# Fallback when ucontext is missing (Android/Termux Bionic): sigaltstack + _setjmp/_longjmp,
# pure POSIX and architecture independent -- still a fast userspace switch.
HAVE_SIGALT := $(shell mkdir -p build 2>/dev/null; printf '#include <signal.h>\n#include <setjmp.h>\nstatic jmp_buf j;\nstatic void h(int s){(void)s;if(!_setjmp(j))return;}\nint main(void){static char st[65536];stack_t ss;struct sigaction sa;ss.ss_sp=st;ss.ss_size=sizeof st;ss.ss_flags=0;sigaltstack(&ss,0);sa.sa_handler=h;sa.sa_flags=SA_ONSTACK;sigemptyset(&sa.sa_mask);sigaction(SIGUSR2,&sa,0);raise(SIGUSR2);pthread_sigmask(0,0,0);return 0;}\n' > build/.sigalt_check.c 2>/dev/null; $(CC) -pthread build/.sigalt_check.c -o build/.sigalt_check 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAVE_UCONTEXT),1)
CFLAGS += -DCPY_HAVE_UCONTEXT
else ifeq ($(HAVE_SIGALT),1)
CFLAGS += -DCPY_HAVE_SIGALT
endif

all: $(BIN)
	@if [ "$(HAVE_OPENSSL)" = "1" ]; then echo "built with https:// support (OpenSSL found)"; \
	else echo "built WITHOUT https:// support (OpenSSL not found -- install openssl-dev/libssl-dev and rebuild for https://)"; fi
	@if [ "$(HAVE_UCONTEXT)" = "1" ]; then echo "generators: fast ucontext engine"; \
	elif [ "$(HAVE_SIGALT)" = "1" ]; then echo "generators: fast sigaltstack engine (no ucontext on this platform)"; \
	else echo "generators: slow thread-based fallback (neither ucontext nor sigaltstack usable)"; fi

# stdlib_data.c is pre-generated and shipped; only regenerate when tools/ are present.
ifneq ($(wildcard tools/gen_stdlib.sh),)
src/stdlib_data.c: $(LIBS) tools/gen_stdlib.sh
	sh tools/gen_stdlib.sh $(LIBS) > $@
endif

$(BIN): $(SRC) src/cpy.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

debug:
	@mkdir -p bin
	$(CC) -g -O0 -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -std=gnu11 $(if $(filter 1,$(HAVE_OPENSSL)),-DCPY_HAVE_OPENSSL) $(if $(filter 1,$(HAVE_UCONTEXT)),-DCPY_HAVE_UCONTEXT,$(if $(filter 1,$(HAVE_SIGALT)),-DCPY_HAVE_SIGALT)) -o bin/cpy-asan $(SRC) $(LDLIBS)

RTSRC := $(filter-out src/main.c,$(SRC))
RTOBJ := $(RTSRC:src/%.c=build/%.o)

build/%.o: src/%.c src/cpy.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

bin/libcpyrt.a: $(RTOBJ)
	@mkdir -p bin
	ar rcs $@ $(RTOBJ)

aot: bin/libcpyrt.a
	@echo "runtime library ready: bin/libcpyrt.a  (use: cpy selfhost/aotc.cpi in.cpi -o out)"

test: $(BIN)
	./run_tests.sh

clean:
	rm -rf bin build

.PHONY: all debug test clean aot
