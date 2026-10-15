#include <ucontext.h>
static ucontext_t a, b;
static void f(void){swapcontext(&a,&b);}
int main(void){static char st[65536]; getcontext(&a); a.uc_stack.ss_sp=st; a.uc_stack.ss_size=sizeof st; a.uc_link=&b; makecontext(&a,f,0); return 0;}
