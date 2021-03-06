#include <stdio.h>

int globalVar = 0;
int var2 = 0;

void bar1(int i, int j) {
  if (i > 4)
    globalVar++;
}

void bar2(int i, int j) {
  if (j > 5)
    var2++;
}

void foo(int argc) {
  void (*fp)(int, int);
  fp = &bar2;
  fp = &bar1;
  fp(argc, argc+2);
}

int main (int argc, char *argv[]) {
  foo(argc);
  return globalVar;
}

/*  Testing purpose: TBD.
*/

// Testing commands:
// RUN: %srcroot/common-scripts/build-bc.sh %s
// RUN: %srcroot/common-scripts/klee-opt.sh %s.bc
// RUN: %kleebindir/klee --use-one-checker=Assert --use-path-slicer=1 %s.bc 2> %s.output
// RUN: cat %s.output | FileCheck %s

// Expected results:
// CHECK: IntraSlicer::calStat TAKEN: IDX: 0: TID: 0: INSTRID: 38: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %argc_addr = alloca i32                         ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 2: TID: 0: INSTRID: 40: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %retval = alloca i32                            ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 3: TID: 0: INSTRID: 41: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   %0 = alloca i32                                 ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 5: TID: 0: INSTRID: 43: TAKEN: INTRA_STORE_OW: INSTR: F: main: BB: entry:   store i32 %argc, i32* %argc_addr

// CHECK: IntraSlicer::calStat TAKEN: IDX: 7: TID: 0: INSTRID: 45: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: entry:   %1 = load i32* %argc_addr, align 4              ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 8: TID: 0: INSTRID: 46: TAKEN: INTRA_NON_MEM: INSTR: F: main: BB: entry:   call void @_Z3fooi(i32 %1)

// CHECK: IntraSlicer::calStat TAKEN: IDX: 9: TID: 0: INSTRID: 26: TAKEN: INTRA_NON_MEM: INSTR: F: _Z3fooi: BB: entry:   %argc_addr = alloca i32                         ; <i32*> [#uses=3]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 10: TID: 0: INSTRID: 27: TAKEN: INTRA_NON_MEM: INSTR: F: _Z3fooi: BB: entry:   %fp = alloca void (i32, i32)*                   ; <void (i32, i32)**> [#uses=3]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 12: TID: 0: INSTRID: 29: TAKEN: INTRA_STORE_OW: INSTR: F: _Z3fooi: BB: entry:   store i32 %argc, i32* %argc_addr

// CHECK: IntraSlicer::calStat TAKEN: IDX: 14: TID: 0: INSTRID: 31: TAKEN: INTRA_STORE_OW: INSTR: F: _Z3fooi: BB: entry:   store void (i32, i32)* @_Z4bar1ii, void (i32, i32)** %fp, align 8

// CHECK: IntraSlicer::calStat TAKEN: IDX: 15: TID: 0: INSTRID: 32: TAKEN: INTRA_LOAD_OW: INSTR: F: _Z3fooi: BB: entry:   %0 = load i32* %argc_addr, align 4              ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 16: TID: 0: INSTRID: 33: TAKEN: INTRA_NON_MEM: INSTR: F: _Z3fooi: BB: entry:   %1 = add nsw i32 %0, 2                          ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 17: TID: 0: INSTRID: 34: TAKEN: INTRA_LOAD_OW: INSTR: F: _Z3fooi: BB: entry:   %2 = load void (i32, i32)** %fp, align 8        ; <void (i32, i32)*> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 18: TID: 0: INSTRID: 35: TAKEN: INTRA_LOAD_OW: INSTR: F: _Z3fooi: BB: entry:   %3 = load i32* %argc_addr, align 4              ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 19: TID: 0: INSTRID: 36: TAKEN: INTRA_NON_MEM: INSTR: F: _Z3fooi: BB: entry:   call void %2(i32 %3, i32 %1)

// CHECK: IntraSlicer::calStat TAKEN: IDX: 20: TID: 0: INSTRID: 0: TAKEN: INTRA_NON_MEM: INSTR: F: _Z4bar1ii: BB: entry:   %i_addr = alloca i32                            ; <i32*> [#uses=2]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 23: TID: 0: INSTRID: 3: TAKEN: INTRA_STORE_OW: INSTR: F: _Z4bar1ii: BB: entry:   store i32 %i, i32* %i_addr

// CHECK: IntraSlicer::calStat TAKEN: IDX: 25: TID: 0: INSTRID: 5: TAKEN: INTRA_LOAD_OW: INSTR: F: _Z4bar1ii: BB: entry:   %0 = load i32* %i_addr, align 4                 ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 26: TID: 0: INSTRID: 6: TAKEN: INTRA_NON_MEM: INSTR: F: _Z4bar1ii: BB: entry:   %1 = icmp sgt i32 %0, 4                         ; <i1> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 27: TID: 0: INSTRID: 7: TAKEN: INTRA_BR_N_POSTDOM: INSTR: F: _Z4bar1ii: BB: entry:   br i1 %1, label %bb, label %return

// CHECK: IntraSlicer::calStat TAKEN: IDX: 28: TID: 0: INSTRID: 12: TAKEN: INTRA_RET_WRITE_FUNC: INSTR: F: _Z4bar1ii: BB: return:   ret void

// CHECK: IntraSlicer::calStat TAKEN: IDX: 29: TID: 0: INSTRID: 37: TAKEN: INTRA_RET_WRITE_FUNC: INSTR: F: _Z3fooi: BB: entry:   ret void

// CHECK: IntraSlicer::calStat TAKEN: IDX: 30: TID: 0: INSTRID: 47: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: entry:   %2 = load i32* @globalVar, align 4              ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 31: TID: 0: INSTRID: 48: TAKEN: INTRA_STORE_OW: INSTR: F: main: BB: entry:   store i32 %2, i32* %0, align 4

// CHECK: IntraSlicer::calStat TAKEN: IDX: 32: TID: 0: INSTRID: 49: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: entry:   %3 = load i32* %0, align 4                      ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 33: TID: 0: INSTRID: 50: TAKEN: INTRA_STORE_OW: INSTR: F: main: BB: entry:   store i32 %3, i32* %retval, align 4

// CHECK: IntraSlicer::calStat TAKEN: IDX: 34: TID: 0: INSTRID: 51: TAKEN: INTRA_LOAD_OW: INSTR: F: main: BB: entry:   %retval1 = load i32* %retval                    ; <i32> [#uses=1]

// CHECK: IntraSlicer::calStat TAKEN: IDX: 35: TID: 0: INSTRID: 52: TAKEN: TEST_TARGET: INSTR: F: main: BB: entry:   ret i32 %retval1



// CHECK: IntraSlicer::calStat STATISTICS: numExedInstrs: 36;  numTakenInstrs: 28;  numExedBrs: 1;  numTakenBrs: 1;  numExedSymBrs: 0;  numTakenSymBrs: 0; StaticExed/Static Instrs: 36/53;  numTakenExtCalls/numExedExtCalls: 0/0;


