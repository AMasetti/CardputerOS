// Snake para M5Stack Cardputer / Cardputer-Adv
//
// Teclas (las flechas impresas en el teclado de la Cardputer):
//   ;  .  ,  /   arriba / abajo / izquierda / derecha   (también w s a d)
//   enter        empezar / reintentar
//   space o p    pausa
//   m            sonido on/off
//
// El récord se guarda en la flash (NVS), así que sobrevive al apagado.

#include <M5Cardputer.h>
#include <Preferences.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// --- Tablero ---------------------------------------------------------------
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HUD_H = 15;
static constexpr int CELL = 6;
static constexpr int COLS = SCREEN_W / CELL;              // 40
static constexpr int ROWS = (SCREEN_H - HUD_H) / CELL;    // 20
static constexpr int MAX_LEN = COLS * ROWS;

// --- Velocidad: arranca en 140 ms por paso y baja hasta 55 ms --------------
static constexpr uint32_t STEP_START_MS = 140;
static constexpr uint32_t STEP_MIN_MS = 55;
static constexpr uint32_t STEP_DEC_MS = 4;  // por cada comida

enum class State { Title, Playing, Paused, GameOver };
enum Dir : uint8_t { UP, DOWN, LEFT, RIGHT };

struct Cell {
    int8_t x, y;
};

static M5Canvas canvas(&M5Cardputer.Display);
static Preferences prefs;

static Cell snake[MAX_LEN];  // snake[0] es la cabeza
static int snakeLen;
static Cell food;
static Dir dir;
static Dir pending[2];  // buffer de giros para no perder teclas rápidas
static int pendingCount;
static uint32_t stepMs;
static uint32_t lastStep;
static int score;
static int highScore;
static bool newRecord;
static bool soundOn = true;
static State state = State::Title;
static bool dirty = true;  // hay que redibujar

// Colores (RGB565)
static uint16_t C_BG, C_GRID, C_HEAD, C_BODY, C_FOOD, C_HUD, C_TEXT, C_ACCENT;

static void beep(float freq, uint32_t ms)
{
    if (soundOn) M5Cardputer.Speaker.tone(freq, ms);
}

static bool onSnake(int x, int y, int len = -1)
{
    if (len < 0) len = snakeLen;
    for (int i = 0; i < len; i++) {
        if (snake[i].x == x && snake[i].y == y) return true;
    }
    return false;
}

static void placeFood()
{
    // Con el tablero casi lleno, buscar al azar puede tardar: se recorre.
    if (snakeLen >= MAX_LEN) return;
    int start = random(MAX_LEN);
    for (int k = 0; k < MAX_LEN; k++) {
        int idx = (start + k) % MAX_LEN;
        int x = idx % COLS, y = idx / COLS;
        if (!onSnake(x, y)) {
            food = {(int8_t)x, (int8_t)y};
            return;
        }
    }
}

static void newGame()
{
    snakeLen = 4;
    for (int i = 0; i < snakeLen; i++) {
        snake[i] = {(int8_t)(COLS / 2 - i), (int8_t)(ROWS / 2)};
    }
    dir = RIGHT;
    pendingCount = 0;
    stepMs = STEP_START_MS;
    score = 0;
    newRecord = false;
    placeFood();
    lastStep = millis();
    state = State::Playing;
}

static bool opposite(Dir a, Dir b)
{
    return (a == UP && b == DOWN) || (a == DOWN && b == UP) || (a == LEFT && b == RIGHT) ||
           (a == RIGHT && b == LEFT);
}

static void queueTurn(Dir d)
{
    Dir last = pendingCount ? pending[pendingCount - 1] : dir;
    if (d == last || opposite(d, last) || pendingCount >= 2) return;
    pending[pendingCount++] = d;
}

static void gameOver()
{
    state = State::GameOver;
    if (score > highScore) {
        highScore = score;
        newRecord = true;
        prefs.putInt("high", highScore);
    }
    beep(220, 120);
    delay(130);
    beep(110, 250);
}

static void step()
{
    if (pendingCount) {
        dir = pending[0];
        pending[0] = pending[1];
        pendingCount--;
    }

    Cell head = snake[0];
    switch (dir) {
        case UP: head.y--; break;
        case DOWN: head.y++; break;
        case LEFT: head.x--; break;
        case RIGHT: head.x++; break;
    }

    if (head.x < 0 || head.x >= COLS || head.y < 0 || head.y >= ROWS) {
        gameOver();
        return;
    }

    bool eats = head.x == food.x && head.y == food.y;
    // Si no come, la cola se corre en este mismo paso: esa celda queda libre.
    if (onSnake(head.x, head.y, eats ? snakeLen : snakeLen - 1)) {
        gameOver();
        return;
    }

    if (eats && snakeLen < MAX_LEN) snakeLen++;
    for (int i = snakeLen - 1; i > 0; i--) snake[i] = snake[i - 1];
    snake[0] = head;

    if (eats) {
        score++;
        if (stepMs > STEP_MIN_MS) stepMs = max(STEP_MIN_MS, stepMs - STEP_DEC_MS);
        beep(1400, 30);
        placeFood();
    }
}

