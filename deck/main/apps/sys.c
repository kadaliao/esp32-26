// 系统：小屏上的 htop。按 Apple Silicon 优化——能效核 / 性能核分组、GPU、内存压力和 swap、
// 进程按 App 合并、coding agent 标出颜色。数据来自 host/apps/sysmon.py，每秒一帧。
#include "app.h"
#include "gfx.h"

#define MAX_CORES 24
#define MAX_TOP   6
#define HIST      96     // 走势图保留多少个采样（秒）
#define STALE_MS  5000

typedef struct {
    char name[48];
    char kind[14];   // coding agent 种类，不是 agent 为空
    int cpu;         // %，可以超过 100（多核）
    int mem;         // MB
} proc_t;

typedef struct {
    bool valid, bye;
    int64_t recv_ms;
    char clock[8];
    char chip[16];
    int ne, ncore;
    uint8_t core[MAX_CORES];
    int cpu, gpu, mp;
    float mu, mt, sw, swt;
    float up_bps, down_bps, rd_bps, wr_bps;
    int df, dp;
    bool has_bat, bat_ac;
    int bat;
    float bat_w;     // 电池充放电功率（充为正）
    float pw;        // 整机功耗，<0 = 没有
    int32_t uptime;
    float load;
    int ntop;
    proc_t top[MAX_TOP];
} snapshot_t;

static snapshot_t s_snap;
static float s_core[MAX_CORES];      // 平滑后的柱高 0..1
static float s_cpu, s_gpu, s_mem;    // 平滑后的总量 0..1
static uint8_t s_hcpu[HIST], s_hdown[HIST], s_hup[HIST];
static int s_hn;                     // 已有几个采样
static int s_hi;                     // 环形缓冲写到哪了
static float s_t;

typedef struct {
    float x, y, vy, life;
    rgb_t col;
} spark_t;
#define MAX_SPARKS 40
static spark_t s_spark[MAX_SPARKS];

// ---- 配色：LATENT 那套深空 HUD ----
static const rgb_t HUD = {140, 165, 200};    // 标签
static const rgb_t DIM = {80, 96, 130};      // 次要文字、刻度
static const rgb_t TXT = {225, 232, 245};

// 负载 → 颜色：蓝 → 青 → 琥珀 → 品红
static rgb_t heat(float v)
{
    static const rgb_t S[] = {{40, 120, 255}, {0, 234, 255}, {255, 200, 90}, {255, 90, 170}};
    static const float P[] = {0.0f, 0.4f, 0.72f, 1.0f};
    v = clampf(v, 0, 1);
    for (int i = 1; i < 4; i++) {
        if (v <= P[i]) return mix(S[i - 1], S[i], (v - P[i - 1]) / (P[i] - P[i - 1]));
    }
    return S[3];
}

static rgb_t agent_color(const char *kind)
{
    if (strcmp(kind, "claude") == 0) return RGB(255, 136, 84);
    if (strcmp(kind, "codex") == 0) return RGB(86, 214, 255);
    if (strcmp(kind, "gemini") == 0) return RGB(120, 150, 255);
    return RGB(196, 140, 255);
}

static void fmt_rate(float bps, char *buf, size_t cap)
{
    if (bps < 1000) snprintf(buf, cap, "%dB", (int)bps);
    else if (bps < 1e6f) snprintf(buf, cap, "%dK", (int)(bps / 1e3f));
    else if (bps < 1e7f) snprintf(buf, cap, "%.1fM", bps / 1e6f);
    else snprintf(buf, cap, "%dM", (int)(bps / 1e6f));
}

static void fmt_mem(int mb, char *buf, size_t cap)
{
    if (mb < 1000) snprintf(buf, cap, "%dM", mb);
    else if (mb < 10240) snprintf(buf, cap, "%.1fG", mb / 1024.0f);
    else snprintf(buf, cap, "%dG", mb / 1024);
}

static void fmt_uptime(int32_t s, char *buf, size_t cap)
{
    if (s >= 86400) snprintf(buf, cap, "%ldD %ldH", (long)(s / 86400), (long)(s % 86400 / 3600));
    else snprintf(buf, cap, "%ldH %ldM", (long)(s / 3600), (long)(s % 3600 / 60));
}

