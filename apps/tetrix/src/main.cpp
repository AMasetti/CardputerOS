// Tetrix lateral para M5Stack Cardputer / Cardputer-Adv
//
// El tablero de 10x20 está acostado: las piezas entran por la izquierda y
// "caen" hacia la derecha, así aprovecha el ancho de la pantalla. Las teclas
// siguen esa rotación:
//   ;  .         mover arriba / abajo                (también w s)
//   ,            rotar horario                       (también a, x)
//   z            rotar antihorario
//   /            bajar rápido (hacia la derecha)     (también d)
//   space        soltar de golpe
//   enter        empezar / reintentar / soltar de golpe
//   p            pausa
//   m            sonido on/off
//
// El récord se guarda en la flash (NVS), así que sobrevive al apagado.

#include <M5Cardputer.h>
#include <Preferences.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// --- Pantalla y tablero ------------------------------------------------------
// El tablero lógico es el clásico (W columnas, H filas, gravedad hacia +y) y
// se dibuja rotado 90° antihorario: +y va a la derecha y +x va hacia arriba.
// Al ser una rotación (no un espejo), rotar horario en el tablero lógico
// también se ve horario en la pantalla.
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;
static constexpr int W = 10;
static constexpr int H = 20;
static constexpr int CELL = 10;
static constexpr int BOARD_X = 4;
static constexpr int BOARD_Y = HUD_H + (SCREEN_H - HUD_H - W * CELL) / 2;  // 25
static constexpr int PANEL_X = BOARD_X + H * CELL + 6;                       // 210
static constexpr int MINI = 5;  // celda del preview de la próxima pieza

// --- Tiempos -----------------------------------------------------------------
static constexpr uint32_t LOCK_MS = 450;     // tiempo apoyada antes de fijarse
static constexpr int MAX_LOCK_RESETS = 15;   // movimientos que reinician ese tiempo
static constexpr uint32_t DAS_MS = 160;      // demora antes de autorepetir
static constexpr uint32_t ARR_MS = 45;       // intervalo de autorepetición
static constexpr uint32_t SOFT_MS = 35;      // intervalo de bajada rápida
static constexpr uint32_t CLEAR_FLASH_MS = 160;

enum class State { Title, Playing, Paused, GameOver };

// Piezas en su rotación inicial, dentro de una caja de n x n.
struct PieceDef {
    uint8_t n;
    int8_t cells[4][2];
};

static constexpr int NUM_PIECES = 7;
static const PieceDef PIECES[NUM_PIECES] = {
    {4, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}},  // I
    {2, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},  // O
    {3, {{1, 0}, {0, 1}, {1, 1}, {2, 1}}},  // T
    {3, {{1, 0}, {2, 0}, {0, 1}, {1, 1}}},  // S
    {3, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}},  // Z
    {3, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}},  // J
    {3, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}},  // L
};

// Desplazamientos que se prueban al rotar contra una pared o el piso.
static const int8_t KICKS[][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {-2, 0}, {2, 0}, {0, -2}};

struct Piece {
    uint8_t type;
    uint8_t rot;  // 0..3
    int8_t x, y;  // esquina de la caja n x n en el tablero
};

static M5Canvas canvas(&M5Cardputer.Display);
static Preferences prefs;

static uint8_t board[H][W];  // 0 vacío, 1..7 tipo de pieza + 1
static Piece cur;
static uint8_t nextType;
static uint8_t bag[NUM_PIECES];
static int bagPos = NUM_PIECES;
static State state = State::Title;
static bool dirty = true;
static bool soundOn = true;

static uint32_t score;
static uint32_t highScore;
static bool newRecord;
static int lines;
static int level;
static uint32_t gravityMs;
static uint32_t lastFall;
static bool grounded;
static uint32_t groundedSince;
static int lockResets;

// Colores (RGB565)
static uint16_t C_BG, C_GRID, C_FRAME, C_FLOOR, C_HUD, C_TEXT, C_ACCENT, C_FLASH;
static uint16_t C_PIECE[NUM_PIECES], C_LIGHT[NUM_PIECES], C_DARK[NUM_PIECES];

static void beep(float freq, uint32_t ms)
{
    if (soundOn) M5Cardputer.Speaker.tone(freq, ms);
}

