/*
 * RUN: clang %cflags -emit-llvm -S %s -o %t.ll
 * RUN: soaap -o %t.soaap.ll %t.ll > %t.out
 * RUN: FileCheck %s -input-file %t.out
 *
 * CHECK: Running Soaap Pass
 */
#include "soaap.h"
#include <fcntl.h>
#include <unistd.h>

void foo();

int main(int argc, char** argv) {
  foo();
  return 0;
}

__soaap_sandbox_persistent("sandbox")
void foo() {
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "open" but it is not allowed to,
  // CHECK-DAG: *** based on the current sandboxing restrictions.
  // CHECK-DAG: +++ Line 24 of file {{.*}}
  int fd = open("somefile", O_CREAT);
  __soaap_limit_syscalls(read, write);
  __soaap_limit_fd_syscalls(fd, read);
  write(fd, NULL, 0);
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "close" but it is not allowed to,
  // CHECK-DAG: *** based on the current sandboxing restrictions.
  // CHECK-DAG: +++ Line 24 of file {{.*}}
  close(fd);
  
  int fds[2];
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "open" but it is not allowed to,
  // CHECK-DAG: *** based on the current sandboxing restrictions.
  // CHECK-DAG: +++ Line 24 of file {{.*}}
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "close" but it is not allowed to,
  // CHECK-DAG: *** based on the current sandboxing restrictions.
  // CHECK-DAG: +++ Line 24 of file {{.*}}
  fds[0] = open("somefile", O_CREAT);
  __soaap_limit_fd_syscalls(fds[0], read, write);
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "write" but is not allowed to for the given fd arg.
  // CHECK-DAG: +++ Line 27 of file {{.*}}
  
  // CHECK-NOT: *** Sandbox "sandbox" performs system call "read" but is not allowed to for the given fd arg.
  // CHECK-NOT: +++ Line 47 of file {{.*}}
  read(fds[0], NULL, 0);
  write(fds[0], NULL, 0);
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "close" but is not allowed to for the given fd arg.
  // CHECK-DAG: +++ Line 31 of file {{.*}}
  // CHECK-DAG: *** Sandbox "sandbox" performs system call "close" but is not allowed to for the given fd arg.
  // CHECK-DAG: +++ Line 53 of file {{.*}}
  close(fds[0]);
}