// 对数刻度：网速跨好几个数量级，线性画会一直贴底
static uint8_t log_level(float bps)
{
    if (bps < 1000) return 0;
    float v = (log10f(bps) - 3.0f) / 5.0f;  // 1KB/s .. 100MB/s
    return (uint8_t)(clampf(v, 0, 1) * 255);
}

static void add_spark(float x, float y, rgb_t c)
{
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (s_spark[i].life <= 0) {
            s_spark[i] = (spark_t){x, y, frand(-34, -18), frand(0.5f, 1.0f), c};
            return;
        }
    }
}

// ============================================================ 小部件

// 分段条：HUD 风格，一格一格亮
static void seg_bar(int x, int y, int w, int h, float v, rgb_t c)
{
    int n = w / 5;
    int lit = (int)(v * n + 0.5f);
    for (int i = 0; i < n; i++) {
        if (i < lit) fb_fill(x + i * 5, y, 4, h, c.r, c.g, c.b, 230);
        else fb_fill(x + i * 5, y, 4, h, DIM.r, DIM.g, DIM.b, 60);
    }
}

// 走势线：从旧到新画在 [x, x+w)，值 0..255。一列一道竖线连到上一个点，1 像素宽不用抗锯齿
static void spark_line(const uint8_t *hist, int x, int y, int w, int h, rgb_t c, bool fill)
{
    int n = s_hn < w ? s_hn : w;
    int prev = 0;
    for (int k = 0; k < n; k++) {
        int idx = (s_hi - n + k + HIST * 4) % HIST;
        int py = y + h - 1 - hist[idx] * (h - 1) / 255;
        int px = x + w - n + k;
        if (fill && py + 1 < y + h) {
            fb_fill(px, py + 1, 1, y + h - py - 1, c.r, c.g, c.b, 30);
        }
        int top = k > 0 && prev < py ? prev : py, bot = k > 0 && prev > py ? prev : py;
        fb_fill(px, top, 1, bot - top + 1, c.r, c.g, c.b, 230);
        prev = py;
    }
}

static void label(int x, int y, const char *s)
{
    text_plain(&font_tiny, x, y, s, 2, HUD.r, HUD.g, HUD.b, 220);
}

// 小三角：up=true 朝上
static void tri(int x, int y, bool up, rgb_t c)
{
    for (int i = 0; i < 4; i++) {
        int yy = up ? y + i : y + 3 - i;
        fb_fill(x + 3 - i, yy, 1 + 2 * i, 1, c.r, c.g, c.b, 230);
    }
}

// ============================================================ 画面

static void draw_cores(int x0, int y0, int w, int h)
{
    int n = s_snap.ncore, ne = s_snap.ne;
    if (n <= 0) return;
    int groups = (ne > 0 && ne < n) ? 2 : 1;
    int ggap = 8, gap = 3;
    int bw = (w - (n - 1) * gap - (groups - 1) * ggap) / n;
    if (bw > 18) bw = 18;
    int total = n * bw + (n - 1) * gap + (groups - 1) * ggap;
    int x = x0 + (w - total);  // 靠右对齐
    int gx0[2] = {x, 0}, gx1[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        if (groups == 2 && i == ne) {
            gx1[0] = x - gap;
            x += ggap;
            gx0[1] = x;
        }
        float v = s_core[i];
        rgb_t c = heat(v);
        // 槽
        fb_fill(x, y0, bw, h, 20, 28, 48, 150);
        // 柱：底暗顶亮
        int bh = (int)(v * h + 0.5f);
        for (int yy = 0; yy < bh; yy++) {
            float k = 0.45f + 0.55f * (float)yy / (bh > 1 ? bh - 1 : 1);
            fb_fill(x, y0 + h - 1 - yy, bw, 1, (uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k), 255);
        }
        if (bh > 0) {
            fb_fill(x, y0 + h - bh, bw, 1, 255, 255, 255, 140);  // 顶上一道亮边
            trail_glow(x + bw / 2.0f, y0 + h - bh, 10 + 8 * v, c.r, c.g, c.b, 0.25f + 0.35f * v);
        }
        if (v > 0.85f && frand(0, 1) < 0.25f) add_spark(x + frand(0, bw), y0 + h - bh, c);
        x += bw + gap;
    }
    gx1[groups - 1] = x - gap;
    // 组标签
    const char *names[2] = {groups == 2 ? "E" : "CORES", "P"};
    for (int g = 0; g < groups; g++) {
        int cx = (gx0[g] + gx1[g]) / 2;
        fb_fill(gx0[g], y0 + h + 3, gx1[g] - gx0[g], 1, DIM.r, DIM.g, DIM.b, 140);
        int lw = text_width_sp(&font_tiny, names[g], 2);
        fb_fill(cx - lw / 2 - 3, y0 + h + 1, lw + 6, 5, 6, 8, 18, 255);
        text_plain(&font_tiny, cx - lw / 2, y0 + h - 1, names[g], 2, HUD.r, HUD.g, HUD.b, 200);
    }
}