// --- Piezas ------------------------------------------------------------------
static void pieceCells(const Piece &p, int8_t out[4][2])
{
    const PieceDef &d = PIECES[p.type];
    for (int i = 0; i < 4; i++) {
        int x = d.cells[i][0], y = d.cells[i][1];
        for (int r = 0; r < p.rot; r++) {  // horario: (x, y) -> (n-1-y, x)
            int t = x;
            x = d.n - 1 - y;
            y = t;
        }
        out[i][0] = p.x + x;
        out[i][1] = p.y + y;
    }
}

static bool fits(const Piece &p)
{
    int8_t c[4][2];
    pieceCells(p, c);
    for (auto &k : c) {
        if (k[0] < 0 || k[0] >= W || k[1] < 0 || k[1] >= H) return false;
        if (board[k[1]][k[0]]) return false;
    }
    return true;
}

static uint8_t drawFromBag()
{
    // Bolsa de 7: cada pieza sale una vez por ronda, en orden aleatorio.
    if (bagPos >= NUM_PIECES) {
        for (int i = 0; i < NUM_PIECES; i++) bag[i] = i;
        for (int i = NUM_PIECES - 1; i > 0; i--) {
            int j = random(i + 1);
            uint8_t t = bag[i];
            bag[i] = bag[j];
            bag[j] = t;
        }
        bagPos = 0;
    }
    return bag[bagPos++];
}

static uint32_t gravityFor(int lv)
{
    float ms = 800.0f;
    for (int i = 1; i < lv; i++) ms *= 0.8f;
    return max<uint32_t>(50, (uint32_t)ms);
}

static void gameOver()
{
    state = State::GameOver;
    if (score > highScore) {
        highScore = score;
        newRecord = true;
        prefs.putUInt("high", highScore);
    }
    beep(330, 120);
    delay(130);
    beep(220, 120);
    delay(130);
    beep(110, 300);
}

static void spawn()
{
    cur.type = nextType;
    cur.rot = 0;
    cur.x = (W - PIECES[cur.type].n) / 2;
    cur.y = 0;
    nextType = drawFromBag();
    grounded = false;
    lockResets = 0;
    lastFall = millis();
    if (!fits(cur)) gameOver();
}

static void newGame()
{
    memset(board, 0, sizeof(board));
    score = 0;
    lines = 0;
    level = 1;
    newRecord = false;
    gravityMs = gravityFor(level);
    bagPos = NUM_PIECES;
    nextType = drawFromBag();
    state = State::Playing;
    spawn();
}

// Un movimiento que sale bien con la pieza apoyada reinicia el tiempo de fijado.
static void touchLock()
{
    if (grounded && lockResets < MAX_LOCK_RESETS) {
        groundedSince = millis();
        lockResets++;
    }
}

static bool tryMove(int dx, int dy)
{
    Piece p = cur;
    p.x += dx;
    p.y += dy;
    if (!fits(p)) return false;
    cur = p;
    if (dy == 0) touchLock();
    return true;
}

static void tryRotate(int dir)  // +1 horario, -1 antihorario
{
    Piece p = cur;
    p.rot = (p.rot + (dir > 0 ? 1 : 3)) & 3;
    for (auto &k : KICKS) {
        Piece q = p;
        q.x += k[0];
        q.y += k[1];
        if (fits(q)) {
            cur = q;
            touchLock();
            beep(1800, 8);
            return;
        }
    }
}

static int dropDistance()
{
    Piece p = cur;
    int d = 0;
    while (true) {
        p.y++;
        if (!fits(p)) return d;
        d++;
    }
}

static void render();

