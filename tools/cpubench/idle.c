// 何もしないプログラム（スリープし続ける）。1 フレームの固定費（CPU コア以外）を測るための比較用。
__attribute__((naked)) void _start(void) {
    __asm__("1: mov r0, #0x10000000\n mov r1, #0\n svc 0x0A\n b 1b\n");
}