static void draw_procs(int y0)
{
    label(8, y0, "APP");
    text_align(&font_tiny, 262, y0, ALIGN_RIGHT, "CPU", 2, HUD.r, HUD.g, HUD.b, 220);
    text_align(&font_tiny, 312, y0, ALIGN_RIGHT, "MEM", 2, HUD.r, HUD.g, HUD.b, 220);
    int maxc = 100;
    for (int i = 0; i < s_snap.ntop; i++) {
        if (s_snap.top[i].cpu > maxc) maxc = s_snap.top[i].cpu;
    }
    for (int i = 0; i < s_snap.ntop && i < 5; i++) {
        const proc_t *p = &s_snap.top[i];
        int y = y0 + 12 + i * 14;
        bool agent = p->kind[0] != 0;
        rgb_t c = agent ? agent_color(p->kind) : heat(p->cpu / 100.0f);
        // 行底：按 CPU 占比铺一道淡色
        int bw = (int)(220.0f * p->cpu / maxc);
        if (bw > 0) fb_fill(8, y + 1, bw, 12, c.r, c.g, c.b, 26);
        fb_fill(8, y + 1, 2, 12, c.r, c.g, c.b, agent ? 230 : 120);
        char name[56];
        snprintf(name, sizeof(name), "%s", p->name);
        text_fit(&font_small, name, sizeof(name), 170);
        rgb_t nc = agent ? mix(c, TXT, 0.45f) : TXT;
        text_plain(&font_small, 15, y, name, 0, nc.r, nc.g, nc.b, 235);
        char v[12];
        snprintf(v, sizeof(v), "%d%%", p->cpu);
        rgb_t vc = p->cpu >= 100 ? heat(0.8f) : TXT;
        text_align(&font_mono, 262, y + 1, ALIGN_RIGHT, v, 0, vc.r, vc.g, vc.b, 235);
        fmt_mem(p->mem, v, sizeof(v));
        text_align(&font_mono, 312, y + 1, ALIGN_RIGHT, v, 0, HUD.r, HUD.g, HUD.b, 235);
    }
}

static void sys_enter(void)
{
    for (int y = 0; y < GFX_H; y++) {
        float t = (float)y / GFX_H;
        gfx_set_bg_row(y, (uint8_t)(5 + 6 * t), (uint8_t)(7 + 5 * t), (uint8_t)(16 + 14 * t));
    }
    memset(s_spark, 0, sizeof(s_spark));
}

