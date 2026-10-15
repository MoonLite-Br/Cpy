#include <signal.h>
#include <setjmp.h>
static jmp_buf j;
static void h(int s){(void)s;if(!_setjmp(j))return;}
int main(void){static char st[65536];stack_t ss;struct sigaction sa;ss.ss_sp=st;ss.ss_size=sizeof st;ss.ss_flags=0;sigaltstack(&ss,0);sa.sa_handler=h;sa.sa_flags=SA_ONSTACK;sigemptyset(&sa.sa_mask);sigaction(SIGUSR2,&sa,0);raise(SIGUSR2);pthread_sigmask(0,0,0);return 0;}
