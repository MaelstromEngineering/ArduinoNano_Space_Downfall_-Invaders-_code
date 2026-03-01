#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// I/O
#define POT_PIN A0
#define BUTTON_PIN 2  // D2 (C -> D2, NO -> GND) -> active LOW

// Timing
const uint16_t FRAME_MS = 25; // ~40 FPS

// Game states
enum GameState { MENU, PLAY };
GameState gameState = MENU;

// Player
float playerX = SCREEN_WIDTH / 2.0f;
const uint8_t PLAYER_Y = SCREEN_HEIGHT - 10;
const uint8_t PLAYER_W = 14;  // wider ship
const uint8_t PLAYER_H = 7;
const uint8_t PLAYER_LIVES_START = 3;
uint8_t lives = PLAYER_LIVES_START;

// Bullets
struct Bullet { int16_t x; int16_t y; int8_t vy; bool alive; };
const uint8_t MAX_PLAYER_BULLETS = 4;
const uint8_t MAX_ENEMY_BULLETS = 6;
Bullet playerBullets[MAX_PLAYER_BULLETS];
Bullet enemyBullets[MAX_ENEMY_BULLETS];

// Enemies
const uint8_t ENEMY_COLS = 8;
const uint8_t ENEMY_ROWS = 3;
struct Enemy {
  int8_t x; int8_t y;
  bool alive;
  uint8_t animFrame;
};
Enemy enemies[ENEMY_ROWS][ENEMY_COLS];

// Game state
uint32_t lastFrameMillis = 0;
uint16_t score = 0;
uint8_t level = 1;

// Enemy movement & shooting timing
float enemyBaseX = 8.0f;
float enemyBaseY = 8.0f;
float enemyPhase = 0.0f;
float enemySpeed = 0.6f;
uint32_t lastEnemyShotAttempt = 0;
uint32_t enemyShotInterval = 800; // ms

// Button hold-to-menu
uint32_t buttonPressedMillis = 0;
const uint32_t HOLD_TO_MENU_MS = 5000; // 5 seconds
bool prevButtonState = HIGH; // previous raw digitalRead (HIGH when not pressed)

// Menu blink helpers (fixed safe blink)
uint32_t lastMenuBlinkMs = 0;
bool menuBlinkOn = true;
const uint32_t MENU_BLINK_INTERVAL = 500; // ms

// Utility ------------------------------------------------------------------
inline bool rectCollide(int x1,int y1,int w1,int h1,int x2,int y2,int w2,int h2) {
  return !(x2 > x1 + w1 || x2 + w2 < x1 || y2 > y1 + h1 || y2 + h2 < y1);
}

int mapAnalogToX(int raw) {
  // ESP32 analogRead ~0..4095 (but library can vary). Defensive clamp.
  if (raw < 0) raw = 0;
  if (raw > 4095) raw = 4095;
  float f = raw / 4095.0f;
  float margin = PLAYER_W/2.0f + 2.0f;
  return (int)(margin + f * (SCREEN_WIDTH - 2*margin));
}

// Bullets ------------------------------------------------------------------
void initBullets() {
  for (uint8_t i=0;i<MAX_PLAYER_BULLETS;i++) playerBullets[i].alive = false;
  for (uint8_t i=0;i<MAX_ENEMY_BULLETS;i++) enemyBullets[i].alive = false;
}

void firePlayerBullet(int x, int y) {
  for (uint8_t i=0;i<MAX_PLAYER_BULLETS;i++){
    if (!playerBullets[i].alive) {
      playerBullets[i].x = x;
      playerBullets[i].y = y;
      playerBullets[i].vy = -5; // upward faster for snappy feel
      playerBullets[i].alive = true;
      return;
    }
  }
}

void fireEnemyBullet(int x, int y) {
  for (uint8_t i=0;i<MAX_ENEMY_BULLETS;i++){
    if (!enemyBullets[i].alive) {
      enemyBullets[i].x = x;
      enemyBullets[i].y = y;
      enemyBullets[i].vy = 2 + (level/2);
      enemyBullets[i].alive = true;
      return;
    }
  }
}

