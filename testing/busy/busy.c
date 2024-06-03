#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

void f1();
void f2();
void f3();
void f4();
void f5();
void f6();
void f7();
void f8();
void f9();
void f10();
void f11();
void f12();
void f13();
void f14();
void f15();
void f16();
void f17();
void f18();
void f19();
void f20();

void f1() { f2(); }
void f2() { f3(); }
void f3() { f4(); }
void f4() { f5(); }
void f5() { f6(); }
void f6() { f7(); }
void f7() { f8(); }
void f8() { f9(); }
void f9() { f10(); }
void f10() { f11(); }
void f11() { f12(); }
void f12() { f13(); }
void f13() { f14(); }
void f14() { f15(); }
void f15() { f16(); }
void f16() { f17(); }
void f17() { f18(); }
void f18() { f19(); }
void f19() { f20(); }

void f20() {
    while (true) {
        // Busy-wait
    }
}

int main() {
    f1();
    return 0;
}

