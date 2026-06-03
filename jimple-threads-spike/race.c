#include <pthread.h>
int balance = 100;
void *w(void *_) { if (balance >= 100) balance -= 100; return 0; }
int main(void){ pthread_t a,b; pthread_create(&a,0,w,0); pthread_create(&b,0,w,0);
  pthread_join(a,0); pthread_join(b,0); __ESBMC_assert(balance>=0,"overdraft"); return 0; }