// Enemies ------------------------------------------------------------------
uint16_t countAliveEnemies() {
  uint16_t cnt = 0;
  for (uint8_t r=0;r<ENEMY_ROWS;r++) for (uint8_t c=0;c<ENEMY_COLS;c++) if (enemies[r][c].alive) cnt++;
  return cnt;
}

void spawnEnemies(uint8_t lvl) {
  for (uint8_t r=0;r<ENEMY_ROWS;r++){
    for (uint8_t c=0;c<ENEMY_COLS;c++){
      enemies[r][c].alive = true;
      enemies[r][c].animFrame = (r + c) & 1;
      enemies[r][c].x = c * 12;
      enemies[r][c].y = r * 10;
    }
  }
  enemyBaseX = 8.0f;
  enemyBaseY = 8.0f;
  enemyPhase = 0.0f;
  enemySpeed = 0.5f + 0.12f * lvl;
}

// Fixed & safe enemy shooting logic (avoids mixed-type max() issues)
void enemyShootingLogic() {
  if ((uint32_t)(millis() - lastEnemyShotAttempt) < enemyShotInterval) return;
  lastEnemyShotAttempt = millis();
  uint16_t alive = countAliveEnemies();
  if (alive == 0) return;

  int computed = 900 - (int)alive * 20;
  if (computed < 180) computed = 180;
  enemyShotInterval = (uint32_t)computed;

  uint8_t startCol = random(0, ENEMY_COLS);
  for (uint8_t shift=0; shift<ENEMY_COLS; shift++) {
    uint8_t c = (startCol + shift) % ENEMY_COLS;
    for (int r = ENEMY_ROWS - 1; r >= 0; r--) {
      if (enemies[r][c].alive) {
        int ex = (int)(enemyBaseX + enemies[r][c].x + (sin((enemyPhase + (r*0.4f + c*0.2f))) * 4.0f));
        int ey = (int)(enemyBaseY + enemies[r][c].y);
        fireEnemyBullet(ex, ey + 6);
        return;
      }
    }
  }
}

// Input & Player -----------------------------------------------------------
void updateInputAndPlayer() {
  static float smoothX = playerX;
  int raw = analogRead(POT_PIN); // likely 0..4095
  int targetX = mapAnalogToX(raw);
  // smoothing for fluid motion
  smoothX = smoothX * 0.80f + targetX * 0.20f;
  playerX = smoothX;

  // Button handling:
  // - single shot on falling edge (not continuous)
  // - hold for > HOLD_TO_MENU_MS returns to MENU
  bool currPressed = (digitalRead(BUTTON_PIN) == LOW); // active LOW
  uint32_t now = millis();

  // falling edge => fired
  if (currPressed && !prevButtonState) {
    // button just pressed
    buttonPressedMillis = now;
    // Fire a shot immediately on press
    if (gameState == PLAY) {
      firePlayerBullet((int)playerX, PLAYER_Y - 2);
    } else if (gameState == MENU) {
      // start game on press
      level = 1;
      score = 0;
      lives = PLAYER_LIVES_START;
      spawnEnemies(level);
      initBullets();
      gameState = PLAY;
      // small start delay
      delay(120);
    }
  }

  // if still pressed, check hold duration
  if (currPressed && (now - buttonPressedMillis >= HOLD_TO_MENU_MS) && (buttonPressedMillis != 0)) {
    // triggered hold -> return to menu
    if (gameState == PLAY) {
      gameState = MENU;
      // reset button timer so it doesn't re-trigger repeatedly
      buttonPressedMillis = 0;
      prevButtonState = currPressed;
      return;
    }
  }

  // release resets the pressed timestamp
  if (!currPressed) {
    buttonPressedMillis = 0;
  }

  prevButtonState = currPressed;
}