static void sys_frame(float dt, int64_t now_ms)
{
    s_t += dt;
    const snapshot_t *s = &s_snap;
    bool online = s->valid && !s->bye && now_ms - s->recv_ms < STALE_MS;

    for (int i = 0; i < s->ncore; i++) {
        s_core[i] = approach(s_core[i], online ? s->core[i] / 100.0f : 0, 6, dt);
    }
    s_cpu = approach(s_cpu, online ? s->cpu / 100.0f : 0, 6, dt);
    s_gpu = approach(s_gpu, online && s->gpu >= 0 ? s->gpu / 100.0f : 0, 6, dt);
    s_mem = approach(s_mem, online && s->mt > 0 ? s->mu / s->mt : 0, 6, dt);

    // ---- 光层 ----
    trail_fade(190);
    for (int i = 0; i < MAX_SPARKS; i++) {
        spark_t *p = &s_spark[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->y += p->vy * dt;
        trail_dot(p->x, p->y, p->col.r, p->col.g, p->col.b, p->life);
    }
    // 柱顶的光是 draw_cores 在合成之后才加进光层的，下一帧才显出来，差一帧看不出
    gfx_compose(GFX_H);

    // 淡淡的网格
    for (int x = 0; x < GFX_W; x += 20) fb_fill(x, 20, 1, GFX_H - 20, 60, 90, 140, 10);
    for (int y = 20; y < GFX_H; y += 20) fb_fill(0, y, GFX_W, 1, 60, 90, 140, 10);

    // ---- 顶栏 ----
    char chip[24];
    snprintf(chip, sizeof(chip), "%s", s->chip[0] ? s->chip : "MAC");
    for (char *p = chip; *p; p++) {
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
    int cw = text_align(&font_tiny, 8, 6, ALIGN_LEFT, chip, 2, C_CYAN.r, C_CYAN.g, C_CYAN.b, 240);
    if (s->valid) {
        char info[48], up[16];
        fmt_uptime(s->uptime, up, sizeof(up));
        snprintf(info, sizeof(info), "\xC2\xB7 %dC \xC2\xB7 %.0fG \xC2\xB7 UP %s", s->ncore, s->mt, up);
        text_plain(&font_tiny, 8 + cw + 5, 6, info, 1, DIM.r, DIM.g, DIM.b, 230);
    }
    text_align(&font_label, 312, 2, ALIGN_RIGHT, s->valid ? s->clock : "--:--", 0, TXT.r, TXT.g, TXT.b, online ? 240 : 120);
    fb_fill(8, 19, 304, 1, C_CYAN.r, C_CYAN.g, C_CYAN.b, 40);

    if (!s->valid) {
        text_align(&font_label, 160, 100, ALIGN_CENTER, "SYSTEM", 4, TXT.r, TXT.g, TXT.b, 230);
        text_align(&font_small, 160, 124, ALIGN_CENTER, "waiting for the mac", 0, HUD.r, HUD.g, HUD.b,
                   (int)(160 + 60 * sinf(s_t * 1.5f)));
        return;
    }

    // ---- CPU ----
    label(8, 24, "CPU");
    char ld[16];
    snprintf(ld, sizeof(ld), "LOAD %.1f", s->load);
    text_align(&font_tiny, 96, 24, ALIGN_RIGHT, ld, 0, DIM.r, DIM.g, DIM.b, 230);
    char num[8];
    snprintf(num, sizeof(num), "%d", (int)(s_cpu * 100 + 0.5f));
    rgb_t cc = heat(s_cpu);
    int nw = text_align(&font_big, 6, 28, ALIGN_LEFT, num, 0, TXT.r, TXT.g, TXT.b, 245);
    text_plain(&font_num, 8 + nw, 40, "%", 0, cc.r, cc.g, cc.b, 230);
    fb_frame(8, 72, 88, 20, DIM.r, DIM.g, DIM.b, 50);
    spark_line(s_hcpu, 9, 73, 86, 18, C_CYAN, true);
    draw_cores(104, 26, 208, 52);

    // ---- GPU / 内存 ----
    int y = 98;
    label(8, y, "GPU");
    char v[24];
    if (s->gpu >= 0) snprintf(v, sizeof(v), "%d%%", (int)(s_gpu * 100 + 0.5f));
    else snprintf(v, sizeof(v), "--");
    text_align(&font_mono, 152, y - 2, ALIGN_RIGHT, v, 0, TXT.r, TXT.g, TXT.b, 240);
    seg_bar(8, y + 12, 145, 6, s_gpu, heat(s_gpu));

    label(166, y, "MEM");
    snprintf(v, sizeof(v), "%.1f/%.0fG", s->mu, s->mt);
    text_align(&font_mono, 312, y - 2, ALIGN_RIGHT, v, 0, TXT.r, TXT.g, TXT.b, 240);
    // 内存条按「内存压力」上色：系统真正觉得紧不紧
    rgb_t mc = s->mp >= 75 ? RGB(255, 90, 120) : (s->mp >= 50 ? RGB(255, 200, 90) : RGB(90, 230, 170));
    seg_bar(166, y + 12, 146, 6, s_mem, mc);
    snprintf(v, sizeof(v), "PRESSURE %d%%", s->mp);
    text_plain(&font_tiny, 166, y + 22, v, 0, mc.r, mc.g, mc.b, 220);
    if (s->swt > 0) {
        snprintf(v, sizeof(v), "SWAP %.1fG", s->sw);
        rgb_t sc = s->sw > 2 ? RGB(255, 200, 90) : DIM;
        text_align(&font_tiny, 312, y + 22, ALIGN_RIGHT, v, 0, sc.r, sc.g, sc.b, 220);
    }
    if (s->gpu >= 0) {
        // GPU 下面放网速走势（对数刻度）：下行实线、上行淡线
        fb_frame(8, y + 21, 145, 14, DIM.r, DIM.g, DIM.b, 40);
        spark_line(s_hdown, 9, y + 22, 143, 12, RGB(90, 230, 170), false);
        spark_line(s_hup, 9, y + 22, 143, 12, RGB(255, 122, 217), false);
    }

    // ---- 网络 / 磁盘 / 电池 ----
    y = 136;
    char r1[12], r2[12];
    fmt_rate(s->up_bps, r1, sizeof(r1));
    fmt_rate(s->down_bps, r2, sizeof(r2));
    tri(8, y + 3, true, RGB(255, 122, 217));
    text_plain(&font_mono, 16, y, r1, 0, TXT.r, TXT.g, TXT.b, 230);
    tri(62, y + 3, false, RGB(90, 230, 170));
    text_plain(&font_mono, 70, y, r2, 0, TXT.r, TXT.g, TXT.b, 230);

    label(124, y + 1, "SSD");
    snprintf(v, sizeof(v), "%dG", s->df);
    rgb_t dc = s->dp >= 95 ? RGB(255, 90, 120) : (s->dp >= 90 ? RGB(255, 200, 90) : TXT);
    int dw = text_align(&font_mono, 150, y, ALIGN_LEFT, v, 0, dc.r, dc.g, dc.b, 230);
    text_plain(&font_tiny, 150 + dw + 3, y + 1, "FREE", 1, DIM.r, DIM.g, DIM.b, 220);

    if (s->has_bat) {
        snprintf(v, sizeof(v), "%d%%", s->bat);
        int bx = 222;
        fb_frame(bx, y + 1, 16, 8, HUD.r, HUD.g, HUD.b, 180);
        fb_fill(bx + 16, y + 3, 2, 4, HUD.r, HUD.g, HUD.b, 180);
        rgb_t bc = s->bat <= 20 ? RGB(255, 90, 120) : RGB(90, 230, 170);
        fb_fill(bx + 2, y + 3, 12 * s->bat / 100, 4, bc.r, bc.g, bc.b, 230);
        text_plain(&font_mono, bx + 22, y, v, 0, TXT.r, TXT.g, TXT.b, 230);
        // 优先显示整机功耗；没有就显示电池充放电功率
        if (s->pw >= 0) {
            snprintf(v, sizeof(v), "%.0fW", s->pw);
            text_align(&font_mono, 312, y, ALIGN_RIGHT, v, 0, s->bat_ac ? HUD.r : 255, s->bat_ac ? HUD.g : 200,
                       s->bat_ac ? HUD.b : 90, 230);
        } else if (fabsf(s->bat_w) >= 0.5f) {
            snprintf(v, sizeof(v), "%+.0fW", s->bat_w);
            text_align(&font_tiny, 312, y + 1, ALIGN_RIGHT, v, 0, DIM.r, DIM.g, DIM.b, 230);
        } else if (s->bat_ac) {
            text_align(&font_tiny, 312, y + 1, ALIGN_RIGHT, "AC", 1, DIM.r, DIM.g, DIM.b, 230);
        }
    }
    fb_fill(8, 151, 304, 1, C_CYAN.r, C_CYAN.g, C_CYAN.b, 30);

    // ---- 进程 ----
    draw_procs(156);

    if (s->bye) {
        fb_rbox(90, 104, 140, 22, 6, 0, 0, 0, 200);
        text_align(&font_small, 160, 108, ALIGN_CENTER, "stopped on the mac", 0, HUD.r, HUD.g, HUD.b, 230);
    } else if (!online) {
        fb_rbox(90, 104, 140, 22, 6, 0, 0, 0, 200);
        text_align(&font_small, 160, 108, ALIGN_CENTER, "no word from the mac", 0, HUD.r, HUD.g, HUD.b, 230);
    }
}

// ============================================================ 接入面板

static void sys_init(void)
{
}

static void sys_data(const cJSON *root, int64_t now_ms)
{
    snapshot_t *s = &s_snap;
    memset(s, 0, sizeof(*s));
    s->valid = true;
    s->recv_ms = now_ms;
    json_str(s->clock, sizeof(s->clock), root, "t");
    json_str(s->chip, sizeof(s->chip), root, "chip");
    s->ne = (int)json_num(root, "ne", 0);
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "c")) {
        if (s->ncore >= MAX_CORES) break;
        s->core[s->ncore++] = (uint8_t)clampf(cJSON_IsNumber(it) ? (float)it->valuedouble : 0, 0, 100);
    }
    s->cpu = (int)json_num(root, "cpu", 0);
    s->gpu = (int)json_num(root, "gpu", -1);
    s->mp = (int)json_num(root, "mp", 0);
    s->mu = (float)json_num(root, "mu", 0);
    s->mt = (float)json_num(root, "mt", 0);
    s->sw = (float)json_num(root, "sw", 0);
    s->swt = (float)json_num(root, "swt", 0);
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "net");
    if (cJSON_GetArraySize(a) >= 2) {
        s->up_bps = (float)cJSON_GetArrayItem(a, 0)->valuedouble;
        s->down_bps = (float)cJSON_GetArrayItem(a, 1)->valuedouble;
    }
    a = cJSON_GetObjectItemCaseSensitive(root, "dio");
    if (cJSON_GetArraySize(a) >= 2) {
        s->rd_bps = (float)cJSON_GetArrayItem(a, 0)->valuedouble;
        s->wr_bps = (float)cJSON_GetArrayItem(a, 1)->valuedouble;
    }
    s->df = (int)json_num(root, "df", 0);
    s->dp = (int)json_num(root, "dp", 0);
    a = cJSON_GetObjectItemCaseSensitive(root, "bat");
    if (cJSON_GetArraySize(a) >= 3) {
        s->has_bat = true;
        s->bat = cJSON_GetArrayItem(a, 0)->valueint;
        s->bat_ac = cJSON_GetArrayItem(a, 1)->valueint != 0;
        s->bat_w = (float)cJSON_GetArrayItem(a, 2)->valuedouble;
    }
    s->pw = (float)json_num(root, "pw", -1);
    s->uptime = (int32_t)json_num(root, "up", 0);
    s->load = (float)json_num(root, "ld", 0);
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "top")) {
        if (s->ntop >= MAX_TOP) break;
        proc_t *p = &s->top[s->ntop++];
        json_str(p->name, sizeof(p->name), it, "n");
        json_str(p->kind, sizeof(p->kind), it, "k");
        p->cpu = (int)json_num(it, "c", 0);
        p->mem = (int)json_num(it, "m", 0);
    }

    s_hcpu[s_hi] = (uint8_t)(clampf(s->cpu / 100.0f, 0, 1) * 255);
    s_hdown[s_hi] = log_level(s->down_bps);
    s_hup[s_hi] = log_level(s->up_bps);
    s_hi = (s_hi + 1) % HIST;
    if (s_hn < HIST) s_hn++;
}

