// JIT の正しさを確かめるための負荷。bench.c が速度を測るのに対し、こちらは命令の種類を広く踏む
// （倍精度・比較・変換・64 ビット乗算・レジスタ指定シフト・キャリー付き演算・関数ポインタ・
// switch の分岐表・再帰・条件付き実行）。JIT の有無で benchChecksum が一致すれば、変換した命令と
// インタプリタに戻す命令（フォールバック）の両方が同じ結果を出している。
// ビルド: ./build.sh（arm-none-eabi-gcc が必要）
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned char u8;
typedef unsigned short u16;

volatile u32 sink;

// bench.c と同じ配置の記録（データ領域の先頭 = 0x00102000）
#define CHECKPOINT 300
struct Report { u32 magic; volatile u32 iterations; u32 checkpoint; };
struct Report report __attribute__((section(".report"))) = {0x42454E43, 0, 0};

static u32 rng_state = 2463534242u;
static inline u32 rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

#define NOINLINE __attribute__((noinline))

static float fa[64];
static double da[32];
static u32 ia[64];
static u16 ha[64];
static u8 ba[64];

static inline u32 fbits(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }
static inline u32 dbits(double d) { union { double d; u64 u; } x; x.d = d; return (u32)x.u ^ (u32)(x.u >> 32); }

// 単精度: 比較（vcmp + vmrs）、絶対値、変換、積和
NOINLINE static u32 floats(float* a, int n) {
    float lo = a[0], hi = a[0], acc = 0;
    int above = 0;
    for (int i = 0; i < n; i++) {
        float v = a[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        if (v > 0.5f) above++;
        acc += (v < 0 ? -v : v) * 1.5f - acc * 0.125f;
        a[i] = v * 0.75f + (float)(i & 7) * 0.0625f - (float)(int)(acc * 3.0f) * 0.001f;
    }
    return fbits(lo) ^ fbits(hi) * 3 ^ fbits(acc) * 5 ^ (u32)above;
}

// 倍精度と単倍の変換
NOINLINE static u32 doubles(double* a, const float* f, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) {
        double v = a[i] * 1.0009765625 + (double)f[i] * 0.5;
        if (v > 4.0) v = v * 0.25 - 1.0;
        if (v < -4.0) v = -v * 0.25;
        a[i] = v;
        s += v * (double)(i + 1);
    }
    return dbits(s) ^ (u32)(int)(s * 16.0);
}

// 64 ビット演算: umull/smull/adc/sbc、可変シフト
NOINLINE static u32 wide(u32 a, u32 b, u32 sh) {
    u64 p = (u64)a * b;
    s64 q = (s64)(int)a * (int)b;
    u64 r = p + (u64)q + ((u64)b << 32 | a);
    r -= (u64)sh * 0x100000001ull;
    u32 s = sh & 63;
    r = (r << (s & 31)) | (r >> (64 - (s & 31) - 1) >> 1);
    u32 x = (a << (sh & 37)) ^ (b >> (sh & 35)) ^ (u32)((int)a >> (sh & 33));
    x = (x >> (sh & 31)) | (x << ((32 - (sh & 31)) & 31));
    return (u32)r ^ (u32)(r >> 32) ^ x ^ (u32)__builtin_clz(a | 1);
}

// 条件付き実行と半端な幅の読み書き、符号拡張
NOINLINE static u32 mixed(u32* w, u16* h, u8* b, int n) {
    u32 s = 0;
    for (int i = 0; i < n; i++) {
        int v = (short)h[i] + (signed char)b[i];
        if (v < 0) v = -v;
        if (w[i] & 1) s += (u32)v; else s ^= (u32)v << 3;
        if ((w[i] & 6) == 6) s = ~s;
        if ((s ^ w[i]) & 0x80000000u) s += 0x10001;
        h[i] = (u16)(s >> 5);
        b[i] = (u8)(s >> 11);
        w[i] = w[i] * 1664525u + s + (w[i] >> 7);
        if (i + 1 < n) { w[i + 1] ^= s >> 3; }
    }
    return s;
}