// Updates ------------------------------------------------------------------
void updateBullets() {
  // player bullets
  for (uint8_t i=0;i<MAX_PLAYER_BULLETS;i++){
    if (!playerBullets[i].alive) continue;
    playerBullets[i].y += playerBullets[i].vy;
    if (playerBullets[i].y < -6) { playerBullets[i].alive = false; continue; }

    for (uint8_t r=0;r<ENEMY_ROWS;r++){
      for (uint8_t c=0;c<ENEMY_COLS;c++){
        if (!enemies[r][c].alive) continue;
        int ex = (int)(enemyBaseX + enemies[r][c].x + (sin((enemyPhase + (r*0.4f + c*0.2f))) * 4.0f));
        int ey = (int)(enemyBaseY + enemies[r][c].y);
        if (rectCollide(playerBullets[i].x-1, playerBullets[i].y-1, 2, 2, ex-4, ey-4, 8, 6)) {
          enemies[r][c].alive = false;
          playerBullets[i].alive = false;
          score += 10 + r*5;
          goto NEXT_PLAYER_BULLET;
        }
      }
    }
    NEXT_PLAYER_BULLET: ;
  }

  // enemy bullets
  for (uint8_t i=0;i<MAX_ENEMY_BULLETS;i++){
    if (!enemyBullets[i].alive) continue;
    enemyBullets[i].y += enemyBullets[i].vy;
    if (rectCollide(enemyBullets[i].x-1, enemyBullets[i].y-1, 2, 2, (int)playerX - PLAYER_W/2, PLAYER_Y - PLAYER_H/2, PLAYER_W, PLAYER_H)) {
      enemyBullets[i].alive = false;
      if (lives > 0) lives--;
      delay(60);
    }
    if (enemyBullets[i].y > SCREEN_HEIGHT + 6) enemyBullets[i].alive = false;
  }
}

void updateEnemies() {
  enemyPhase += enemySpeed * 0.02f * level;
  enemyBaseX += sin(enemyPhase*0.5f) * 0.03f;

  static uint32_t lastPush = 0;
  uint32_t pushInterval = max(4000u, 12000u - (uint32_t)level*900u);
  if ((uint32_t)(millis() - lastPush) > pushInterval) {
    lastPush = millis();
    enemyBaseY += 6;
  }

  for (uint8_t r=0;r<ENEMY_ROWS;r++){
    for (uint8_t c=0;c<ENEMY_COLS;c++){
      if (!enemies[r][c].alive) continue;
      int ey = (int)(enemyBaseY + enemies[r][c].y);
      if (ey + 6 >= PLAYER_Y - 8) {
        if (lives > 0) lives--;
        spawnEnemies(level);
        initBullets();
        delay(200);
        return;
      }
    }
  }
}

// Drawing ------------------------------------------------------------------
void drawShipDetailed(int cx, int cy) {
  int x = cx;
  int y = cy;
  // body (rounded-ish rectangle)
  display.fillRect(x - 6, y - 4, 12, 6, SSD1306_WHITE);
  // cockpit dome
  display.fillRect(x - 2, y - 6, 4, 2, SSD1306_WHITE);
  // left wing
  display.fillTriangle(x - 7, y - 1, x - 10, y + 3, x - 4, y + 2, SSD1306_WHITE);
  // right wing
  display.fillTriangle(x + 7, y - 1, x + 10, y + 3, x + 4, y + 2, SSD1306_WHITE);
  // thruster "glow" small rect below
  display.fillRect(x - 2, y + 3, 4, 2, SSD1306_WHITE);
  // small cockpit window (inverted pixel)
  display.drawPixel(x - 1, y - 5, SSD1306_BLACK);
  display.drawPixel(x,     y - 5, SSD1306_BLACK);
  display.drawPixel(x + 1, y - 5, SSD1306_BLACK);
}

void drawPlayer() {
  int px = (int)playerX;
  // use top-center as reference (PLAYER_Y - 2)
  drawShipDetailed(px, PLAYER_Y - 2);
}

void drawEnemies() {
  for (uint8_t r=0;r<ENEMY_ROWS;r++){
    for (uint8_t c=0;c<ENEMY_COLS;c++){
      if (!enemies[r][c].alive) continue;
      int ex = (int)(enemyBaseX + enemies[r][c].x + (sin((enemyPhase + (r*0.4f + c*0.2f))) * 4.0f));
      int ey = (int)(enemyBaseY + enemies[r][c].y);
      if (r == 0) {
        display.drawRect(ex-4, ey-3, 9, 6, SSD1306_WHITE);
        display.fillRect(ex-2, ey-1, 5, 2, SSD1306_WHITE);
      } else if (r == 1) {
        display.fillRect(ex-3, ey-2, 7, 4, SSD1306_WHITE);
        display.drawPixel(ex, ey-3, SSD1306_WHITE);
      } else {
        display.drawRect(ex-3, ey-3, 7, 5, SSD1306_WHITE);
        display.drawPixel(ex-2, ey+1, SSD1306_WHITE);
        display.drawPixel(ex+2, ey+1, SSD1306_WHITE);
      }
    }
  }
}