static void sys_bye(void)
{
    s_snap.bye = true;
}

static int sys_badge(int64_t now_ms)
{
    (void)now_ms;
    if (s_snap.mp >= 75 || (s_snap.dp >= 97 && s_snap.df > 0)) return 2;  // 内存吃紧 / 磁盘快满
    return s_snap.cpu >= 70 ? 1 : 0;
}

// 磁贴：CPU 大字 + 各核小柱 + GPU / 内存细条 + 最忙的 App
static void sys_tile(const tile_t *t, int64_t now_ms)
{
    (void)now_ms;
    const snapshot_t *s = &s_snap;
    char num[8];
    snprintf(num, sizeof(num), "%d", s->cpu);
    int nw = text_align(&font_num, t->x, t->y - 3, ALIGN_LEFT, num, 0, TXT.r, TXT.g, TXT.b, 240);
    text_plain(&font_tiny, t->x + nw + 3, t->y + 2, "% CPU", 1, HUD.r, HUD.g, HUD.b, 210);

    // 各核小柱，右上角
    int n = s->ncore, bw = 4, gap = 2;
    int x = t->x + t->w - (n * (bw + gap) - gap) - (s->ne > 0 ? 3 : 0);
    for (int i = 0; i < n; i++) {
        if (i == s->ne && s->ne > 0) x += 3;
        float v = s->core[i] / 100.0f;
        rgb_t c = heat(v);
        int h = 18, bh = (int)(v * h + 0.5f);
        fb_fill(x, t->y, bw, h, 20, 28, 48, 160);
        if (bh) fb_fill(x, t->y + h - bh, bw, bh, c.r, c.g, c.b, 235);
        x += bw + gap;
    }

    int y = t->y + 24;
    char v[16];
    text_plain(&font_tiny, t->x, y, "GPU", 1, HUD.r, HUD.g, HUD.b, 210);
    snprintf(v, sizeof(v), s->gpu >= 0 ? "%d%%" : "--", s->gpu);
    text_align(&font_tiny, t->x + 62, y, ALIGN_RIGHT, v, 0, TXT.r, TXT.g, TXT.b, 230);
    seg_bar(t->x, y + 9, 62, 3, s->gpu > 0 ? s->gpu / 100.0f : 0, heat(s->gpu / 100.0f));

    int mx = t->x + 70;
    rgb_t mc = s->mp >= 75 ? RGB(255, 90, 120) : (s->mp >= 50 ? RGB(255, 200, 90) : RGB(90, 230, 170));
    text_plain(&font_tiny, mx, y, "MEM", 1, HUD.r, HUD.g, HUD.b, 210);
    snprintf(v, sizeof(v), "%d%%", s->mp);
    text_align(&font_tiny, t->x + t->w, y, ALIGN_RIGHT, v, 0, mc.r, mc.g, mc.b, 230);
    seg_bar(mx, y + 9, t->w - 70, 3, s->mt > 0 ? s->mu / s->mt : 0, mc);

    if (s->ntop > 0) {
        const proc_t *p = &s->top[0];
        char line[56];
        snprintf(line, sizeof(line), "%s", p->name);
        text_fit(&font_tiny, line, sizeof(line), t->w - 40);
        rgb_t c = p->kind[0] ? agent_color(p->kind) : TXT;
        text_plain(&font_tiny, t->x, t->y + t->h - 9, line, 0, c.r, c.g, c.b, 220);
        snprintf(v, sizeof(v), "%d%%", p->cpu);
        text_align(&font_tiny, t->x + t->w, t->y + t->h - 9, ALIGN_RIGHT, v, 0, TXT.r, TXT.g, TXT.b, 220);
    }
}

const app_t app_sys = {
    .id = "sys",
    .name = "SYSTEM",
    .color = {0, 234, 255},
    .init = sys_init,
    .data = sys_data,
    .bye = sys_bye,
    .enter = sys_enter,
    .frame = sys_frame,
    .tile = sys_tile,
    .badge = sys_badge,
};
