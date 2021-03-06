#include <stdio.h>

int globalVar = 0;

void foo() {
  globalVar++;
}

/* Slicing target: the last return instruction, including the returned value.
    Testcase purpose: the not executed branch will modify shared variable; make 
    sure our algorithm will take this branch.
*/
int main (int argc, char *argv[]) {
  if (argc > 4) {
    foo();
  }
  return globalVar;
}

// Testing commands:
// RUN: %srcroot/common-scripts/build-bc.sh %s
// RUN: %srcroot/common-scripts/klee-opt.sh %s.bc
// RUN: %kleebindir/klee --use-one-checker=Assert --use-path-slicer=1 %s.bc 2> %s.output
// RUN: cat %s.output | FileCheck %s


// Expected results:
// CHECK: IntraSlicer::calStat TAKEN: IDX: 0: TID: 0: INSTRID: 4: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %argc_addr = alloca i32                         ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 2: TID: 0: INSTRID: 6: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %retval = alloca i32                            ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 3: TID: 0: INSTRID: 7: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %0 = alloca i32                                 ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 5: TID: 0: INSTRID: 9: TAKEN: INTRA_STORE_OW: INSTR: F: main: BB: entry:   store i32 %argc, i32* %argc_addr

// CHECK: IntraSlicer::calStat TAKEN: IDX: 7: TID: 0: INSTRID: 11: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: entry:   %1 = load i32* %argc_addr, align 4              ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 8: TID: 0: INSTRID: 12: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %2 = icmp sgt i32 %1, 4                         ; <i1> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 9: TID: 0: INSTRID: 13: TAKEN: INTRA_BR_WR_BETWEEN: INSTR: F: main: BB: entry:   br i1 %2, label %bb, label %bb1

// CHECK: IntraSlicer::calStat TAKEN: IDX: 10: TID: 0: INSTRID: 16: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: bb1:   %3 = load i32* @globalVar, align 4              ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 11: TID: 0: INSTRID: 17: TAKEN: INTRA_STORE_OW: INSTR: F: main: BB: bb1:   store i32 %3, i32* %0, align 4

// CHECK: IntraSlicer::calStat TAKEN: IDX: 12: TID: 0: INSTRID: 18: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: bb1:   %4 = load i32* %0, align 4                      ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 13: TID: 0: INSTRID: 19: TAKEN: INTRA_STORE_OW: INSTR: F: main: BB: bb1:   store i32 %4, i32* %retval, align 4

// CHECK: IntraSlicer::calStat TAKEN: IDX: 14: TID: 0: INSTRID: 20: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: bb1:   %retval2 = load i32* %retval                    ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 15: TID: 0: INSTRID: 21: TAKEN: TEST_TARGET: INSTR: F: main: BB: bb1:   ret i32 %retval2



// CHECK: IntraSlicer::calStat STATISTICS: numExedInstrs: 16;  numTakenInstrs: 13;  numExedBrs: 1;  numTakenBrs: 1;  numExedSymBrs: 0;  numTakenSymBrs: 0