void drawBullets() {
  for (uint8_t i=0;i<MAX_PLAYER_BULLETS;i++){
    if (!playerBullets[i].alive) continue;
    // small tapered bullet (3x4)
    display.fillRect(playerBullets[i].x-1, playerBullets[i].y-2, 3, 4, SSD1306_WHITE);
    display.drawPixel(playerBullets[i].x, playerBullets[i].y-3, SSD1306_WHITE);
  }
  for (uint8_t i=0;i<MAX_ENEMY_BULLETS;i++){
    if (!enemyBullets[i].alive) continue;
    display.drawRect(enemyBullets[i].x-1, enemyBullets[i].y-1, 2, 3, SSD1306_WHITE);
  }
}

void drawHUD() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.print("S:");
  display.print(score);
  display.setCursor(SCREEN_WIDTH - 44, 0);
  display.print("L:");
  display.print(lives);
  display.setCursor(SCREEN_WIDTH/2 - 14, 0);
  display.print("LV");
  display.print(level);
}

// Setup & main loop -------------------------------------------------------
void setup() {
  Wire.begin(); // uses A4/A5 by default on Nano ESP32
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(10);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 not found");
    while(true);
  }
  display.clearDisplay();
  display.display();

  randomSeed(analogRead(A0));

  spawnEnemies(level);
  initBullets();

  // Show menu initially
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10,8);
  display.println("SPACE DOWNFALL");
  display.setCursor(6,26);
  display.println("Press switch to start");
  display.setCursor(6,40);
  display.println("Hold switch >5s => menu");
  display.display();

  lastFrameMillis = millis();
  prevButtonState = digitalRead(BUTTON_PIN);
}

void showMenuSplash() {
  // static title; actual "Press" blink is controlled in loop() to avoid full-screen invert
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10,8);
  display.println("SPACE DOWNFALL");
  display.setCursor(6,40);
  display.println("Hold switch >5s => menu");
  display.display();
}

void loop() {
  uint32_t now = millis();
  if ((uint32_t)(now - lastFrameMillis) < FRAME_MS) {
    delay(1);
    return;
  }
  lastFrameMillis = now;

  // Poll input & handle menu/game start/hold
  updateInputAndPlayer();

  if (gameState == PLAY) {
    // Updates
    updateEnemies();
    updateBullets();
    enemyShootingLogic();

    // Level cleared?
    if (countAliveEnemies() == 0) {
      level++;
      spawnEnemies(level);
      initBullets();
    }

    // Rendering
    display.clearDisplay();

    // simple starfield
    static uint8_t starPhase = 0;
    starPhase++;
    for (uint8_t i=0;i<10;i++){
      uint8_t sx = (i*23 + (starPhase + level*3)) % SCREEN_WIDTH;
      uint8_t sy = (i*7 + (starPhase*2)) % (SCREEN_HEIGHT-8);
      display.drawPixel(sx, sy + 6, SSD1306_WHITE);
    }

    drawEnemies();
    drawBullets();
    drawPlayer();
    drawHUD();

    display.display();
  } else {
    // MENU state - blink only the "Press switch to start" text (avoid full-screen invert)
    if (now - lastMenuBlinkMs >= MENU_BLINK_INTERVAL) {
      lastMenuBlinkMs = now;
      menuBlinkOn = !menuBlinkOn;
    }

    // draw base menu
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10,8);
    display.println("SPACE DOWNFALL");
    display.setCursor(6,40);
    display.println("Hold switch >5s => menu");

    // blinking line (only shows when menuBlinkOn==true)
    if (menuBlinkOn) {
      display.setCursor(6,26);
      display.println("Press switch to start");
    }
    display.display();
  }
}