static void lockPiece()
{
    int8_t c[4][2];
    pieceCells(cur, c);
    for (auto &k : c) board[k[1]][k[0]] = cur.type + 1;

    // Filas completas (en pantalla se ven como columnas).
    bool full[H] = {};
    int cleared = 0;
    for (int y = 0; y < H; y++) {
        bool f = true;
        for (int x = 0; x < W && f; x++) f = board[y][x] != 0;
        full[y] = f;
        cleared += f;
    }

    if (cleared) {
        // Destello corto sobre las filas que se van.
        render();
        for (int y = 0; y < H; y++) {
            if (full[y]) canvas.fillRect(BOARD_X + y * CELL, BOARD_Y, CELL - 1, W * CELL - 1, C_FLASH);
        }
        canvas.pushSprite(0, 0);
        static const float NOTES[] = {660, 880, 1100, 1320};
        for (int i = 0; i < cleared; i++) {
            beep(NOTES[i], 35);
            delay(40);
        }
        delay(CLEAR_FLASH_MS > 40u * cleared ? CLEAR_FLASH_MS - 40 * cleared : 0);

        int dst = H - 1;
        for (int y = H - 1; y >= 0; y--) {
            if (full[y]) continue;
            if (dst != y) memcpy(board[dst], board[y], W);
            dst--;
        }
        for (; dst >= 0; dst--) memset(board[dst], 0, W);

        static const uint16_t POINTS[] = {0, 100, 300, 500, 800};
        score += POINTS[cleared] * level;
        lines += cleared;
        int newLevel = lines / 10 + 1;
        if (newLevel != level) {
            level = newLevel;
            gravityMs = gravityFor(level);
            beep(1600, 60);
        }
    } else {
        beep(240, 15);
    }
    spawn();
}

static void hardDrop()
{
    int d = dropDistance();
    cur.y += d;
    score += 2 * d;
    lockPiece();
}

static void softDrop()
{
    if (tryMove(0, 1)) {
        score++;
        lastFall = millis();
    }
}

// --- Dibujo ------------------------------------------------------------------
static void drawBlock(int sx, int sy, int size, uint8_t type)
{
    canvas.fillRect(sx, sy, size - 1, size - 1, C_PIECE[type]);
    if (size >= 6) {
        canvas.drawFastHLine(sx, sy, size - 1, C_LIGHT[type]);
        canvas.drawFastVLine(sx, sy, size - 1, C_LIGHT[type]);
        canvas.drawFastHLine(sx, sy + size - 2, size - 1, C_DARK[type]);
        canvas.drawFastVLine(sx + size - 2, sy, size - 1, C_DARK[type]);
    }
}

// Celda lógica (x, y) -> esquina en pantalla.
static inline int cellSX(int y) { return BOARD_X + y * CELL; }
static inline int cellSY(int x) { return BOARD_Y + (W - 1 - x) * CELL; }

static void drawHud()
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.setTextDatum(middle_left);
    canvas.drawString("TETRIX", 4, HUD_H / 2);
    canvas.setTextDatum(middle_center);
    canvas.drawString(String("Puntos ") + score, SCREEN_W / 2, HUD_H / 2);
    canvas.setTextDatum(middle_right);
    canvas.drawString(String("Record ") + highScore, SCREEN_W - 4, HUD_H / 2);
}

static void drawBoard()
{
    canvas.fillRect(0, HUD_H, SCREEN_W, SCREEN_H - HUD_H, C_BG);
    canvas.drawRect(BOARD_X - 2, BOARD_Y - 2, H * CELL + 3, W * CELL + 3, C_FRAME);
    // El "piso" queda a la derecha: se marca para que se lea hacia dónde caen.
    canvas.fillRect(BOARD_X + H * CELL + 1, BOARD_Y - 2, 2, W * CELL + 3, C_FLOOR);

    for (int y = 1; y < H; y++) {
        for (int x = 1; x < W; x++) canvas.drawPixel(cellSX(y) - 1, cellSY(x) + CELL - 1, C_GRID);
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (board[y][x]) drawBlock(cellSX(y), cellSY(x), CELL, board[y][x] - 1);
        }
    }

    if (state == State::Playing || state == State::Paused) {
        int8_t c[4][2];
        // Sombra: dónde va a caer la pieza.
        Piece ghost = cur;
        ghost.y += dropDistance();
        pieceCells(ghost, c);
        for (auto &k : c) canvas.drawRect(cellSX(k[1]), cellSY(k[0]), CELL - 1, CELL - 1, C_DARK[cur.type]);

        pieceCells(cur, c);
        for (auto &k : c) drawBlock(cellSX(k[1]), cellSY(k[0]), CELL, cur.type);
    }
}