// --- Dibujo ------------------------------------------------------------------
static void drawHud()
{
    canvas.fillRect(0, 0, SCREEN_W, HUD_H, C_HUD);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextColor(C_TEXT, C_HUD);
    canvas.setTextDatum(middle_left);
    canvas.drawString("SNAKE", 4, HUD_H / 2);
    canvas.setTextDatum(middle_center);
    canvas.drawString(String("Puntos ") + score, SCREEN_W / 2, HUD_H / 2);
    canvas.setTextDatum(middle_right);
    canvas.drawString(String("Record ") + highScore, SCREEN_W - 4, HUD_H / 2);
}

static void drawBoard()
{
    canvas.fillRect(0, HUD_H, SCREEN_W, SCREEN_H - HUD_H, C_BG);
    // Puntos de grilla tenues para ubicarse
    for (int y = 0; y < ROWS; y += 2) {
        for (int x = 0; x < COLS; x += 2) {
            canvas.drawPixel(x * CELL + CELL / 2, HUD_H + y * CELL + CELL / 2, C_GRID);
        }
    }
    // Comida
    canvas.fillRoundRect(food.x * CELL, HUD_H + food.y * CELL, CELL, CELL, 2, C_FOOD);
    // Víbora
    for (int i = snakeLen - 1; i >= 0; i--) {
        uint16_t c = i == 0 ? C_HEAD : C_BODY;
        canvas.fillRect(snake[i].x * CELL, HUD_H + snake[i].y * CELL, CELL - 1, CELL - 1, c);
    }
}

static void drawCenteredBox(const char *title, const char *line1, const char *line2)
{
    const int w = 180, h = 70;
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
    canvas.drawString(line2, SCREEN_W / 2, y + 50);
}

static void render()
{
    drawHud();
    drawBoard();
    switch (state) {
        case State::Title:
            drawCenteredBox("SNAKE", "enter jugar  ;.,/ mover", "space pausa  m sonido");
            break;
        case State::Paused:
            drawCenteredBox("PAUSA", "space seguir", soundOn ? "m sonido: on" : "m sonido: off");
            break;
        case State::GameOver:
            drawCenteredBox(newRecord ? "NUEVO RECORD!" : "GAME OVER", (String("Puntos: ") + score).c_str(),
                            "enter otra vez");
            break;
        default:
            break;
    }
    canvas.pushSprite(0, 0);
}

// --- Entrada -----------------------------------------------------------------
static void handleKeys()
{
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    dirty = true;

    auto ks = M5Cardputer.Keyboard.keysState();

    if (ks.enter) {
        if (state != State::Playing && state != State::Paused) newGame();
        return;
    }

    for (char c : ks.word) {
        switch (c) {
            case ';': case 'w': case 'W': queueTurn(UP); break;
            case '.': case 's': case 'S': queueTurn(DOWN); break;
            case ',': case 'a': case 'A': queueTurn(LEFT); break;
            case '/': case 'd': case 'D': queueTurn(RIGHT); break;
            case ' ': case 'p': case 'P':
                if (state == State::Playing) state = State::Paused;
                else if (state == State::Paused) { state = State::Playing; lastStep = millis(); }
                break;
            case 'm': case 'M':
                soundOn = !soundOn;
                prefs.putBool("sound", soundOn);
                beep(880, 40);
                break;
        }
    }
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Speaker.setVolume(120);

    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    C_BG = canvas.color565(10, 14, 20);
    C_GRID = canvas.color565(32, 40, 52);
    C_HEAD = canvas.color565(150, 255, 120);
    C_BODY = canvas.color565(60, 190, 90);
    C_FOOD = canvas.color565(236, 64, 140);
    C_HUD = canvas.color565(24, 30, 42);
    C_TEXT = canvas.color565(220, 226, 235);
    C_ACCENT = canvas.color565(64, 200, 220);

    prefs.begin("snake", false);
    highScore = prefs.getInt("high", 0);
    soundOn = prefs.getBool("sound", true);

    randomSeed(esp_random());
    newGame();
    state = State::Title;
    render();
}

void loop()
{
    handleKeys();

    if (state == State::Playing && millis() - lastStep >= stepMs) {
        lastStep = millis();
        step();
        dirty = true;
    }

    if (dirty) {
        render();
        dirty = false;
    }
    delay(2);
}
