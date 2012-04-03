#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int Global;

void __attribute__((noinline)) foo1() {
  Global = 42;
}

void __attribute__((noinline)) bar1() {
  volatile int tmp = 42;
  foo1();
}

void __attribute__((noinline)) foo2() {
  volatile int v = Global; (void)v;
}

void __attribute__((noinline)) bar2() {
  volatile int tmp = 42;
  foo2();
}

void *Thread1(void *x) {
  usleep(1000000);
  bar1();
  return NULL;
}

void *Thread2(void *x) {
  bar2();
  return NULL;
}

void StartThread(pthread_t *t, void *(*f)()) {
  pthread_create(t, NULL, f, NULL);
}

int main() {
  pthread_t t[2];
  StartThread(&t[0], Thread1);
  StartThread(&t[1], Thread2);
  pthread_join(t[0], NULL);
  pthread_join(t[1], NULL);
}

// CHECK:      WARNING: ThreadSanitizer: data race
// CHECK-NEXT:   Write of size 4 at {{.*}} by thread 1:
// CHECK-NEXT:     #0 {{.*}}: foo1 simple_stack.c:8
// CHECK-NEXT:     #1 {{.*}}: bar1 simple_stack.c:13
// CHECK-NEXT:     #2 {{.*}}: Thread1 simple_stack.c:27
// CHECK-NEXT:   Previous Read of size 4 at {{.*}} by thread 2:
// CHECK-NEXT:     #0 {{.*}}: foo2 simple_stack.c:17
// CHECK-NEXT:     #1 {{.*}}: bar2 simple_stack.c:22
// CHECK-NEXT:     #2 {{.*}}: Thread2 simple_stack.c:32
// CHECK-NEXT:   Thread 1 (running) created at:
// CHECK-NEXT:     #0 {{.*}}: pthread_create {{.*}}
// CHECK-NEXT:     #1 {{.*}}: StartThread simple_stack.c:37
// CHECK-NEXT:     #2 {{.*}}: main simple_stack.c:42
// CHECK-NEXT:   Thread 2 ({{.*}}) created at:
// CHECK-NEXT:     #0 {{.*}}: pthread_create {{.*}}
// CHECK-NEXT:     #1 {{.*}}: StartThread simple_stack.c:37
// CHECK-NEXT:     #2 {{.*}}: main simple_stack.c:43