static void drawPanel()
{
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(C_ACCENT, C_BG);
    const int cx = (PANEL_X + SCREEN_W) / 2;
    canvas.drawString("SIG", cx, BOARD_Y);

    // Próxima pieza, rotada igual que el tablero y centrada en una caja de 4x4.
    const PieceDef &d = PIECES[nextType];
    const int boxX = cx - 2 * MINI, boxY = BOARD_Y + 12;
    const int off = (4 - d.n) * MINI / 2;
    for (auto &k : d.cells) {
        drawBlock(boxX + off + k[1] * MINI, boxY + off + (d.n - 1 - k[0]) * MINI, MINI, nextType);
    }

    canvas.setTextColor(C_ACCENT, C_BG);
    canvas.drawString("NIV", cx, BOARD_Y + 40);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(String(level), cx, BOARD_Y + 50);
    canvas.setTextColor(C_ACCENT, C_BG);
    canvas.drawString("LIN", cx, BOARD_Y + 68);
    canvas.setTextColor(C_TEXT, C_BG);
    canvas.drawString(String(lines), cx, BOARD_Y + 78);
}

static void drawCenteredBox(const char *title, const char *line1, const char *line2, const char *line3 = nullptr)
{
    const int w = 200, h = line3 ? 80 : 70;
    const int x = (SCREEN_W - w) / 2, y = HUD_H + (SCREEN_H - HUD_H - h) / 2;
    canvas.fillRoundRect(x, y, w, h, 6, C_HUD);
    canvas.drawRoundRect(x, y, w, h, 6, C_ACCENT);
    canvas.setTextDatum(top_center);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_ACCENT, C_HUD);
    canvas.drawString(title, SCREEN_W / 2, y + 8);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.drawString(line1, SCREEN_W / 2, y + 34);
    canvas.drawString(line2, SCREEN_W / 2, y + 48);
    if (line3) canvas.drawString(line3, SCREEN_W / 2, y + 62);
}

static void render()
{
    drawHud();
    drawBoard();
    drawPanel();
    switch (state) {
        case State::Title:
            drawCenteredBox("TETRIX", "enter jugar   ; . mover", ", rotar   / bajar", "space soltar  p pausa  m sonido");
            break;
        case State::Paused:
            drawCenteredBox("PAUSA", "p seguir", soundOn ? "m sonido: on" : "m sonido: off");
            break;
        case State::GameOver:
            drawCenteredBox(newRecord ? "NUEVO RECORD!" : "GAME OVER", (String("Puntos: ") + score).c_str(),
                            (String("Lineas: ") + lines + "  Nivel: " + level).c_str(), "enter otra vez");
            break;
        default:
            break;
    }
    canvas.pushSprite(0, 0);
}

// --- Entrada -----------------------------------------------------------------
// Las teclas se leen como "mantenidas" en cada vuelta para poder autorepetir
// los movimientos (DAS) mientras se mantienen apretadas.
enum Action : uint16_t {
    A_UP = 1 << 0,     // +x lógico (arriba en pantalla)
    A_DOWN = 1 << 1,   // -x lógico
    A_SOFT = 1 << 2,
    A_ROT_CW = 1 << 3,
    A_ROT_CCW = 1 << 4,
    A_HARD = 1 << 5,
    A_START = 1 << 6,
    A_PAUSE = 1 << 7,
    A_SOUND = 1 << 8,
};

static uint16_t heldPrev;
static uint32_t heldSince[3];  // A_UP, A_DOWN, A_SOFT
static uint32_t lastRepeat[3];

static uint16_t readHeld()
{
    if (!M5Cardputer.Keyboard.isPressed()) return 0;
    auto ks = M5Cardputer.Keyboard.keysState();
    uint16_t a = 0;
    if (ks.enter) a |= A_START | A_HARD;
    for (char c : ks.word) {
        switch (c) {
            case ';': case 'w': case 'W': a |= A_UP; break;
            case '.': case 's': case 'S': a |= A_DOWN; break;
            case '/': case 'd': case 'D': a |= A_SOFT; break;
            case ',': case 'a': case 'A': case 'x': case 'X': a |= A_ROT_CW; break;
            case 'z': case 'Z': a |= A_ROT_CCW; break;
            case ' ': a |= A_HARD; break;
            case 'p': case 'P': a |= A_PAUSE; break;
            case 'm': case 'M': a |= A_SOUND; break;
        }
    }
    return a;
}

// true si la acción repetible debe dispararse en esta vuelta.
static bool repeatFires(int idx, uint16_t bit, uint16_t held, uint16_t pressed, uint32_t interval, uint32_t now)
{
    if (!(held & bit)) return false;
    if (pressed & bit) {
        heldSince[idx] = lastRepeat[idx] = now;
        return true;
    }
    uint32_t delayMs = bit == A_SOFT ? 0 : DAS_MS;
    if (now - heldSince[idx] >= delayMs && now - lastRepeat[idx] >= interval) {
        lastRepeat[idx] = now;
        return true;
    }
    return false;
}

