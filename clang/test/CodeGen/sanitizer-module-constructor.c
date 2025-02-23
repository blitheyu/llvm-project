// RUN: %clang_cc1 -fsanitize=address -emit-llvm -O3 -fdebug-pass-manager -fexperimental-new-pass-manager -o - %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -fsanitize=thread -emit-llvm -O3 -fdebug-pass-manager -fexperimental-new-pass-manager -o - %s 2>&1 | FileCheck %s
// RUN: %clang_cc1 -fsanitize=memory -emit-llvm -O3 -fdebug-pass-manager -fexperimental-new-pass-manager -o - %s 2>&1 | FileCheck %s

// This is regression test for PR42877

typedef struct a *b;
struct a {
  int c;
};
int d;
b e;
static void f(b g) {
  for (d = g->c;;)
    ;
}
void h() { f(e); }

// CHECK: Running pass: {{.*}}SanitizerPass on {{.*}}sanitizer-module-constructor.c
// CHECK-NOT: Running pass: LoopSimplifyPass on {{.*}}san.module_ctor
// CHECK: Running analysis: DominatorTreeAnalysis on {{.*}}san.module_ctor
// CHECK: Running pass: LoopSimplifyPass on {{.*}}san.module_ctor