// switch の分岐表
NOINLINE static u32 dispatch(u32 op, u32 a, u32 b) {
    switch (op & 7) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a ^ (b << 1);
    case 3: return a & ~b;
    case 4: return a | (b >> 3);
    case 5: return a * 31 + b;
    case 6: return (a >> 1) ^ b;
    default: return b - a;
    }
}

// 関数ポインタ（blx）
typedef u32 (*Fn)(u32, u32);
NOINLINE static u32 f_add(u32 a, u32 b) { return a + b * 3; }
NOINLINE static u32 f_xor(u32 a, u32 b) { return (a ^ b) + (a >> 3); }
NOINLINE static u32 f_mul(u32 a, u32 b) { return a * (b | 1); }
static Fn fns[4] = {f_add, f_xor, f_mul, f_add};

// 再帰（push/pop、bl、bx lr）
NOINLINE static u32 recurse(u32 n, u32 acc) {
    if (n == 0) return acc;
    u32 r = recurse(n - 1, acc * 33 + n);
    return r ^ (n << (n & 15));
}

// 構造体の 64 ビット要素（ldrd/strd が出やすい）
struct Pair { u64 a; u64 b; };
static struct Pair pairs[16];
NOINLINE static u32 pairwork(struct Pair* p, int n) {
    u64 s = 0;
    for (int i = 0; i < n; i++) {
        u64 t = p[i].a;
        p[i].a = p[i].b + s;
        p[i].b = t ^ (s << 1);
        s += p[i].a ^ (p[i].b >> 3);
    }
    return (u32)s ^ (u32)(s >> 32);
}

// フラグの直接書き込み（msr cpsr_f）と読み出し、スレッドローカル領域のポインタ（mrc p15）
NOINLINE static u32 flagsio(u32 v) {
    u32 r, t;
    __asm__ volatile(
        "msr cpsr_f, %2\n"
        "movcs %0, #1\n"
        "movcc %0, #2\n"
        "addeq %0, %0, #4\n"
        "addmi %0, %0, #8\n"
        "addvs %0, %0, #16\n"
        "mrs %1, cpsr\n"
        "eor %0, %0, %1, lsr #28\n"
        "mrc p15, 0, %1, c13, c0, 3\n"
        "add %0, %0, %1\n"
        : "=&r"(r), "=&r"(t) : "r"(v) : "cc");
    return r;
}

void _start(void) {
    for (int i = 0; i < 64; i++) {
        fa[i] = (float)(int)(rng() & 0xFFFF) * (1.0f / 32768.0f) - 1.0f;
        ia[i] = rng();
        ha[i] = (u16)rng();
        ba[i] = (u8)rng();
    }
    for (int i = 0; i < 32; i++) da[i] = (double)(int)(rng() & 0xFFFFF) * (1.0 / 262144.0) - 2.0;
    for (int i = 0; i < 16; i++) { pairs[i].a = ((u64)rng() << 32) | rng(); pairs[i].b = rng(); }
    for (;;) {
        u32 s = sink;
        s += floats(fa, 64);
        s ^= doubles(da, fa, 32);
        for (int i = 0; i < 32; i++) s += wide(ia[i], ia[63 - i] ^ s, s + (u32)i);
        s ^= mixed(ia, ha, ba, 64);
        for (u32 i = 0; i < 64; i++) s = dispatch(ia[i] >> 9, s, ia[(i * 5) & 63]);
        for (u32 i = 0; i < 64; i++) s = fns[(s >> 4) & 3](s, ia[i]);
        s += recurse(12 + (s & 7), s);
        s ^= pairwork(pairs, 16);
        for (u32 i = 0; i < 32; i++) s = s * 5 + flagsio(ia[i] ^ (s << 7));
        sink = s;
        if (++report.iterations == CHECKPOINT) report.checkpoint = sink;
    }
}