static void handleKeys()
{
    M5Cardputer.update();
    uint32_t now = millis();
    uint16_t held = readHeld();
    uint16_t pressed = held & ~heldPrev;
    heldPrev = held;

    if (pressed & A_SOUND) {
        soundOn = !soundOn;
        prefs.putBool("sound", soundOn);
        beep(880, 40);
        dirty = true;
    }

    switch (state) {
        case State::Title:
        case State::GameOver:
            if (pressed & A_START) {
                newGame();  // el mismo enter no suelta la pieza: ya cuenta como mantenido
                dirty = true;
            }
            return;
        case State::Paused:
            if (pressed & A_PAUSE) {
                state = State::Playing;
                lastFall = now;
                groundedSince = now;
                dirty = true;
            }
            return;
        case State::Playing:
            break;
    }

    if (pressed & A_PAUSE) {
        state = State::Paused;
        dirty = true;
        return;
    }
    if (pressed & A_HARD) {
        hardDrop();
        dirty = true;
        return;
    }
    if (pressed & A_ROT_CW) { tryRotate(+1); dirty = true; }
    if (pressed & A_ROT_CCW) { tryRotate(-1); dirty = true; }

    // Arriba y abajo a la vez se anulan.
    if (!((held & A_UP) && (held & A_DOWN))) {
        if (repeatFires(0, A_UP, held, pressed, ARR_MS, now)) dirty |= tryMove(1, 0);
        if (repeatFires(1, A_DOWN, held, pressed, ARR_MS, now)) dirty |= tryMove(-1, 0);
    }
    if (repeatFires(2, A_SOFT, held, pressed, SOFT_MS, now)) {
        softDrop();
        dirty = true;
    }
}

static void updateGravity()
{
    uint32_t now = millis();
    Piece below = cur;
    below.y++;
    bool onFloor = !fits(below);

    if (onFloor) {
        if (!grounded) {
            grounded = true;
            groundedSince = now;
        }
        if (now - groundedSince >= LOCK_MS) {
            lockPiece();
            dirty = true;
        }
        return;
    }
    grounded = false;

    if (now - lastFall >= gravityMs) {
        lastFall = now;
        cur.y++;
        dirty = true;
    }
}

static uint16_t shade(uint16_t c, int pct)
{
    int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
    auto f = [pct](int v) { return pct > 100 ? min(255, v + (255 - v) * (pct - 100) / 100) : v * pct / 100; };
    return canvas.color565(f(r), f(g), f(b));
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Speaker.setVolume(120);

    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    C_BG = canvas.color565(10, 12, 20);
    C_GRID = canvas.color565(36, 42, 58);
    C_FRAME = canvas.color565(60, 70, 92);
    C_FLOOR = canvas.color565(64, 200, 220);
    C_HUD = canvas.color565(24, 28, 42);
    C_TEXT = canvas.color565(220, 226, 235);
    C_ACCENT = canvas.color565(64, 200, 220);
    C_FLASH = canvas.color565(255, 255, 255);

    const uint8_t RGB[NUM_PIECES][3] = {
        {40, 210, 230},   // I
        {240, 210, 40},   // O
        {170, 80, 230},   // T
        {80, 210, 90},    // S
        {235, 70, 70},    // Z
        {60, 110, 240},   // J
        {245, 140, 40},   // L
    };
    for (int i = 0; i < NUM_PIECES; i++) {
        C_PIECE[i] = canvas.color565(RGB[i][0], RGB[i][1], RGB[i][2]);
        C_LIGHT[i] = shade(C_PIECE[i], 150);
        C_DARK[i] = shade(C_PIECE[i], 55);
    }

    prefs.begin("tetrix", false);
    highScore = prefs.getUInt("high", 0);
    soundOn = prefs.getBool("sound", true);

    randomSeed(esp_random());
    nextType = drawFromBag();
    level = 1;
    state = State::Title;
    render();
}

void loop()
{
    handleKeys();
    if (state == State::Playing) updateGravity();

    if (dirty) {
        render();
        dirty = false;
    }
    delay(2);
}
