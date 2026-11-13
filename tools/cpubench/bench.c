// インタプリタの速度を測るための CPU 負荷（SVC を呼ばず、整数・メモリ・分岐・関数呼び出し・VFP を
// 混ぜて回し続ける）。Azahar は ELF を直接読めるので、ヘッドレスのブラウザで 1 フレームあたりの
// 処理時間（avgFrameMs）を比べれば、インタプリタの変更の効果が分かる。
// ビルド: ./build.sh（arm-none-eabi-gcc が必要）
typedef unsigned int u32;
typedef unsigned char u8;

static u32 table[256];
static u8 buf[4096];
static int arr[512];
static float m[3][16];
volatile u32 sink;

// テスト側が読む記録（データ領域の先頭 = 0x00102000 に置く。link.ld を参照）:
//   magic、外側のループを回った回数、CHECKPOINT 周目の sink（JIT の有無で一致するはず）
#define CHECKPOINT 20
struct Report { u32 magic; volatile u32 iterations; u32 checkpoint; };
struct Report report __attribute__((section(".report"))) = {0x42454E43, 0, 0};

static u32 rng_state = 12345;
static inline u32 rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

__attribute__((noinline)) static u32 crc(const u8* p, u32 n) {
    u32 c = ~0u;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

__attribute__((noinline)) static void sort(int* a, int n) {
    for (int i = 1; i < n; i++) {
        int v = a[i], j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

__attribute__((noinline)) static void matmul(float* r, const float* a, const float* b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

struct Node { struct Node* next; u32 v; };
static struct Node nodes[256];

__attribute__((noinline)) static u32 walk(struct Node* n) {
    u32 s = 0;
    while (n) { s += n->v; if (s & 1) s ^= 0x9E3779B9; n = n->next; }
    return s;
}

void _start(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        table[i] = c;
        nodes[i].next = i < 255 ? &nodes[(i * 37 + 11) & 255] : 0;
        nodes[i].v = i * 2654435761u;
    }
    for (int i = 0; i < 16; i++) { m[0][i] = i * 0.25f; m[1][i] = 1.0f / (i + 1); }
    for (;;) {
        for (u32 i = 0; i < sizeof(buf); i++) buf[i] = (u8)rng();
        sink += crc(buf, sizeof(buf));
        for (int i = 0; i < 512; i++) arr[i] = (int)(rng() & 0xFFFF);
        sort(arr, 512);
        sink += (u32)arr[100];
        for (int i = 0; i < 200; i++) { matmul(m[2], m[0], m[1]); m[0][i & 15] = m[2][(i * 7) & 15] * 0.001f + 0.5f; }
        sink += (u32)m[2][5];
        struct Node* n = &nodes[0];
        for (int i = 0; i < 20; i++) sink += walk(n);
        if (++report.iterations == CHECKPOINT) report.checkpoint = sink;
    }
}
