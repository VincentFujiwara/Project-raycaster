#include "raylib.h"
#define MMNOSOUND
#define PlaySoundA PlaySoundAWin32
#define PlaySoundW PlaySoundWWin32
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <omp.h> // Для многопоточности

int cpu_threads = 4;

// ------------------------------------------------------------
//  ОСНОВНЫЕ КОНСТАНТЫ
// ------------------------------------------------------------

#define MAX_PITCH 1.0f  
#define SW 1080
#define SH 720
#define MW 192
#define MH 192
#define TS 32
#define SKY_W 10000
#define SKY_H 5000

// Высоты (Потолки подняты, чтобы не застревать на ящиках)

#define Z_FLOOR      0.0f
#define Z_STEP       0.125f
#define Z_CRATE      2.5f
#define Z_CEIL       2.5f
#define Z_LOWCEIL    2.5f
#define Z_HIGHCEIL   2.5f

// Игрок
#define PLAYER_RADIUS 0.4f
#define EYE_HEIGHT    0.55f
#define PLAYER_HEIGHT 1.0f

// Физика
#define GRAVITY       -12.0f
#define JUMP_SPEED    3.0f
#define STEP_UP       0.15f

// Аудио (только для старого синтеза, если понадобится)
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BPM 150
#define AUDIO_TICK_RATE (AUDIO_SAMPLE_RATE * 60 / (AUDIO_BPM * 4))

// ------------------------------------------------------------
//  ТИПЫ КЛЕТОК И СТРУКТУРЫ
// ------------------------------------------------------------

typedef enum {
	CELL_EMPTY = 0,
	CELL_WALL,
	CELL_DOOR,
	CELL_CRATE,
	CELL_LOW_WALL
} CellType;

typedef struct {
	uint8_t  type;
	uint8_t  wallTex;
	uint8_t  floorTex;
	uint8_t  ceilTex;
	float    floorZ;
	float    ceilZ;
} Cell;

// --- ОРУЖИЕ И ГИЛЬЗЫ ---
#define MAX_SHELLS 1000
#define MAX_BULLETS 1000

typedef struct {
	float x, y, z;
	float normalX, normalY;
	int texID;
	bool active;
} BulletHole;

BulletHole bulletHoles[MAX_BULLETS];

typedef struct {
	float x, y, z;
	float vx, vy, vz;
	float life;
	bool active;
} Shell;

Shell shells[MAX_SHELLS];

typedef struct {
	int ammo;
	int maxAmmo;
	float fireRate;
	float lastShotTime;
	float recoil;
	float reloadTime;
	float reloadTimer;
	bool isReloading;
	
	Texture2D texIdle;
	Texture2D texFire1;
	Texture2D texFire2;
	Texture2D texFire3;
	Texture2D texReload;
	
	// --- ЗВУКИ ---
	Sound sndShot;
	Sound sndReloadStart;
	Sound sndReloadEnd;
	Sound sndEmpty;
	
	int currentFrame;
	float animTimer;
} Weapon;

Weapon pistol;

// ------------------------------------------------------------
//  СИСТЕМА СОСТОЯНИЙ И МЕНЮ
// ------------------------------------------------------------
typedef enum {
	STATE_MENU,
	STATE_TRANSITION,
	STATE_PLAYING,
} GameState;

GameState currentState = STATE_MENU;
int menuSelection = 0; 
float transitionAlpha = 0.0f; 
bool fadeToWhite = true;

	// --- ГЛОБАЛЬНЫЕ ЗВУКИ И ТАЙМЕРЫ ---
	Sound sndShellFloor;
	Sound sndWallImpactBeton;
	Sound sndWallImpactDerevo;
	Sound sndWallImpactZhelezo;
	Sound sndWallImpactPesok;
	Sound sndFootstep;
	Sound sndStaminaOut;
	Sound sndStaminaBack;

	float footstepTimer = 0.15f;
	float shellSndCd = 0.0f;
	bool isFullyExhausted = false;

typedef struct {
	float x, y, z;
	float vx, vy, vz;
	float pa, pitch;
	bool  onGround;
	float bobPhase, bobAmount;
	float stamina;
	float maxStamina;
	float aimSpreadX;
	float aimSpreadY;
	float crosshairX;
	float crosshairY;	
} Player;

// ------------------------------------------------------------
//  ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ------------------------------------------------------------

Cell map[MW][MH];
Player player;

float fov       = 60.0f * (PI / 180.0f);
float depth     = 64.0f;
bool showMap = true;

// Буферы для двухпроходного рендера (ВЫРАВНЕНЫ ПО 64 БАЙТАМ ДЛЯ OPENMP)
__attribute__((aligned(64))) int rawStartBuf[SW], rawEndBuf[SW];
__attribute__((aligned(64))) int   drawStartBuf[SW];
__attribute__((aligned(64))) int   drawEndBuf[SW];
__attribute__((aligned(64))) int   texIDBuf[SW];
__attribute__((aligned(64))) int   txBuf[SW];
__attribute__((aligned(64))) int   sideBuf[SW];
__attribute__((aligned(64))) float perpBuf[SW];
__attribute__((aligned(64))) float lightBuf[SW];
__attribute__((aligned(64))) int   skyXBuf[SW];
__attribute__((aligned(64))) int   topZBuf[SW];    
__attribute__((aligned(64))) int   botZBuf[SW];    
__attribute__((aligned(64))) int   shadeFloorBuf[SH];

float zbuf[SW];

int   drawEndBuf[SW];
int   texIDBuf[SW];
int   txBuf[SW];
int   sideBuf[SW];
float perpBuf[SW];
float lightBuf[SW];
int   skyXBuf[SW];
int   topZBuf[SW];    
int   botZBuf[SW];    
int   shadeFloorBuf[SH];

// Текстуры
#define N_WALL_TEX  8
#define N_FLOOR_TEX 8
#define N_CEIL_TEX  4

Color wallTex[N_WALL_TEX][TS][TS];
Color floorTex[N_FLOOR_TEX][TS][TS];
Color ceilTex[N_CEIL_TEX][TS][TS];
Color skyTex[SKY_H][SKY_W];

// ------------------------------------------------------------
//  СЕКТОР 2: МАТЕМАТИКА ДЛЯ ТЕКСТУР
// ------------------------------------------------------------

// Хеш-функция для псевдослучайных чисел без внешнего состояния
static inline unsigned int hash2(unsigned int x, unsigned int y) {
	unsigned int h = x * 374761393u + y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

// Плавный шум для деталей
static inline float noise2(float x, float y) {
	int xi = (int)floorf(x);
	int yi = (int)floorf(y);
	float xf = x - xi;
	float yf = y - yi;
	float u = xf * xf * (3.0f - 2.0f * xf);
	float v = yf * yf * (3.0f - 2.0f * yf);
	
	float a = (float)(hash2(xi,     yi    ) & 0xFFFF) / 65535.0f;
	float b = (float)(hash2(xi + 1, yi    ) & 0xFFFF) / 65535.0f;
	float c = (float)(hash2(xi,     yi + 1) & 0xFFFF) / 65535.0f;
	float d = (float)(hash2(xi + 1, yi + 1) & 0xFFFF) / 65535.0f;
	
	return a * (1 - u) * (1 - v) + b * u * (1 - v) + c * (1 - u) * v + d * u * v;
}

// Фрактальный шум (несколько октав) для органичных текстур
static float fbm(float x, float y, int octaves) {
	float sum = 0.0f, amp = 0.5f, freq = 1.0f;
	for (int i = 0; i < octaves; i++) {
		sum += noise2(x * freq, y * freq) * amp;
		freq *= 2.0f;
		amp  *= 0.5f;
	}
	return sum;
}

// Смешать два цвета (для плавных переходов)
static inline Color MixColor(Color a, Color b, float t) {
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return (Color){
		(unsigned char)(a.r + (b.r - a.r) * t),
		(unsigned char)(a.g + (b.g - a.g) * t),
		(unsigned char)(a.b + (b.b - a.b) * t),
		255
	};
}

// ------------------------------------------------------------
//  СЕКТОР 3: ТЕКСТУРЫ СТЕН
// ------------------------------------------------------------

// Текстура 0: Песочный кирпич
static void GenerateWall_SandBrick(Color tex[TS][TS]) {
	Color brickA   = (Color){205, 175, 130, 255};
	Color brickB   = (Color){190, 158, 112, 255};
	
	int brickH = 8;
	int brickW = 16;
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			int row = y / brickH;
			int offset = (row % 2) * (brickW / 2);
			int bx = (x + offset) % brickW;
			int by = y % brickH;
			
			bool isMortar = (bx == 0 || by == 0);
			
			if (isMortar) {
				float n = fbm(x * 0.3f, y * 0.3f, 2);
				unsigned char v = (unsigned char)(150 + n * 30);
				tex[y][x] = (Color){v, (unsigned char)(v * 0.82f), (unsigned char)(v * 0.58f), 255};
			} else {
				int variant = ((bx * 7 + by * 13 + row * 17) % 5);
				Color base = (variant < 3) ? brickA : brickB;
				float n = fbm(x * 0.4f + row * 5.0f, y * 0.4f, 2);
				float shade = 0.85f + n * 0.3f;
				
				if (by == 1) shade *= 0.92f;
				if (bx == 1) shade *= 0.95f;
				if (by == brickH - 1) shade *= 1.05f;
				
				tex[y][x] = (Color){
					(unsigned char)(base.r * shade),
					(unsigned char)(base.g * shade),
					(unsigned char)(base.b * shade),
					255
				};
			}
		}
	}
}

// Текстура 1: Облупленная штукатурка
static void GenerateWall_Stucco(Color tex[TS][TS]) {
	Color base = (Color){196, 168, 122, 255};
	Color dark = (Color){140, 115, 78, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.25f, y * 0.25f, 3);
			float cracks = fbm(x * 0.8f, y * 0.8f, 2);
			
			Color c = MixColor(dark, base, n);
			
			if (cracks > 0.72f) {
				float t = (cracks - 0.72f) / 0.28f;
				c = MixColor(c, (Color){60, 45, 30, 255}, t * 0.9f);
			}
			
			if (n > 0.75f) {
				c = MixColor(c, (Color){220, 200, 160, 255}, (n - 0.75f) * 1.5f);
			}
			
			tex[y][x] = c;
		}
	}
}

// Текстура 2: Каменная кладка (туннели)
static void GenerateWall_Stone(Color tex[TS][TS]) {
	Color mortar = (Color){88, 78, 62, 255};
	Color stoneA = (Color){130, 118, 96, 255};
	Color stoneB = (Color){110, 98, 80, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			int row = y / 8;
			int offset = (row % 2) * 8;
			int bx = (x + offset) % 16;
			int by = y % 8;
			
			bool isMortar = (bx < 1 || by < 1);
			
			if (isMortar) {
				tex[y][x] = mortar;
			} else {
				int variant = (hash2(x, y + row * 17) & 3);
				Color base = (variant < 2) ? stoneA : stoneB;
				float n = fbm(x * 0.5f + row * 3.0f, y * 0.5f, 2);
				float shade = 0.85f + n * 0.3f;
				
				tex[y][x] = (Color){
					(unsigned char)(base.r * shade),
					(unsigned char)(base.g * shade),
					(unsigned char)(base.b * shade),
					255
				};
			}
		}
	}
}

// ------------------------------------------------------------
//  СЕКТОР 4: ТЕКСТУРЫ СТЕН
// ------------------------------------------------------------

// Текстура 3: Деревянный ящик
static void GenerateWall_WoodCrate(Color tex[TS][TS]) {
	Color plankLight = (Color){168, 122, 72, 255};
	Color plankDark  = (Color){120, 82, 45, 255};
	Color edgeDark   = (Color){70, 45, 22, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			bool isEdge = (x < 2 || x >= TS - 2 || y < 2 || y >= TS - 2);
			bool isBrace = (abs((x - TS / 2) - (y - TS / 2)) < 2) ||
			(abs((x - TS / 2) + (y - TS / 2) - TS / 2) < 2);
			
			if (isEdge) {
				tex[y][x] = edgeDark;
			} else if (isBrace) {
				float n = fbm(x * 0.4f, y * 0.4f, 2);
				Color c = MixColor(plankDark, edgeDark, 0.4f);
				c.r = (unsigned char)(c.r * (0.85f + n * 0.3f));
				c.g = (unsigned char)(c.g * (0.85f + n * 0.3f));
				c.b = (unsigned char)(c.b * (0.85f + n * 0.3f));
				tex[y][x] = c;
			} else {
				int plank = y / 8;
				Color base = (plank % 2 == 0) ? plankLight : plankDark;
				float grain = sinf((float)x * 0.6f + fbm(x * 0.3f, y * 0.5f, 2) * 6.0f) * 0.5f + 0.5f;
				float shade = 0.8f + grain * 0.4f;
				
				if (x >= 4 && x <= 6 && y >= 4 && y <= 6) shade *= 0.5f;
				if (x >= TS-7 && x <= TS-5 && y >= 4 && y <= 6) shade *= 0.5f;
				if (x >= 4 && x <= 6 && y >= TS-7 && y <= TS-5) shade *= 0.5f;
				if (x >= TS-7 && x <= TS-5 && y >= TS-7 && y <= TS-5) shade *= 0.5f;
				
				tex[y][x] = (Color){
					(unsigned char)(base.r * shade),
					(unsigned char)(base.g * shade),
					(unsigned char)(base.b * shade),
					255
				};
			}
		}
	}
}

// Текстура 4: Металлическая дверь
static void GenerateWall_MetalDoor(Color tex[TS][TS]) {
	Color metalLight = (Color){130, 130, 135, 255};
	Color metalDark  = (Color){75, 75, 80, 255};
	Color rust       = (Color){130, 75, 40, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			bool isFrame = (x < 2 || x >= TS - 2 || y < 2 || y >= TS - 2);
			
			if (isFrame) {
				tex[y][x] = metalDark;
			} else {
				float band = ((x / 4) % 2 == 0) ? 1.05f : 0.9f;
				float n = fbm(x * 0.6f, y * 0.6f, 2);
				float shade = band * (0.8f + n * 0.3f);
				Color c = MixColor(metalDark, metalLight, n);
				
				float rustNoise = fbm(x * 0.2f + 10.0f, y * 0.2f + 10.0f, 3);
				if (rustNoise > 0.65f) {
					c = MixColor(c, rust, (rustNoise - 0.65f) * 1.8f);
				}
				
				if (x >= TS/2 - 3 && x <= TS/2 + 3 && y >= TS/2 - 2 && y <= TS/2 + 2) {
					c = (Color){50, 50, 55, 255};
				}
				
				tex[y][x] = (Color){
					(unsigned char)(c.r * shade),
					(unsigned char)(c.g * shade),
					(unsigned char)(c.b * shade),
					255
				};
			}
		}
	}
}

// Текстура 5: Металлическая бочка
static void GenerateWall_Barrel(Color tex[TS][TS]) {
	Color base = (Color){90, 110, 130, 255};
	Color highlight = (Color){150, 170, 190, 255};
	Color rust = (Color){140, 80, 35, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float dx = (float)x - TS / 2.0f;
			float dv = 1.0f - fabsf(dx) / (TS / 2.0f);
			dv = dv * dv;
			
			Color c = MixColor(base, highlight, dv * 0.7f);
			
			if (y % 8 == 0 || y % 8 == 1) {
				c = MixColor(c, (Color){40, 40, 45, 255}, 0.7f);
			}
			
			float n = fbm(x * 0.35f + 20.0f, y * 0.35f + 20.0f, 3);
			if (n > 0.6f) {
				c = MixColor(c, rust, (n - 0.6f) * 1.5f);
			}
			
			float metalNoise = (hash2(x, y) & 0xFF) / 255.0f;
			c.r = (unsigned char)(c.r * (0.92f + metalNoise * 0.15f));
			c.g = (unsigned char)(c.g * (0.92f + metalNoise * 0.15f));
			c.b = (unsigned char)(c.b * (0.92f + metalNoise * 0.15f));
			
			tex[y][x] = c;
		}
	}
}

// Текстура 6: Тёмная штукатурка (туннели)
static void GenerateWall_DarkStucco(Color tex[TS][TS]) {
	Color base = (Color){120, 100, 72, 255};
	Color dark = (Color){75, 60, 42, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.3f + 5.0f, y * 0.3f + 5.0f, 3);
			Color c = MixColor(dark, base, n);
			
			float dirt = fbm(x * 0.15f, y * 0.15f, 2);
			if (dirt > 0.7f) {
				c = MixColor(c, (Color){50, 40, 28, 255}, (dirt - 0.7f) * 2.0f);
			}
			
			tex[y][x] = c;
		}
	}
}

// Текстура 7: Кирпич с меткой (акценты)
static void GenerateWall_BrickMarked(Color tex[TS][TS]) {
	GenerateWall_SandBrick(tex);
	
	for (int y = 10; y < 22; y++) {
		for (int x = 8; x < 24; x++) {
			if ((x + y) % 3 == 0 && x > 10 && x < 22 && y > 12 && y < 20) {
				tex[y][x] = (Color){140, 45, 35, 255};
			}
		}
	}
}

// ------------------------------------------------------------
//  СЕКТОР 5: ТЕКСТУРЫ ПОЛОВ
// ------------------------------------------------------------

// Пол 0: Песок (улицы)
static void GenerateFloor_Sand(Color tex[TS][TS]) {
	Color sandLight = (Color){200, 175, 130, 255};
	Color sandDark  = (Color){160, 135, 95, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.4f, y * 0.4f, 3);
			Color c = MixColor(sandDark, sandLight, n);
			
			if ((hash2(x, y) & 0x7F) < 3) {
				c = MixColor(c, (Color){120, 100, 70, 255}, 0.6f);
			}
			
			tex[y][x] = c;
		}
	}
}

// Пол 1: Песчаная плитка (A site, B site)
static void GenerateFloor_SandTile(Color tex[TS][TS]) {
	Color tileLight = (Color){210, 185, 140, 255};
	Color tileDark  = (Color){170, 145, 105, 255};
	Color grout     = (Color){110, 90, 65, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			bool isGrout = (x % 16 == 0 || y % 16 == 0);
			
			if (isGrout) {
				tex[y][x] = grout;
			} else {
				int tileX = x / 16;
				int tileY = y / 16;
				float variance = ((hash2(tileX, tileY) & 0xFF) / 255.0f - 0.5f) * 0.2f;
				
				float n = fbm(x * 0.5f, y * 0.5f, 2);
				Color c = MixColor(tileDark, tileLight, n + variance);
				
				tex[y][x] = c;
			}
		}
	}
}

// Пол 2: Каменная плитка (туннели)
static void GenerateFloor_StoneTile(Color tex[TS][TS]) {
	Color stone = (Color){95, 85, 70, 255};
	Color stoneLight = (Color){125, 112, 92, 255};
	Color grout = (Color){50, 45, 38, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			bool isGrout = (x % 8 == 0 || y % 8 == 0);
			
			if (isGrout) {
				tex[y][x] = grout;
			} else {
				int tx = x / 8;
				int ty = y / 8;
				float v = ((hash2(tx + 100, ty + 100) & 0xFF) / 255.0f);
				float n = fbm(x * 0.6f, y * 0.6f, 2);
				Color c = MixColor(stone, stoneLight, n * 0.5f + v * 0.5f);
				
				if (n < 0.3f) c = MixColor(c, (Color){60, 50, 38, 255}, 0.5f);
				
				tex[y][x] = c;
			}
		}
	}
}

// Пол 3: Гравий / щебёнка (дороги)
static void GenerateFloor_Gravel(Color tex[TS][TS]) {
	Color base = (Color){140, 120, 90, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.7f, y * 0.7f, 3);
			Color c = base;
			
			if ((hash2(x, y) & 0x3F) < 6) {
				unsigned int h = hash2(x, y);
				int shade = (h >> 8) & 0x3F;
				c = (Color){
					(unsigned char)(100 + shade),
					(unsigned char)(90 + shade),
					(unsigned char)(75 + shade),
					255
				};
			} else {
				c = MixColor(base, (Color){180, 160, 125, 255}, n);
			}
			
			tex[y][x] = c;
		}
	}
}

// ------------------------------------------------------------
//  СЕКТОР 6: ТЕКСТУРЫ ПОЛОВ
// ------------------------------------------------------------

// Пол 4: Деревянный пол (внутри зданий)
static void GenerateFloor_Wood(Color tex[TS][TS]) {
	Color wood = (Color){145, 100, 60, 255};
	Color woodDark = (Color){100, 68, 40, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			int plank = y / 4;
			Color base = (plank % 2 == 0) ? wood : woodDark;
			
			float grain = sinf(x * 0.8f + fbm(x * 0.4f, y * 0.6f, 2) * 5.0f) * 0.5f + 0.5f;
			float shade = 0.85f + grain * 0.3f;
			
			if (y % 4 == 0) shade *= 0.5f;
			
			tex[y][x] = (Color){
				(unsigned char)(base.r * shade),
				(unsigned char)(base.g * shade),
				(unsigned char)(base.b * shade),
				255
			};
		}
	}
}

// Пол 5: Песок с камнями
static void GenerateFloor_SandyStones(Color tex[TS][TS]) {
	GenerateFloor_Sand(tex);
	
	for (int i = 0; i < 20; i++) {
		unsigned int h = hash2(i * 17, i * 31);
		int cx = (h >> 0) & 0x1F;
		int cy = (h >> 8) & 0x1F;
		int r  = 1 + ((h >> 16) & 1);
		
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				int x = cx + dx;
				int y = cy + dy;
				if (x >= 0 && x < TS && y >= 0 && y < TS) {
					if (dx * dx + dy * dy <= r * r) {
						unsigned char v = (unsigned char)(90 + ((h >> 20) & 0x3F));
						tex[y][x] = (Color){v, (unsigned char)(v * 0.9f), (unsigned char)(v * 0.75f), 255};
					}
				}
			}
		}
	}
}

// Пол 6: Земля / грязь
static void GenerateFloor_Dirt(Color tex[TS][TS]) {
	Color dirt = (Color){75, 60, 42, 255};
	Color dirtLight = (Color){110, 88, 62, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.5f, y * 0.5f, 3);
			Color c = MixColor(dirt, dirtLight, n);
			
			float dark = fbm(x * 0.2f + 30.0f, y * 0.2f, 2);
			if (dark > 0.7f) {
				c = MixColor(c, (Color){40, 30, 20, 255}, (dark - 0.7f) * 2.0f);
			}
			
			tex[y][x] = c;
		}
	}
}

// Пол 7: Красная плитка (акценты)
static void GenerateFloor_RedTile(Color tex[TS][TS]) {
	Color tile = (Color){150, 55, 40, 255};
	Color tileDark = (Color){110, 35, 25, 255};
	Color grout = (Color){60, 25, 18, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			bool isGrout = (x % 16 == 0 || y % 16 == 0);
			if (isGrout) {
				tex[y][x] = grout;
			} else {
				float n = fbm(x * 0.4f, y * 0.4f, 2);
				tex[y][x] = MixColor(tileDark, tile, n);
			}
		}
	}
}

static void ApplyDirt(Color tex[TS][TS], float intensity, Color dirtColor) {
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.15f + 50.0f, y * 0.15f + 50.0f, 3);
			if (n > 0.5f) {
				float t = (n - 0.5f) * 2.0f * intensity;
				if (t > 1.0f) t = 1.0f;
				tex[y][x] = MixColor(tex[y][x], dirtColor, t);
			}
			
			int noise = (hash2(x, y) % 20) - 10; 
			int r = (int)tex[y][x].r + noise;
			int g = (int)tex[y][x].g + noise;
			int b = (int)tex[y][x].b + noise;
			
			// НОВОЕ: Увеличиваем контраст (тени становятся чернее, света ярче)
			float contrast = 1.25f;
			r = (int)((r - 128) * contrast + 128);
			g = (int)((g - 128) * contrast + 128);
			b = (int)((b - 128) * contrast + 128);
			
			if (r < 0) r = 0; if (r > 255) r = 255;
			if (g < 0) g = 0; if (g > 255) g = 255;
			if (b < 0) b = 0; if (b > 255) b = 255;
			
			tex[y][x].r = (unsigned char)r;
			tex[y][x].g = (unsigned char)g;
			tex[y][x].b = (unsigned char)b;
		}
	}
}

// Собираем все текстуры в кучу
static void GenerateSky(void); // <-- Добавь эту строчку
void GenerateAllTextures(void) {
	Color dirtBrown = (Color){40, 30, 20, 255}; // Цвет грязи
	Color dirtDark  = (Color){20, 20, 25, 255}; // Цвет сырости/тени
	
	// Стены (8)
	GenerateWall_SandBrick  (wallTex[0]); ApplyDirt(wallTex[0], 0.6f, dirtBrown);
	GenerateWall_Stucco     (wallTex[1]); ApplyDirt(wallTex[1], 0.4f, dirtDark);
	GenerateWall_Stone      (wallTex[2]); ApplyDirt(wallTex[2], 0.7f, dirtDark);
	GenerateWall_WoodCrate  (wallTex[3]); ApplyDirt(wallTex[3], 0.3f, dirtBrown);
	GenerateWall_MetalDoor  (wallTex[4]); ApplyDirt(wallTex[4], 0.5f, dirtBrown);
	GenerateWall_Barrel     (wallTex[5]); ApplyDirt(wallTex[5], 0.5f, dirtBrown);
	GenerateWall_DarkStucco (wallTex[6]); ApplyDirt(wallTex[6], 0.6f, dirtDark);
	GenerateWall_BrickMarked(wallTex[7]); ApplyDirt(wallTex[7], 0.5f, dirtBrown);
	
	// Полы (8)
	GenerateFloor_Sand        (floorTex[0]); ApplyDirt(floorTex[0], 0.5f, dirtBrown);
	GenerateFloor_SandTile    (floorTex[1]); ApplyDirt(floorTex[1], 0.4f, dirtBrown);
	GenerateFloor_StoneTile   (floorTex[2]); ApplyDirt(floorTex[2], 0.6f, dirtDark);
	GenerateFloor_Gravel      (floorTex[3]); ApplyDirt(floorTex[3], 0.5f, dirtDark);
	GenerateFloor_Wood        (floorTex[4]); ApplyDirt(floorTex[4], 0.4f, dirtBrown);
	GenerateFloor_SandyStones (floorTex[5]); ApplyDirt(floorTex[5], 0.6f, dirtBrown);
	GenerateFloor_Dirt        (floorTex[6]); ApplyDirt(floorTex[6], 0.3f, dirtDark);
	GenerateFloor_RedTile     (floorTex[7]); ApplyDirt(floorTex[7], 0.4f, dirtDark);

	GenerateSky();
}

// ------------------------------------------------------------
//  СЕКТОР 7: НЕБО (без потолков)
// ------------------------------------------------------------

static void GenerateSky(void) {
	// 1. Базовый градиент
	for (int y = 0; y < SKY_H; y++) {
		float t = (float)y / (float)SKY_H; 
		Color c;
		if (t < 0.7f) {
			float st = t / 0.7f;
			c = (Color){(unsigned char)(5 + st * 10), (unsigned char)(5 + st * 15), (unsigned char)(20 + st * 25), 255};
		} else {
			float ht = (t - 0.7f) / 0.3f;
			c = (Color){(unsigned char)(15 + ht * 20), (unsigned char)(20 + ht * 25), (unsigned char)(45 + ht * 30), 255};
		}
		for (int x = 0; x < SKY_W; x++) {
			skyTex[y][x] = c;
		}
	}
	
	// 2. Звёзды
	for (int i = 0; i < 1500; i++) {
		unsigned int h1 = hash2(i * 13, 42);
		unsigned int h2 = hash2(i * 7, 1337);
		unsigned int h3 = hash2(i * 3, 777);
		
		int sx = h1 % SKY_W;
		int sy = h2 % (int)(SKY_H * 0.6f); 
		
		Color starCol = (Color){255, 255, 255, 255};
		int type = h3 % 20;
		
		if (type == 0) starCol = (Color){180, 200, 255, 255}; 
		else if (type == 1) starCol = (Color){255, 240, 200, 255}; 
		else if (type == 2) starCol = (Color){255, 200, 200, 255}; 
		else if (type > 15) starCol = (Color){150, 150, 150, 255}; 
		
		skyTex[sy][sx] = starCol;
		
		if (type < 3 && sx > 0 && sx < SKY_W-1 && sy > 0 && sy < SKY_H-1) {
			Color bg = skyTex[sy][sx+1];
			skyTex[sy][sx+1] = MixColor(bg, starCol, 0.4f);
			skyTex[sy][sx-1] = MixColor(bg, starCol, 0.4f);
			skyTex[sy+1][sx] = MixColor(bg, starCol, 0.4f);
			skyTex[sy-1][sx] = MixColor(bg, starCol, 0.4f);
		}
	}
}

// ------------------------------------------------------------
//  СЕКТОР 8: АУДИО (Глобальные настройки)
// -----------------------------------------------------------
static float music_volume = 0.5f; // Громкость (0.0 - 1.0)
static float volume_ui_timer = 0.0f;
static float stamina_ui_timer = 0.0f;

// ------------------------------------------------------------
//  СЕКТОР 9: КАРТА
// ------------------------------------------------------------

static void SetCell(int x, int y, CellType type, int wtex, int ftex, int ctex, float fz, float cz) {
	if (x < 0 || x >= MW || y < 0 || y >= MH) return;
	map[x][y].type     = (uint8_t)type;
	map[x][y].wallTex  = (uint8_t)wtex;
	map[x][y].floorTex = (uint8_t)ftex;
	map[x][y].ceilTex  = (uint8_t)ctex;
	map[x][y].floorZ   = fz;
	map[x][y].ceilZ    = cz;
}

static void Carve(int x1, int y1, int x2, int y2, int ftex, int ctex, float fz, float cz) {
	if (x1 > x2) { int t=x1; x1=x2; x2=t; }
	if (y1 > y2) { int t=y1; y1=y2; y2=t; }
	for (int x = x1; x <= x2; x++)
		for (int y = y1; y <= y2; y++)
			SetCell(x, y, CELL_EMPTY, 0, ftex, ctex, fz, cz);
}

// Починенная функция: не закрашивает проходы и двери
static void SetWalls(int x1, int y1, int x2, int y2, int wtex, float fz, float cz) {
	if (x1 > x2) { int t=x1; x1=x2; x2=t; }
	if (y1 > y2) { int t=y1; y1=y2; y2=t; }
	for (int x = x1; x <= x2; x++) {
		if (map[x][y1].type != CELL_DOOR)
			SetCell(x, y1, CELL_WALL, wtex, 0, 0, fz, cz);
		if (map[x][y2].type != CELL_DOOR)
			SetCell(x, y2, CELL_WALL, wtex, 0, 0, fz, cz);
	}
	for (int y = y1; y <= y2; y++) {
		if (map[x1][y].type != CELL_DOOR)
			SetCell(x1, y, CELL_WALL, wtex, 0, 0, fz, cz);
		if (map[x2][y].type != CELL_DOOR)
			SetCell(x2, y, CELL_WALL, wtex, 0, 0, fz, cz);
	}
}

void GenMap(void) {
	
	// Всё — каменная скала
	for (int x = 0; x < MW; x++)
		for (int y = 0; y < MH; y++)
			SetCell(x, y, CELL_WALL, 6, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	
	// 1. Центральный зал
	Carve(80, 80, 120, 120, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	SetWalls(80, 80, 120, 120, 0, Z_FLOOR, Z_HIGHCEIL);
	
	// 2. Коридоры
	Carve(20, 95, 80, 105, 3, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(120, 95, 159, 105, 3, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(95, 20, 105, 80, 3, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(95, 120, 105, 159, 3, 3, Z_FLOOR, Z_HIGHCEIL);
	
	// 3. Левая комната
	Carve(25, 85, 75, 115, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	SetWalls(25, 85, 75, 115, 1, Z_FLOOR, Z_HIGHCEIL);
	Carve(30, 90, 70, 92, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(30, 108, 70, 110, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(28, 97, 32, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	
	// 4. Правая комната
	Carve(125, 85, 175, 115, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	SetWalls(125, 85, 175, 115, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(147, 97, 153, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(170, 97, 174, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(135, 120, 165, 145, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(145, 115, 155, 120, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	
	// 5. Верхняя комната
	Carve(85, 25, 115, 55, 4, 1, Z_FLOOR, Z_CEIL);
	SetWalls(85, 25, 115, 55, 5, Z_FLOOR, Z_CEIL);
	Carve(80, 55, 120, 75, 4, 1, Z_FLOOR, Z_CEIL);
	Carve(95, 20, 105, 25, 4, 1, Z_FLOOR, Z_CEIL);
	
	// 6. Нижняя комната (туннели)
	Carve(85, 125, 115, 155, 2, 2, Z_FLOOR, Z_LOWCEIL);
	SetWalls(85, 125, 115, 155, 2, Z_FLOOR, Z_LOWCEIL);
	Carve(120, 130, 150, 150, 2, 2, Z_FLOOR, Z_LOWCEIL);
	Carve(50, 130, 80, 150, 2, 2, Z_FLOOR, Z_LOWCEIL);
	Carve(115, 135, 120, 140, 2, 2, Z_FLOOR, Z_LOWCEIL);
	Carve(80, 135, 85, 140, 2, 2, Z_FLOOR, Z_LOWCEIL);
	
	// 7. Внешние комнаты
	Carve(30, 30, 70, 50, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(30, 50, 35, 80, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(130, 30, 170, 50, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(165, 50, 170, 80, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(30, 150, 70, 170, 6, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(130, 150, 170, 170, 6, 0, Z_FLOOR, Z_HIGHCEIL);
	
	// ========================================================
	// 8. ПРОБИВАЕМ ВХОДЫ (чтобы не было глухих стен)
	// ========================================================
	Carve(80, 95, 81, 105, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(119, 95, 120, 105, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(95, 80, 105, 81, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(95, 119, 105, 120, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	
	Carve(20, 97, 30, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(70, 97, 80, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	
	Carve(120, 97, 130, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(170, 97, 180, 103, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	Carve(145, 115, 155, 125, 1, 3, Z_FLOOR, Z_HIGHCEIL);
	
	Carve(95, 20, 105, 30, 4, 1, Z_FLOOR, Z_CEIL);
	Carve(95, 55, 105, 75, 4, 1, Z_FLOOR, Z_CEIL);
	
	Carve(95, 125, 105, 140, 2, 2, Z_FLOOR, Z_LOWCEIL);
	Carve(115, 135, 125, 145, 2, 2, Z_FLOOR, Z_LOWCEIL);
	Carve(80, 135, 90, 145, 2, 2, Z_FLOOR, Z_LOWCEIL);
	
	Carve(70, 30, 80, 50, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(30, 50, 35, 60, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(120, 30, 130, 50, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(165, 50, 170, 60, 5, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(70, 150, 80, 170, 6, 0, Z_FLOOR, Z_HIGHCEIL);
	Carve(120, 150, 130, 170, 6, 0, Z_FLOOR, Z_HIGHCEIL);
	
	// ========================================================
	// 9. ЯЩИКИ (Укрытия)
	// ========================================================
	
	// Центральный зал
	SetCell(100, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(84, 84, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(85, 84, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(84, 85, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(116, 84, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(115, 84, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(116, 85, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(84, 116, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(85, 116, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(84, 115, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(116, 116, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(115, 116, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(116, 115, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(97, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(103, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(100, 97, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(100, 103, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// Коридоры
	SetCell(35, 96, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(36, 96, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(55, 104, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(56, 104, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(70, 96, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(130, 104, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(131, 104, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(145, 96, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(146, 96, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(155, 104, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(96, 40, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(96, 41, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(104, 60, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(104, 61, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(96, 135, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(96, 136, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(104, 150, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(104, 151, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// Левая комната
	SetCell(40, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(28, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(29, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(28, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(29, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(72, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(72, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(50, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(50, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// Правая комната
	SetCell(128, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(129, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(128, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(129, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(172, 88, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(172, 112, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(150, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(160, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(140, 100, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(140, 125, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(141, 125, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(160, 140, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(159, 140, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// Верхняя комната
	SetCell(90, 30, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(91, 30, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(110, 30, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(110, 31, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(90, 50, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(91, 50, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(110, 50, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(110, 51, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(98, 40, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(102, 40, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(98, 45, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(102, 45, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// Нижняя комната
	SetCell(88, 130, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(89, 130, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(112, 130, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(112, 131, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(88, 150, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(112, 150, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(112, 151, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(130, 135, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(131, 135, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(145, 145, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(144, 145, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(60, 135, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(61, 135, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(75, 145, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(74, 145, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// Внешние комнаты
	SetCell(35, 35, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(36, 35, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(65, 35, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(65, 36, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(35, 45, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(65, 45, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(50, 40, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(135, 35, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(136, 35, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(165, 35, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(165, 36, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(135, 45, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(165, 45, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(150, 40, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(35, 155, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(36, 155, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(65, 155, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(65, 156, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(35, 165, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(65, 165, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(50, 160, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(135, 155, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(136, 155, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(165, 155, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(165, 156, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(135, 165, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(165, 165, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	SetCell(150, 160, CELL_CRATE, 3, 1, 0, Z_FLOOR, Z_CRATE);
	
	// 10. Обводка краёв (в самом конце, чтобы ничего не пробить)
	for (int x = 0; x < MW; x++) {
		SetCell(x, 0, CELL_WALL, 6, 0, 0, Z_FLOOR, Z_HIGHCEIL);
		SetCell(x, MH-1, CELL_WALL, 6, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	}
	for (int y = 0; y < MH; y++) {
		SetCell(0, y, CELL_WALL, 6, 0, 0, Z_FLOOR, Z_HIGHCEIL);
		SetCell(MW-1, y, CELL_WALL, 6, 0, 0, Z_FLOOR, Z_HIGHCEIL);
	}
	
// --- ИНИЦИАЛИЗАЦИЯ ОРУЖИЯ ---
	pistol.ammo = 7;
	pistol.maxAmmo = 7;
	pistol.fireRate = 0.5f;
	pistol.lastShotTime = 0.0f;
	pistol.recoil = 0.0f;
	
	// --- ИНИЦИАЛИЗАЦИЯ ПЕРЕЗАРЯДКИ ---
	pistol.reloadTime = 5.0f;     // Перезарядка 5 секунд
	pistol.reloadTimer = 0.0f;
	pistol.isReloading = false;
	
	// --- ОЧИСТКА ГИЛЬЗ ---
	for (int i = 0; i < MAX_SHELLS; i++) {
		shells[i].active = false;
	}
	
		// --- ОЧИСТКА ДЫРОК ОТ ПУЛЬ ---
	for (int i = 0; i < MAX_BULLETS; i++) {
		bulletHoles[i].active = false;
	}
	
	// 11. Старт игрока
	player.x = 100.5f;
	player.y = 110.5f;
	player.z = Z_FLOOR;
	player.pa = -1.57f;
	player.pitch = 0.0f;
	player.vx = 0; player.vy = 0; player.vz = 0;
	player.onGround = true;
	player.stamina = 100.0f;
	player.maxStamina = 100.0f;
	player.aimSpreadX = 0.0f;
	player.aimSpreadY = 0.0f;
}

// ------------------------------------------------------------
//  СЕКТОР 10: ФИЗИКА И УПРАВЛЕНИЕ
// ------------------------------------------------------------

static bool GetFloorCeilAt(float wx, float wy, float *outFloorZ, float *outCeilZ, bool allowStepUp, float currentZ) {
	int cx = (int)floorf(wx);
	int cy = (int)floorf(wy);
	if (cx < 0 || cx >= MW || cy < 0 || cy >= MH) return false;
	
	Cell *c = &map[cx][cy];
	if (c->type == CELL_WALL) return false;
	
		if (c->type == CELL_CRATE) {
			// Если ящик высокий (как стена), мы не можем на него зайти
			if (Z_CRATE > 1.0f) return false; 
			
			if (currentZ >= c->ceilZ - STEP_UP) {
				*outFloorZ = c->ceilZ;                        
				*outCeilZ  = Z_HIGHCEIL; 
				return true;
			}
			return false; 
		}
	
	float fz = c->floorZ;
	float cz = c->ceilZ;
	
	if (cz - fz < PLAYER_HEIGHT) return false;
	
	*outFloorZ = fz;
	*outCeilZ  = cz;
	return true;
}

static bool CanStandAt(float wx, float wy, float z, bool allowStepUp) {
	float r = PLAYER_RADIUS;
	float corners[4][2] = {
		{wx - r, wy - r}, {wx + r, wy - r},
		{wx - r, wy + r}, {wx + r, wy + r}
	};
	float fz, cz;
	for (int i = 0; i < 4; i++) {
		if (!GetFloorCeilAt(corners[i][0], corners[i][1], &fz, &cz, allowStepUp, z))
			return false;
	}
	return true;
}

void SpawnBulletHole(float startX, float startY, float startZ, float dirX, float dirY, float dirZ, float maxDist) {
	int mapX = (int)floorf(startX);
	int mapY = (int)floorf(startY);
	
	float deltaDistX = (fabsf(dirX) < 1e-6f) ? 1e30f : fabsf(1.0f / dirX);
	float deltaDistY = (fabsf(dirY) < 1e-6f) ? 1e30f : fabsf(1.0f / dirY);
	float sideDistX, sideDistY;
	int stepX, stepY;
	
	if (dirX < 0) { stepX = -1; sideDistX = (startX - mapX) * deltaDistX; }
	else { stepX = 1; sideDistX = (mapX + 1.0f - startX) * deltaDistX; }
	if (dirY < 0) { stepY = -1; sideDistY = (startY - mapY) * deltaDistY; }
	else { stepY = 1; sideDistY = (mapY + 1.0f - startY) * deltaDistY; }
	
	bool hit = false;
	int side = 0;
	float perpDist = 0.0f;
	
	while (!hit) {
		if (sideDistX < sideDistY) {
			sideDistX += deltaDistX;
			mapX += stepX;
			side = 0;
		} else {
			sideDistY += deltaDistY;
			mapY += stepY;
			side = 1;
		}
		
		if (mapX < 0 || mapX >= MW || mapY < 0 || mapY >= MH) break;
		
		perpDist = (side == 0) ? (mapX - startX + (1 - stepX) / 2.0f) / dirX : (mapY - startY + (1 - stepY) / 2.0f) / dirY;
		if (perpDist > maxDist) break;
		
		Cell *c = &map[mapX][mapY];
		float floorZ = c->floorZ;
		float ceilZ = c->ceilZ;
		float hitZ = startZ + dirZ * perpDist;
		
		if (hitZ > Z_HIGHCEIL) return;
		
		// Попадание в пол
		if (hitZ < floorZ && dirZ < 0.0f) {
			float tFloor = (floorZ - startZ) / dirZ;
			float hitX = startX + dirX * tFloor;
			float hitY = startY + dirY * tFloor;
			
			for (int i = 0; i < MAX_BULLETS; i++) {
				if (!bulletHoles[i].active) {
					bulletHoles[i].active = true;
					bulletHoles[i].x = hitX;
					bulletHoles[i].y = hitY;
					bulletHoles[i].z = floorZ; 
					return;
				}
			}
			return;
		}
		
		// Попадание в стену или ящик
		if (c->type == CELL_WALL || c->type == CELL_CRATE) {
			if (hitZ >= floorZ && hitZ <= ceilZ) {
				float hitX = startX + dirX * perpDist;
				float hitY = startY + dirY * perpDist;
				
				// ЗВУК ПОПАДАНИЯ ПО МАТЕРИАЛУ
				int tex = c->wallTex;
				Sound sndToPlay = sndWallImpactBeton;
				
				if (tex == 3) sndToPlay = sndWallImpactDerevo;
				else if (tex == 4 || tex == 5) sndToPlay = sndWallImpactZhelezo;
				else if (tex == 2) sndToPlay = sndWallImpactPesok;
				
				if (sndToPlay.frameCount > 0) {
					SetSoundPitch(sndToPlay, 0.9f + (rand() % 21) / 100.0f);
					PlaySound(sndToPlay);
				}
				
				for (int i = 0; i < MAX_BULLETS; i++) {
					if (!bulletHoles[i].active) {
						bulletHoles[i].active = true;
						bulletHoles[i].x = hitX;
						bulletHoles[i].y = hitY;
						bulletHoles[i].z = hitZ;
						return; 
					}
				}
				return; 
			}
		}
	}
}

void FireWeapon(void) {
	if (pistol.isReloading) return;
	float now = (float)GetTime();
	if (now - pistol.lastShotTime < pistol.fireRate) return;
	
	if (pistol.ammo <= 0) {
		if (pistol.sndEmpty.frameCount > 0) PlaySound(pistol.sndEmpty);
		pistol.lastShotTime = now; 
		return;
	}
	
	pistol.lastShotTime = now;
	pistol.ammo--;
	pistol.recoil = 1.0f;
	pistol.animTimer = 0.5f;
	
	if (pistol.sndShot.frameCount > 0) PlaySound(pistol.sndShot);
	
	// Позиция ствола для гильзы (ИСПРАВЛЕНО: right = вправо, ближе к стволу)
	float rightX = -sinf(player.pa);
	float rightY =  cosf(player.pa);
	float muzzleX = player.x + cosf(player.pa) * 0.4f + rightX * 0.15f;
	float muzzleY = player.y + sinf(player.pa) * 0.4f + rightY * 0.15f;
	float muzzleZ = player.z + EYE_HEIGHT - 0.1f;
	
	// ФАКТИЧЕСКИЕ ЭКРАННЫЕ КООРДИНАТЫ ПРИЦЕЛА
	float screenAimX = player.crosshairX + player.aimSpreadX;
	float screenAimY = player.crosshairY + player.aimSpreadY;
	
	// Текущий экранный Y прицела от верха экрана
	float aimPixelY = (float)SH / 2.0f + screenAimY;
	
	float dirX = cosf(player.pa);
	float dirY = sinf(player.pa);
	float planeLen = tanf(fov * 0.5f);
	float planeX = -dirY * planeLen;
	float planeY = dirX * planeLen;
	
	float projPlane = (float)SH * 0.5f / tanf(fov * 0.5f);
	float camXc = screenAimX / ((float)SW * 0.5f); 
	
	float rayDirX = dirX + planeX * camXc;
	float rayDirY = dirY + planeY * camXc;
	
	// Вычисляем заново horizon, как он считается в рендере (без покачивания bobV для точности выстрела)
	float pitchOffset = tanf(player.pitch) * projPlane;
	float currentHorizon = (float)SH / 2.0f + pitchOffset;
	
	// ЧЕСТНЫЙ тангенс угла пули, завязанный напрямую на пиксель прицела и горизонт
	// Направление Z = (физический Y горизонта на экране - Y прицела на экране) / projPlane
	float rayDirZ = (currentHorizon - aimPixelY) / projPlane;
	
	// Отправляем пулю в DDA
	SpawnBulletHole(player.x, player.y, player.z + EYE_HEIGHT, rayDirX, rayDirY, rayDirZ, 50.0f);
	
		// Спавн гильзы (ИСПРАВЛЕНО: медленнее, ближе к стволу)
	for (int i = 0; i < MAX_SHELLS; i++) {
		if (!shells[i].active) {
			shells[i].active = true;
			shells[i].x = muzzleX;
			shells[i].y = muzzleY;
			shells[i].z = muzzleZ;
			shells[i].vx = cosf(player.pa) * 1.5f + rightX * 1.0f + (rand() % 100 - 50) * 0.02f;
			shells[i].vy = sinf(player.pa) * 1.5f + rightY * 1.0f + (rand() % 100 - 50) * 0.02f;
			shells[i].vz = 2.5f + (rand() % 100 - 50) * 0.02f;
			shells[i].life = 5.0f;
			break;
		}
	}
}

	void UpdateShells(float dt) {
	for (int i = 0; i < MAX_SHELLS; i++) {
		if (!shells[i].active) continue;
		
		shells[i].life -= dt;
		
		float drag = expf(-1.5f * dt);
		shells[i].vx *= drag;
		shells[i].vy *= drag;
		
		shells[i].vz -= 9.8f * dt;
		
		float nx = shells[i].x + shells[i].vx * dt;
		float ny = shells[i].y + shells[i].vy * dt;
		float nz = shells[i].z + shells[i].vz * dt;
		
				if (nz < Z_FLOOR + 0.02f) { 
					nz = Z_FLOOR + 0.02f;
					
					// Звук при касании пола
					shellSndCd -= dt;
					if (shells[i].vz < -0.5f && shellSndCd <= 0.0f) {
						if (sndShellFloor.frameCount > 0) {
							SetSoundPitch(sndShellFloor, 0.9f + (rand() % 21) / 100.0f);
							PlaySound(sndShellFloor);
							shellSndCd = 0.05f;
						}
					}
					
					shells[i].vz = 0.0f; 
					shells[i].vx = 0.0f; 
					shells[i].vy = 0.0f;
				}
		
		int mapX = (int)floorf(nx);
		int mapY = (int)floorf(ny);
		if (mapX >= 0 && mapX < MW && mapY >= 0 && mapY < MH) {
			if (map[mapX][mapY].type == CELL_WALL || map[mapX][mapY].type == CELL_CRATE) {
				// ИСПРАВЛЕНО: При ударе о стену полностью останавливаем, чтобы не дёргалась
				shells[i].vx = 0.0f;
				shells[i].vy = 0.0f;
				nx = shells[i].x;
				ny = shells[i].y;
			}
		}
		
		shells[i].x = nx;
		shells[i].y = ny;
		shells[i].z = nz;
	}
}

	void UpdatePlayer(float dt) {
		Vector2 md = GetMouseDelta();
	
	// --- 1. ДВИГАЕМ ПРИЦЕЛ ВНУТРИ ЭКРАНА ---
	float aimSensitivity = 1.5f; 
	player.crosshairX += md.x * aimSensitivity;
	player.crosshairY += md.y * aimSensitivity;
	
	float limitX = 300.0f;
	float limitY = 200.0f;
	
	// --- 2. ЕСЛИ ПРИЦЕЛ УПЕРСЯ В КРАЙ — КРУТИМ КАМЕРУ ---
	if (player.crosshairX > limitX) {
		player.pa += (player.crosshairX - limitX) * 0.001f;
		player.crosshairX = limitX;
	}
	if (player.crosshairX < -limitX) {
		player.pa += (player.crosshairX + limitX) * 0.001f;
		player.crosshairX = -limitX;
	}
	
	// Крутим камеру вверх/вниз (pitch теперь в радианах!)
	float pitchSpeed = 0.002f; 
	if (player.crosshairY > limitY) {
		player.pitch -= (player.crosshairY - limitY) * pitchSpeed;
		player.crosshairY = limitY;
	}
	if (player.crosshairY < -limitY) {
		player.pitch -= (player.crosshairY + limitY) * pitchSpeed;
		player.crosshairY = -limitY;
	}
	
	// Ограничиваем наклон камеры (используем MAX_PITCH, который мы добавили в начале)
	if (player.pitch > MAX_PITCH) player.pitch = MAX_PITCH;
	if (player.pitch < -MAX_PITCH) player.pitch = -MAX_PITCH;
	
	// Защита от переполнения горизонтального угла
	if (player.pa < 0.0f)      player.pa += 2.0f * PI;
	if (player.pa > 2.0f * PI) player.pa -= 2.0f * PI;
	
	float cosA = cosf(player.pa);
	float sinA = sinf(player.pa);
	float wx = 0.0f, wy = 0.0f;
	
	if (IsKeyDown(KEY_W)) { wx += cosA; wy += sinA; }
	if (IsKeyDown(KEY_S)) { wx -= cosA; wy -= sinA; }
	if (IsKeyDown(KEY_A)) { wx += sinA; wy -= cosA; }
	if (IsKeyDown(KEY_D)) { wx -= sinA; wy += cosA; }
	
	float wl = sqrtf(wx * wx + wy * wy);
	if (wl > 0.001f) { wx /= wl; wy /= wl; }
	
	// --- СИСТЕМА СТАМИНЫ ---
	bool isMoving = (wl > 0.001f);
	bool wantsToSprint = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
	bool isSprinting = false;
	
	float maxSpeed = 2.4f; 
	
	if (wantsToSprint && isMoving && player.stamina > 0.0f) {
		isSprinting = true;
		maxSpeed *= 1.6f; 
		player.stamina -= 25.0f * dt; 
		if (player.stamina < 0.0f) player.stamina = 0.0f;
	} else {
		player.stamina += 15.0f * dt;
		if (player.stamina > player.maxStamina) player.stamina = player.maxStamina;
	}
	
	if (isSprinting || player.stamina < player.maxStamina) {
		stamina_ui_timer = 2.0f;
	}
		// --- ЗВУКИ СТАМИНЫ ---
		// 1. Игрок выдохся до нуля (играем тяжелый выдох)
		if (player.stamina <= 0.1f && !isFullyExhausted) {
			if (sndStaminaOut.frameCount > 0) PlaySound(sndStaminaOut);
			isFullyExhausted = true; // Запоминаем, что мы устали
		}
		
		// 2. Игрок полностью восстановился (играем вдох)
		if (player.stamina >= player.maxStamina - 0.1f && isFullyExhausted) {
			if (sndStaminaBack.frameCount > 0) PlaySound(sndStaminaBack);
			isFullyExhausted = false; // Сбрасываем усталость
		}
	
	// --- ФИЗИКА ---
	if (!isMoving && player.onGround) {
		player.vx = 0;
		player.vy = 0;
	} else if (player.onGround) {
		float speed = sqrtf(player.vx * player.vx + player.vy * player.vy);
		if (speed > 0.1f) { 
			float drop = ((speed < 1.5f) ? 1.5f : speed) * 4.0f * dt; 
			float ns = speed - drop; 
			if (ns < 0) ns = 0; 
			ns /= speed; 
			player.vx *= ns; player.vy *= ns; 
		}
		if (wl > 0.001f) { 
			float add = 10.0f * dt * maxSpeed; 
			player.vx += add * wx; player.vy += add * wy; 
		}
	} else {
		if (wl > 0.001f) { 
			float ws = maxSpeed; 
			if (ws > 0.42f) ws = 0.42f; 
			float add = 12.0f * dt * ws; 
			player.vx += add * wx; player.vy += add * wy; 
		}
	}
	
	if (IsKeyDown(KEY_SPACE) && player.onGround) { 
		player.vz = JUMP_SPEED * 1.15f; 
		player.onGround = false; 
	}
	
	player.vz += GRAVITY * dt; 
	player.z += player.vz * dt;
	
	float fz = Z_FLOOR, cz = Z_HIGHCEIL;
	if (GetFloorCeilAt(player.x, player.y, &fz, &cz, false, player.z)) {
		if (player.z <= fz && player.vz <= 0.0f) {
			player.z = fz; player.vz = 0.0f; player.onGround = true;
		} else {
			player.onGround = false;
		}
	} else {
		if (player.z < Z_FLOOR) player.z = Z_FLOOR;
		player.onGround = false;
	}
	
	float nx = player.x + player.vx * dt;
	float ny = player.y + player.vy * dt;
	
	if (CanStandAt(nx, player.y, player.z, true)) player.x = nx; else player.vx *= 0.1f;
	if (CanStandAt(player.x, ny, player.z, true)) player.y = ny; else player.vy *= 0.1f;
	
	// --- ПОКАЧИВАНИЕ (BOB) ---
	float hs = sqrtf(player.vx * player.vx + player.vy * player.vy);
	if (hs > 0.5f && player.onGround) { 
		player.bobPhase += dt * 3.3f * (hs / maxSpeed); 
		if (player.bobPhase > 2.0f * PI) player.bobPhase -= 2.0f * PI; 
		player.bobAmount += (1.0f - player.bobAmount) * dt * 6.0f; 
	} else {
		player.bobAmount += (0.0f - player.bobAmount) * dt * 6.0f;
	}

// --- ЗВУК ШАГОВ ---
		if (player.onGround && hs > 1.0f) {
			footstepTimer -= dt; // Просто вычитаем время кадра
			
			if (footstepTimer <= 0.0f) {
				if (sndFootstep.frameCount > 0) {
					SetSoundPitch(sndFootstep, 0.9f + (rand() % 21) / 100.0f);
					PlaySound(sndFootstep);
				}
				
				// Задаем частоту шагов
				if (isSprinting) {
					footstepTimer = 0.28f; // При беге шаги звучат чаще
				} else {
					footstepTimer = 0.50f; // При ходьбе шаги звучат реже
				}
			}
		} else {
			// Если стоим на месте, сбрасываем таймер на 0.1 сек
			footstepTimer = 0.1f; 
		}

	// --- СТРЕЛЬБА ---
	if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsKeyDown(KEY_LEFT_CONTROL)) {
		FireWeapon();
	}
	
	// --- ПЕРЕЗАРЯДКА ---
	if (IsKeyPressed(KEY_R) && !pistol.isReloading && pistol.ammo < pistol.maxAmmo) {
		pistol.isReloading = true;
		pistol.reloadTimer = pistol.reloadTime;
		if (pistol.sndReloadStart.frameCount > 0) PlaySound(pistol.sndReloadStart);
	}
	
	if (pistol.isReloading) {
		pistol.reloadTimer -= dt;
		if (pistol.reloadTimer <= 0.0f) {
			pistol.ammo = pistol.maxAmmo;
			pistol.isReloading = false;
			pistol.reloadTimer = 0.0f;
			if (pistol.sndReloadEnd.frameCount > 0) PlaySound(pistol.sndReloadEnd);
		}
	}
	
	if (pistol.recoil > 0.0f) {
		pistol.recoil -= dt * 20.0f;
		if (pistol.recoil < 0.0f) pistol.recoil = 0.0f;
	}
	
	UpdateShells(dt);
	
	// --- АНИМАЦИЯ ПИСТОЛЕТА ---
	if (pistol.isReloading) {
		pistol.currentFrame = 0; 
	} 
	else if (pistol.animTimer > 0.0f) {
		pistol.animTimer -= dt; 
		float progress = 0.5f - pistol.animTimer;
		if (progress < 0.16f) pistol.currentFrame = 1;
		else if (progress < 0.33f) pistol.currentFrame = 2;
		else pistol.currentFrame = 3;
	} 
	else {
		pistol.currentFrame = 0;
	}
	
	if (IsKeyPressed(KEY_M)) showMap = !showMap;
}

// ------------------------------------------------------------
//  ФИЛЬТР VHS/PS2 (ОПТИМИЗИРОВАННЫЙ: LUT + FIXED-POINT + OMP)
// ------------------------------------------------------------
void ApplyPS2Filter(Color *pixels) {
	static bool lutInitialized = false;
	static uint16_t vignetteLUT[SH][SW]; // Фиксированная точка 0..256
	static int8_t   noiseLUT[256][256];  // Предвычисленный шум -15..+14
	static uint8_t  clampLUT[1024];      // Быстрый clamp без ветвлений (смещение +256)
	
	// 1. Однократная инициализация таблиц при первом кадре
	if (!lutInitialized) {
		float halfW = SW * 0.5f;
		float halfH = SH * 0.5f;
		float maxDist = sqrtf(halfW * halfW + halfH * halfH);
		
		for (int y = 0; y < SH; y++) {
			float dy = (float)y - halfH;
			float dy2 = dy * dy;
			for (int x = 0; x < SW; x++) {
				float dx = (float)x - halfW;
				float dist = sqrtf(dx * dx + dy2);
				float vig = 1.0f - (dist / maxDist) * 0.8f;
				if (vig < 0.0f) vig = 0.0f;
				vignetteLUT[y][x] = (uint16_t)(vig * 256.0f);
			}
		}
		
		for (int y = 0; y < 256; y++) {
			for (int x = 0; x < 256; x++) {
				noiseLUT[y][x] = (int8_t)((hash2(x, y) % 30) - 15);
			}
		}
		
		for (int i = 0; i < 1024; i++) {
			int val = i - 256;
			if (val < 0) val = 0;
			else if (val > 255) val = 255;
			clampLUT[i] = (uint8_t)val;
		}
		
		lutInitialized = true;
	}
	
	static const int8_t bayer[4][4] = {
		{ -8,  0, -6,  2 },
		{  4, -4,  6, -2 },
		{ -5,  3, -7,  1 },
		{  7, -1,  5, -3 }
	};
	
	// Случайный сдвиг тайла шума для каждого кадра (дешево и без GetTime/float)
	static uint32_t frameSeed = 0;
	frameSeed = frameSeed * 1664525u + 1013904223u;
	uint8_t noiseOffX = (uint8_t)(frameSeed >> 16);
	uint8_t noiseOffY = (uint8_t)(frameSeed >> 24);
	
	// 2. Многопоточный проход по строкам
	#pragma omp parallel for num_threads(cpu_threads) schedule(static)
	for (int y = 0; y < SH; y++) {
		Color *row = pixels + y * SW;
		Color srcRow[SW];
		memcpy(srcRow, row, SW * sizeof(Color)); // Копируем строку в L1-кэш (убирает баг перезаписи CA)
		
		const uint16_t *vigRow = vignetteLUT[y];
		const int8_t *bayerRow = bayer[y & 3];
		const int8_t *noiseRow = noiseLUT[(uint8_t)(y + noiseOffY)];
		uint32_t scanlineMul = (y & 1) ? 256 : 218; // 0.85 * 256 ≈ 218 для четных строк
		
		for (int x = 0; x < SW; x++) {
			Color c = srcRow[x];
			
			// Хроматическая аберрация только по краям экрана (левые/правые 25%), в центре — бесплатно
			if (x < 270 || x > SW - 270) {
				int r_x = (x > 0) ? (x - 1) : 0;
				int b_x = (x < SW - 1) ? (x + 1) : (SW - 1);
				c.r = srcRow[r_x].r;
				c.b = srcRow[b_x].b;
			}
			
			// Целочисленная цветокоррекция (Desaturate 40% + грязный оттенок Buckshot Roulette)
			// gray ≈ (r + g + b) * 85 >> 8
			int gray = ((c.r + c.g + c.b) * 85) >> 8;
			// 0.6 * 256 = 154, 0.4 * 256 = 102
			int r = (c.r * 154 + gray * 102) >> 8;
			int g = (c.g * 154 + gray * 102) >> 8;
			int b = (c.b * 154 + gray * 102) >> 8;
			
			// Тинт: R * 0.95 (243/256), G * 1.0, B * 0.85 (218/256)
			r = (r * 243) >> 8;
			b = (b * 218) >> 8;
			
			// Дизеринг Байера (быстрая битовая маска & 3 вместо % 4)
			int dither = bayerRow[x & 3];
			r = clampLUT[r + dither + 256];
			g = clampLUT[g + dither + 256];
			b = clampLUT[b + dither + 256];
			
			// Объединяем скан-линию и виньетку в один множитель (0..256)
			uint32_t darkMul = (vigRow[x] * scanlineMul) >> 8;
			r = (r * darkMul) >> 8;
			g = (g * darkMul) >> 8;
			b = (b * darkMul) >> 8;
			
			// Зерно из 256x256 LUT + финальный clamp без if-ветвлений
			int grain = noiseRow[(uint8_t)(x + noiseOffX)];
			row[x].r = clampLUT[r + grain + 256];
			row[x].g = clampLUT[g + grain + 256];
			row[x].b = clampLUT[b + grain + 256];
		}
	}
}


// ------------------------------------------------------------
//  СЕКТОР 11: РЕНДЕР (ПОЛНОСТЬЮ ИСПРАВЛЕННЫЙ, МОНОЛИТНЫЙ)
// ------------------------------------------------------------

void RenderFrame(Color *pixels) {
	float dirX = cosf(player.pa), dirY = sinf(player.pa);
	float planeLen = tanf(fov * 0.5f);
	float planeX = -dirY * planeLen, planeY = dirX * planeLen;
	float camX = player.x, camY = player.y, camZ = player.z + EYE_HEIGHT;
	
	float bobV = sinf(player.bobPhase * 2.0f) * 6.0f * player.bobAmount;
	float bobH = cosf(player.bobPhase) * 4.0f * player.bobAmount;
	
	float projPlane = (float)SH * 0.5f / tanf(fov * 0.5f);
	float pitchOffset = tanf(player.pitch) * projPlane;
	int horizon = (int)(SH / 2.0f + pitchOffset + bobV); 
	
	// Луна
	float moonAngle = 2.5f; 
	float angleDiff = moonAngle - player.pa;
	while (angleDiff > PI) { angleDiff -= 2.0f * PI; }
	while (angleDiff < -PI) { angleDiff += 2.0f * PI; }
	int moonX = -9999;
	if (fabsf(angleDiff) < 1.45f) {
		moonX = (int)((SW / 2.0f) + tanf(angleDiff) * (SW / 2.0f) / tanf(fov/2.0f));
	}
	int moonY = (int)(horizon - 250.0f); 
	int moonR = 40;
	
	// ===== ПРОХОД 1: DDA (СТЕНЫ) =====
	#pragma omp parallel for num_threads(cpu_threads) schedule(static)
	for (int x = 0; x < SW; x++) {
		float camXc = 2.0f * ((float)x / (float)SW) - 1.0f + bobH / (float)SW;
		float rayDirX = dirX + planeX * camXc, rayDirY = dirY + planeY * camXc;
		int mapX = (int)floorf(camX), mapY = (int)floorf(camY);
		
		float deltaDistX = (fabsf(rayDirX) < 1e-6f) ? 1e30f : fabsf(1.0f / rayDirX);
		float deltaDistY = (fabsf(rayDirY) < 1e-6f) ? 1e30f : fabsf(1.0f / rayDirY);
		float sideDistX, sideDistY; int stepX, stepY;
		
		if (rayDirX < 0) { stepX = -1; sideDistX = (camX - mapX) * deltaDistX; } else { stepX = 1; sideDistX = (mapX + 1.0f - camX) * deltaDistX; }
		if (rayDirY < 0) { stepY = -1; sideDistY = (camY - mapY) * deltaDistY; } else { stepY = 1; sideDistY = (mapY + 1.0f - camY) * deltaDistY; }
		
		bool hit = false; int side = 0;
		while (!hit) {
			if (sideDistX < sideDistY) { sideDistX += deltaDistX; mapX += stepX; side = 0; } 
			else { sideDistY += deltaDistY; mapY += stepY; side = 1; }
			if (mapX < 0 || mapX >= MW || mapY < 0 || mapY >= MH) { hit = true; break; }
			
			Cell *c = &map[mapX][mapY];
			if (c->type == CELL_WALL || c->type == CELL_CRATE) hit = true;
		}
		
		float perpDist = (side == 0) ? (mapX - camX + (1 - stepX)/2.0f) / rayDirX : (mapY - camY + (1 - stepY)/2.0f) / rayDirY;
		if (perpDist < 0.1f) perpDist = 0.1f;
		zbuf[x] = perpDist;
		
		float wallFloorZ = Z_FLOOR, wallCeilZ = Z_CEIL; int wallTexID = 0;
				// --- НАПРАВЛЕННЫЙ СВЕТ ---
		float lightMultiplier = 1.0f;
		if (side == 0) {
			// Вертикальные стены (Запад/Восток)void GenerateAllTextures(void) {
			if (rayDirX > 0) lightMultiplier = 0.65f; // Западные (в тени)
			else lightMultiplier = 1.0f;              // Восточные (светлые)
		} else {
			// Горизонтальные стены (Север/Юг)
			if (rayDirY > 0) lightMultiplier = 0.65f; // Северные (в тени)
			else lightMultiplier = 1.0f;              // Южные (светлые)
		}
		lightBuf[x] = lightMultiplier; // <-- ДОБАВИТЬ ВОТ ЭТО
		
		// Применяем этот множитель к базовому затенению (которое считается позже)
		// (Если у тебя shade считается в проходе 2, мы передадим lightMultiplier туда).
		if (mapX >= 0 && mapX < MW && mapY >= 0 && mapY < MH) {
			Cell *c = &map[mapX][mapY]; 
			wallFloorZ = c->floorZ; wallCeilZ = c->ceilZ; wallTexID = c->wallTex;
			if (wallTexID >= N_WALL_TEX) wallTexID = 0;
		}
		
		float yTop = horizon - (wallCeilZ - camZ) / perpDist * projPlane;
		float yBottom = horizon - (wallFloorZ - camZ) / perpDist * projPlane;
		
		rawStartBuf[x] = (int)yTop; rawEndBuf[x] = (int)yBottom;
		int drawStart = (int)yTop;  int drawEnd = (int)yBottom;
		if (drawStart < 0) drawStart = 0;
		if (drawEnd > SH) drawEnd = SH;
		if (drawEnd <= drawStart) drawEnd = drawStart + 1;
		
		float sampleX = (side == 0) ? camY + perpDist * rayDirY : camX + perpDist * rayDirX;
		sampleX -= floorf(sampleX);
		int tx = (int)(sampleX * TS) & (TS - 1);
		if (tx < 0) tx += TS;
		
		drawStartBuf[x] = drawStart; drawEndBuf[x] = drawEnd; texIDBuf[x] = wallTexID;
		txBuf[x] = tx; sideBuf[x] = side; perpBuf[x] = perpDist;
	}
	
	// ===== ПРОХОД 2: РАСТЕРИЗАЦИЯ (СТЕНЫ И ПОЛ) =====
#pragma omp parallel for num_threads(cpu_threads)
	for (int y = 0; y < SH; y++) {
		Color *row = pixels + y * SW;
		int skyY = (int)((float)y - horizon + SKY_H * 0.5f);
		if (skyY < 0) skyY = 0; if (skyY >= SKY_H) skyY = SKY_H - 1;
		
		for (int x = 0; x < SW; x++) {
			float camXc = 2.0f * ((float)x / (float)SW) - 1.0f;
			float rayAngle = player.pa + camXc * (fov * 0.5f);
			float normAngle = rayAngle;
			while (normAngle < 0.0f) normAngle += 2.0f * PI;
			while (normAngle >= 2.0f * PI) normAngle -= 2.0f * PI;
			
			int skyX = (int)((normAngle / (2.0f * PI)) * (float)SKY_W);
			if (skyX < 0) skyX = 0; if (skyX >= SKY_W) skyX = SKY_W - 1;
			
			int dx = x - moonX; int dy = y - moonY;
			float distSq = (float)(dx*dx + dy*dy);
			float radiusSq = (float)(moonR * moonR);
			
			if (distSq < radiusSq) {
				float nx = (float)dx / (float)moonR; float ny = (float)dy / (float)moonR;
				float distFromCenter = sqrtf(nx*nx + ny*ny);
				float shading = 1.0f - (distFromCenter * 0.3f);
				float craterNoise = fbm(nx * 3.0f + 10.0f, ny * 3.0f + 10.0f, 3);
				float craterDarkness = (craterNoise > 0.45f && craterNoise < 0.6f) ? 0.25f : 0.1f;
				row[x] = (Color){ (unsigned char)(240 * shading * (1.0f - craterDarkness)), (unsigned char)(240 * shading * (1.0f - craterDarkness)), (unsigned char)(220 * shading * (1.0f - craterDarkness * 0.8f)), 255 };
			} else if (distSq < radiusSq * 2.5f) {
				float glowFactor = 1.0f - (sqrtf(distSq) - (float)moonR) / ((float)moonR * 1.5f);
				if (glowFactor < 0.0f) glowFactor = 0.0f;
				Color skyCol = skyTex[skyY][skyX];
				row[x] = (Color){ (unsigned char)(skyCol.r + (100 - skyCol.r) * glowFactor * 0.4f), (unsigned char)(skyCol.g + (120 - skyCol.g) * glowFactor * 0.4f), (unsigned char)(skyCol.b + (160 - skyCol.b) * glowFactor * 0.4f), 255 };
			} else {
				row[x] = skyTex[skyY][skyX];
			}
		}
		
		for (int x = 0; x < SW; x++) {
			int ds = drawStartBuf[x], de = drawEndBuf[x];
			if (y >= ds && y < de) {
				int lineHeight = rawEndBuf[x] - rawStartBuf[x];
				if (lineHeight < 1) lineHeight = 1;
				int ty = (int)(((float)(y - rawStartBuf[x]) / (float)lineHeight) * TS);
				if (ty < 0) ty = 0; if (ty >= TS) ty = TS - 1;
				Color c = wallTex[texIDBuf[x]][ty][txBuf[x]];
				float shade = 1.0f - (perpBuf[x] / depth) * 0.85f;
				if (shade < 0.2f) shade = 0.2f;
				shade *= lightBuf[x]; // <-- Применяем направленный свет!
				if (sideBuf[x] == 1) shade *= 0.75f;
				c.r = (unsigned char)(c.r * shade); c.g = (unsigned char)(c.g * shade); c.b = (unsigned char)(c.b * shade);
				row[x] = c;
			} else if (y > de) {
				float camXc = 2.0f * ((float)x / (float)SW) - 1.0f;
				float rayDirX = dirX + planeX * camXc, rayDirY = dirY + planeY * camXc;
				float p = (float)y - horizon;
				float tanAngle = (p / projPlane);
				if (tanAngle > 0.0f) {
					float rd = (camZ - Z_FLOOR) / tanAngle;
					if (rd > depth) rd = depth;
					float floorX = camX + rayDirX * rd, floorY = camY + rayDirY * rd;
					int cellX = (int)floorf(floorX), cellY = (int)floorf(floorY);
					int ftex = 0;
					if (cellX >= 0 && cellX < MW && cellY >= 0 && cellY < MH) {
						ftex = map[cellX][cellY].floorTex;
						if (ftex >= N_FLOOR_TEX) ftex = 0;
					}
					int ftx = (int)((floorX - (float)cellX) * (float)TS);
					if (ftx < 0) ftx = 0; if (ftx >= TS) ftx = TS - 1;
					int fty = (int)((floorY - (float)cellY) * (float)TS);
					if (fty < 0) fty = 0; if (fty >= TS) fty = TS - 1;
					Color c = floorTex[ftex][fty][ftx];
					float fshade = 1.0f - (rd / depth) * 0.85f;
					if (fshade < 0.2f) fshade = 0.2f;
					c.r = (unsigned char)(c.r * fshade); c.g = (unsigned char)(c.g * fshade); c.b = (unsigned char)(c.b * fshade);
					row[x] = c;
				}
			}
		}
	}
	
// ===== ПРОХОД 3: ПЛАВНОЕ ЗАТУХАНИЕ СПРАЙТОВ (НЕЛИНЕЙНЫЙ МАСШТАБ + ALPHA FADE) =====
	
	// 1. ОТРЕСОВКА ДЫРОК ОТ ПУЛЬ
	for (int i = 0; i < MAX_BULLETS; i++) {
		if (!bulletHoles[i].active) continue;
		
		float dx = bulletHoles[i].x - camX;
		float dy = bulletHoles[i].y - camY;
		
		float projDist = dx * dirX + dy * dirY;
		if (projDist < 0.2f) continue;
		
		float sideDist = dx * (-dirY) + dy * dirX;
		float camXc = sideDist / (projDist * planeLen);
		
		float screenX = (camXc + 1.0f) * ((float)SW * 0.5f) - (bobH * 0.5f);
		int px = (int)screenX;
		
		if (px < 0 || px >= SW) continue;
		
		float wallDist = zbuf[px];
		if (projDist > wallDist + 0.3f) continue;
		if (wallDist >= 100.0f) continue; // Полное исчезновение на 100 метрах
		
		// Нелинейная кривая размера: (1 - t)^2 от 1м до 100м
		float t = (wallDist - 1.0f) / 99.0f;
		if (t < 0.0f) t = 0.0f;
		float sizeScale = (1.0f - t) * (1.0f - t);
		
		// Не даем радиусу обнулиться раньше 100м — держим минимум 1 пиксель,
		// чтобы на последних метрах дырка плавно растворилась через альфа-канал
		int radius = (int)roundf(3.0f * sizeScale);
		if (radius < 1) radius = 1;
		if (radius > 3) radius = 3;
		
		// Плавный Fade Out (прозрачность) на дистанции от 75 до 100 метров (smoothstep)
		float alpha = 1.0f;
		if (wallDist > 75.0f) {
			float ft = (100.0f - wallDist) / 25.0f;
			if (ft < 0.0f) ft = 0.0f;
			alpha = ft * ft * (3.0f - 2.0f * ft);
		}
		if (alpha <= 0.01f) continue;
		
		// Учитываем дистанционное затенение стены (как в Проходе 2), чтобы вдали дырка не "светилась"
		float distShade = 1.0f - (wallDist / depth) * 0.85f;
		if (distShade < 0.2f) distShade = 0.2f;
		
		Color colCenter = { (unsigned char)(20 * distShade),  (unsigned char)(20 * distShade),  (unsigned char)(20 * distShade),  255 };
		Color colRim    = { (unsigned char)(100 * distShade), (unsigned char)(100 * distShade), (unsigned char)(100 * distShade), 255 };
		
		int py = (int)((float)horizon - (bulletHoles[i].z - camZ) / wallDist * projPlane);
		
		for (int sy = py - radius; sy <= py + radius; sy++) {
			if (sy < 0 || sy >= SH) continue;
			int dy_circle = sy - py;
			int halfWidth = (int)sqrtf((float)(radius * radius - dy_circle * dy_circle));
			Color *row = pixels + sy * SW;
			
			for (int sx_loop = px - halfWidth; sx_loop <= px + halfWidth; sx_loop++) {
				if (sx_loop < 0 || sx_loop >= SW) continue;
				if (projDist <= zbuf[sx_loop] + 0.3f) {
					Color target = (abs(sx_loop - px) > halfWidth - 1) ? colRim : colCenter;
					// Если полностью непрозрачна — пишем напрямую, иначе смешиваем с пикселем стены
					if (alpha >= 0.99f) {
						row[sx_loop] = target;
					} else {
						row[sx_loop] = MixColor(row[sx_loop], target, alpha);
					}
				}
			}
		}
	}
	
	// 2. ОТРЕСОВКА ГИЛЬЗ
	for (int i = 0; i < MAX_SHELLS; i++) {
		if (!shells[i].active) continue;
		
		float dx = shells[i].x - camX;
		float dy = shells[i].y - camY;
		
		float projDist = dx * dirX + dy * dirY;
		if (projDist < 0.1f || projDist >= 100.0f) continue;
		
		float sideDist = dx * (-dirY) + dy * dirX;
		float camXc = sideDist / (projDist * planeLen);
		
		float screenX = (camXc + 1.0f) * ((float)SW * 0.5f) - (bobH * 0.5f);
		int px = (int)screenX;
		
		if (px < 0 || px >= SW) continue;
		if (projDist > zbuf[px] + 0.1f) continue;
		
		// Нелинейное уменьшение размера: (1 - t)^2
		float t = (projDist - 1.0f) / 99.0f;
		if (t < 0.0f) t = 0.0f;
		float sizeScale = (1.0f - t) * (1.0f - t);
		
		// Минимум 1x1 пиксель до самых 100 метров, чтобы растворялась без скачка
		int w = (int)roundf(7.0f * sizeScale);
		int h = (int)roundf(3.0f * sizeScale);
		if (w < 1) w = 1;
		if (w > 7) w = 7;
		if (h < 1) h = 1;
		if (h > 3) h = 3;
		
		// Плавный Fade Out от 75 до 100 метров + затухание в конце жизни гильзы (последняя 1.0 секунда)
		float alpha = 1.0f;
		if (projDist > 75.0f) {
			float ft = (100.0f - projDist) / 25.0f;
			if (ft < 0.0f) ft = 0.0f;
			alpha *= ft * ft * (3.0f - 2.0f * ft);
		}
		if (shells[i].life < 1.0f) {
			float lt = shells[i].life;
			if (lt < 0.0f) lt = 0.0f;
			alpha *= lt;
		}
		if (alpha <= 0.01f) continue;
		
		// Дистанционное затенение, чтобы латунь не горела в темноте вдалеке
		float distShade = 1.0f - (projDist / depth) * 0.85f;
		if (distShade < 0.25f) distShade = 0.25f;
		
		Color col_rim   = { (unsigned char)(130 * distShade), (unsigned char)(90 * distShade),  (unsigned char)(40 * distShade),  255 };
		Color col_body  = { (unsigned char)(190 * distShade), (unsigned char)(150 * distShade), (unsigned char)(60 * distShade),  255 };
		Color col_shine = { (unsigned char)(255 * distShade), (unsigned char)(230 * distShade), (unsigned char)(150 * distShade), 255 };
		Color col_shade = { (unsigned char)(110 * distShade), (unsigned char)(80 * distShade),  (unsigned char)(30 * distShade),  255 };
		
		int py = (int)((float)horizon - (shells[i].z - camZ) / projDist * projPlane);
		
		for (int sx_loop = px - w / 2; sx_loop <= px + w / 2; sx_loop++) {
			if (sx_loop < 0 || sx_loop >= SW) continue;
			if (projDist > zbuf[sx_loop] + 0.1f) continue;
			
			int current_h = h;
			if (sx_loop > px + w / 4) current_h = h - 1;
			if (current_h < 1) current_h = 1;
			
			for (int sy = py - current_h / 2; sy <= py + current_h / 2; sy++) {
				if (sy < 0 || sy >= SH) continue;
				Color *row = pixels + sy * SW;
				
				Color c = col_body;
				if (sx_loop < px - w / 2 + 1) c = col_rim;
				else if (sy < py) c = col_shine;
				else if (sy > py) c = col_shade;
				
				if (alpha >= 0.99f) {
					row[sx_loop] = c;
				} else {
					row[sx_loop] = MixColor(row[sx_loop], c, alpha);
				}
			}
		}
	}
}
	
// ------------------------------------------------------------
//  СЕКТОР 12: МИНИ-КАРТА И MAIN
// ------------------------------------------------------------

void DrawMiniMap(RenderTexture2D minimapRT) {
	if (!showMap) return;
	
	BeginTextureMode(minimapRT);
	// Делаем фон полностью прозрачным (BLANK), чтобы квадрат миникарты не перекрывал игру
	ClearBackground(BLANK); 
	
	int centerX = 100;
	int centerY = 100;
	int radius = 80; // Радиус нашего радара
	int mmSize = 5;
	int curCellX = (int)player.x;
	int curCellY = (int)player.y;
	
	// 1. РИСУЕМ КАРТУ (Только внутри круга)
	for (int mx = -20; mx <= 20; mx++) {
		for (int my = -20; my <= 20; my++) {
			int cx = curCellX + mx;
			int cy = curCellY + my;
			if (cx < 0 || cx >= MW || cy < 0 || cy >= MH) continue;
			
			Cell *c = &map[cx][cy];
			if (c->type == CELL_EMPTY) continue; 
			
			// Вычисляем расстояние от центра, чтобы обрезать карту по кругу
			float dx = mx * mmSize;
			float dy = my * mmSize;
			if (dx * dx + dy * dy > radius * radius) continue; // Если за пределами круга — пропускаем
			
			// Темно-коричневые стены, чуть светлее ящики
			Color col = (Color){35, 30, 25, 255};
			if (c->type == CELL_CRATE) col = (Color){60, 45, 30, 255};
			
			DrawRectangle(centerX + (int)dx - (mmSize/2), centerY + (int)dy - (mmSize/2), mmSize, mmSize, col);
		}
	}
	
	// 2. ЗАТЕНЕНИЕ ПО КРАЯМ (Эффект линзы/радара)
	// Рисуем темные полупрозрачные круги поверх краев карты, чтобы она растворялась в темноте
	DrawRing((Vector2){centerX, centerY}, radius - 15, radius, 0, 360, 36, Fade(BLACK, 0.6f));
	DrawRing((Vector2){centerX, centerY}, radius - 5, radius, 0, 360, 36, Fade(BLACK, 0.8f));
	
	// 3. МЕТАЛЛИЧЕСКОЕ КОЛЬЦО (Внешний корпус)
	Color ringColor = (Color){80, 70, 50, 255}; // Грязно-латунный цвет
	// Внешний обод
	DrawRing((Vector2){centerX, centerY}, radius, radius + 6, 0, 360, 36, ringColor);
	// Внутренняя тонкая линия
	DrawRing((Vector2){centerX, centerY}, radius - 1, radius, 0, 360, 36, (Color){20, 20, 15, 255});
	
	// 4. НАСЕЧКИ (Как на компасе)
	for (int i = 0; i < 360; i += 15) {
		float angle = i * DEG2RAD;
		float x1 = centerX + cosf(angle) * (radius + 2);
		float y1 = centerY + sinf(angle) * (radius + 2);
		float x2 = centerX + cosf(angle) * (radius + 6);
		float y2 = centerY + sinf(angle) * (radius + 6);
		
		// Каждая 4-я насечка (каждые 60 градусов) — длиннее и ярче
		if (i % 60 == 0) {
			x1 = centerX + cosf(angle) * radius;
			DrawLine((int)x1, (int)y1, (int)x2, (int)y2, (Color){180, 160, 120, 255});
		} else {
			DrawLine((int)x1, (int)y1, (int)x2, (int)y2, (Color){100, 90, 70, 200});
		}
	}
	
	// 5. ПОТЁРТОСТИ И ЦАРАПИНЫ
	// Рисуем несколько случайных линий поверх кольца (используем фиксированные координаты, чтобы не мерцало)
	Color scratchColor = (Color){30, 25, 15, 150};
	DrawLine(centerX - 70, centerY - 45, centerX - 80, centerY - 55, scratchColor); // Царапина сверху-слева
	DrawLine(centerX + 60, centerY + 60, centerX + 75, centerY + 50, scratchColor);  // Царапина снизу-справа
	DrawLine(centerX + 82, centerY - 10, centerX + 85, centerY - 20, scratchColor);  // Справа
	DrawCircle(centerX - 40, centerY + 75, 2, (Color){20, 20, 10, 200}); // Пятно/скол
	DrawCircle(centerX + 45, centerY - 65, 1.5f, (Color){20, 20, 10, 200}); // Ещё скол
	
	// 6. МАРКЕР ИГРОКА (В центре)
	// Бледно-желтая точка
	DrawCircle(centerX, centerY, 3, (Color){220, 200, 150, 255});
	
	// Указатель направления взгляда (в виде треугольника/стрелки)
	float viewLength = 15.0f;
	float coneWidth = 4.0f;
	float lookX = centerX + cosf(player.pa) * viewLength;
	float lookY = centerY + sinf(player.pa) * viewLength;
	
	// Линия направления
	DrawLine(centerX, centerY, (int)lookX, (int)lookY, (Color){220, 200, 150, 255});
	
	// Верхушка стрелки (чтобы было видно, куда именно смотрит игрок)
	float perpAngle = player.pa + PI/2;
	int tipX1 = (int)(lookX + cosf(perpAngle) * coneWidth);
	int tipY1 = (int)(lookY + sinf(perpAngle) * coneWidth);
	int tipX2 = (int)(lookX - cosf(perpAngle) * coneWidth);
	int tipY2 = (int)(lookY - sinf(perpAngle) * coneWidth);
	DrawTriangle((Vector2){(float)lookX, (float)lookY}, (Vector2){(float)tipX1, (float)tipY1}, (Vector2){(float)tipX2, (float)tipY2}, (Color){220, 200, 150, 255});
	
	EndTextureMode();
}

void DrawMainMenu(void) {
	ClearBackground((Color){15, 15, 15, 255}); // Почти черный фон
	
	// Заголовок
	const char* title = "BATTLE SIMULATION";
	int titleSize = 60;
	int titleWidth = MeasureText(title, titleSize);
	int titleX = SW/2 - titleWidth/2;
	int titleY = SH/3;
	
	// Тень для текста (эффект старого монитора)
	DrawText(title, titleX + 3, titleY + 3, titleSize, (Color){50, 45, 30, 255});
	DrawText(title, titleX, titleY, titleSize, (Color){220, 200, 150, 255}); // Желтоватый текст
	
	const char* subtitle = "in hard times";
	int subWidth = MeasureText(subtitle, 20);
	DrawText(subtitle, SW/2 - subWidth/2, titleY + 70, 20, (Color){150, 140, 110, 255});
	
	// Кнопки
	const char* playText = "PLAY";
	const char* exitText = "EXIT";
	int playWidth = MeasureText(playText, 40);
	int exitWidth = MeasureText(exitText, 40);
	
	int playY = SH/2 + 50;
	int exitY = SH/2 + 130;
	
	// Кнопка PLAY
	Color playColor = (menuSelection == 0) ? (Color){255, 230, 150, 255} : (Color){80, 75, 60, 255};
	DrawText(playText, SW/2 - playWidth/2, playY, 40, playColor);
	if (menuSelection == 0) DrawText(">", SW/2 - playWidth/2 - 40, playY, 40, playColor);
	
	// Кнопка EXIT
	Color exitColor = (menuSelection == 1) ? (Color){255, 230, 150, 255} : (Color){80, 75, 60, 255};
	DrawText(exitText, SW/2 - exitWidth/2, exitY, 40, exitColor);
	if (menuSelection == 1) DrawText(">", SW/2 - exitWidth/2 - 40, exitY, 40, exitColor);
	
	// Подсказка внизу
	const char* hint = "W / S - NAVIGATE   |   ENTER - SELECT";
	int hintWidth = MeasureText(hint, 15);
	DrawText(hint, SW/2 - hintWidth/2, SH - 50, 15, (Color){60, 60, 60, 255});
}

// ------------------------------------------------------------
//  GLSL ШЕЙДЕР
// ------------------------------------------------------------

static const char *ps2FragmentShader =
"#version 330\n"
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"
"uniform sampler2D texture0;\n"
"uniform float uTime;\n"
"uniform vec2 uResolution;\n"
"out vec4 finalColor;\n"
"\n"
"float hash12(vec2 p) {\n"
"    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
"    p3 += dot(p3, p3.yzx + 33.33);\n"
"    return fract((p3.x + p3.y) * p3.z);\n"
"}\n"
"\n"
"float bayer4x4(vec2 pos) {\n"
"    int x = int(mod(pos.x, 4.0));\n"
"    int y = int(mod(pos.y, 4.0));\n"
"    int idx = y * 4 + x;\n"
"    float m[16] = float[16](\n"
"        0.0,  8.0,  2.0, 10.0,\n"
"       12.0,  4.0, 14.0,  6.0,\n"
"        3.0, 11.0,  1.0,  9.0,\n"
"       15.0,  7.0, 13.0,  5.0\n"
"    );\n"
"    return (m[idx] - 8.0) / 255.0;\n"
"}\n"
"\n"
"void main() {\n"
"    // УБРАЛИ CRT-ИЗГИБ: используем стандартные координаты без рамок\n"
"    vec2 uv = fragTexCoord;\n"
"    vec2 pixelPos = uv * uResolution;\n"
"    vec2 off = vec2(1.0 / uResolution.x, 0.0);\n"
"\n"
"    // 1. ХРОМАТИЧЕСКАЯ АБЕРРАЦИЯ\n"
"    float edge = abs(uv.x - 0.5) * 2.0;\n"
"    float caStrength = smoothstep(0.4, 1.0, edge);\n"
"    vec3 col;\n"
"    col.r = texture(texture0, uv - off * caStrength).r;\n"
"    col.g = texture(texture0, uv).g;\n"
"    col.b = texture(texture0, uv + off * caStrength).b;\n"
"\n"
"    // 2. ЦВЕТОКОРРЕКЦИЯ (средний баланс)\n"
"    float gray = (col.r + col.g + col.b) / 3.0;\n"
"    col = mix(col, vec3(gray), 0.38);\n"
"    col *= vec3(1.04, 0.97, 0.84);\n"
"\n"
"    // 3. ДИЗЕРИНГ\n"
"    col += bayer4x4(pixelPos);\n"
"\n"
"    // 4. СКАН-ЛИНИИ\n"
"    float scanline = sin(pixelPos.y * 2.5) * 0.5 + 0.5;\n"
"    col *= 0.88 + 0.12 * scanline; \n"
"    \n"
"    // RGB-МАСКА\n"
"    float mask = mod(floor(pixelPos.x) + floor(pixelPos.y), 3.0);\n"
"    if (mask < 1.0) col.r *= 1.08;\n"
"    else if (mask < 2.0) col.g *= 1.08;\n"
"    else col.b *= 1.08;\n"
"\n"
"    // 5. VHS ПОЛЗУЩАЯ ПОЛОСА\n"
"    float bar = smoothstep(0.0, 0.1, abs(fract(uv.y + uTime * 0.15) - 0.5));\n"
"    col *= mix(1.0, 0.92, bar);\n"
"\n"
"    // 6. ЗЕРНО\n"
"    float grain = (hash12(floor(pixelPos) + fract(uTime) * 100.0) - 0.5) * (25.0 / 255.0);\n"
"    col += grain;\n"
"\n"
"    // 7. ВИНЬЕТКА (осталась, чтобы фокусировать взгляд в центре)\n"
"    vec2 d = uv - 0.5;\n"
"    float dist = length(d) / 0.7071;\n"
"    float vignette = clamp(1.0 - dist * 0.72, 0.0, 1.0);\n"
"    col *= vignette;\n"
"\n"
"    // 8. ТЕМНЕЕ НА 3% (умножаем на 0.97)\n"
"    col *= 0.97;\n"
"\n"
"    finalColor = vec4(clamp(col, 0.0, 1.0), 1.0) * fragColor;\n"
"}\n";

int main(void) {
	InitWindow(SW, SH, "Battle simulation in hard times");
	InitAudioDevice();
	GenerateAllTextures();
	GenMap();
	
	// --- ЗАГРУЗКА СПРАЙТОВ ГЛОКА ---
	pistol.texIdle   = LoadTexture("pistol_anim/glock_idle.png");
	pistol.texFire1  = LoadTexture("pistol_anim/glock_fire1.png");
	pistol.texFire2  = LoadTexture("pistol_anim/glock_fire2.png");
	pistol.texFire3  = LoadTexture("pistol_anim/glock_fire3.png");
	pistol.texReload = LoadTexture("pistol_anim/glock_reload.png");
	
	pistol.currentFrame = 0;
	pistol.animTimer = 0.0f;
	
	Music music = LoadMusicStream("soundfiles/music_play.mp3");
	bool hasMusic = (music.frameCount > 0);
	if (hasMusic) { PlayMusicStream(music); music.looping = true; SetMusicVolume(music, music_volume); }
	
	// --- ЗАГРУЗКА ЗВУКОВ ---
	pistol.sndShot        = LoadSound("soundfiles/shot_pistol.mp3");
	pistol.sndReloadStart = LoadSound("soundfiles/reload_pistol_start.mp3");
	pistol.sndReloadEnd   = LoadSound("soundfiles/reload_pistol_end.mp3");
	pistol.sndEmpty       = LoadSound("soundfiles/nobullets_pistol.mp3");
	
	sndShellFloor        = LoadSound("soundfiles/gilza_padaet.mp3");
	sndWallImpactBeton   = LoadSound("soundfiles/shot_beton.mp3");
	sndWallImpactDerevo  = LoadSound("soundfiles/shot_derevo.mp3");
	sndWallImpactZhelezo = LoadSound("soundfiles/shot_zhelezo.mp3");
	sndWallImpactPesok   = LoadSound("soundfiles/shot_pesok.mp3");
	sndFootstep          = LoadSound("soundfiles/step.mp3");
	sndStaminaOut        = LoadSound("soundfiles/odyshka.mp3");
	sndStaminaBack       = LoadSound("soundfiles/vostanovlenie_dyhaniya.mp3");
	
	SetSoundVolume(sndShellFloor, 0.8f);
	SetSoundVolume(sndWallImpactBeton, 0.8f);
	SetSoundVolume(sndWallImpactDerevo, 0.8f);
	SetSoundVolume(sndWallImpactZhelezo, 0.8f);
	SetSoundVolume(sndWallImpactPesok, 0.8f);
	SetSoundVolume(sndFootstep, 0.04f);
	SetSoundVolume(sndStaminaOut, 0.8f);
	SetSoundVolume(sndStaminaBack, 0.1f);	
	SetSoundVolume(pistol.sndShot, 0.7f);
	SetSoundVolume(pistol.sndReloadStart, 1.0f);
	SetSoundVolume(pistol.sndReloadEnd, 1.0f);
	SetSoundVolume(pistol.sndEmpty, 1.2f);
	
	Image renderBuffer = GenImageColor(SW, SH, BLACK);
	Color *pixels = (Color *)renderBuffer.data;
	Texture2D screenTexture = LoadTextureFromImage(renderBuffer);
	
	RenderTexture2D minimapRT = LoadRenderTexture(200, 200);
	int minimapTick = 0;
	
	RenderTexture2D postRT = LoadRenderTexture(SW, SH);
	Shader ps2Shader = LoadShaderFromMemory(NULL, ps2FragmentShader);
	int locTime = GetShaderLocation(ps2Shader, "uTime");
	int locRes  = GetShaderLocation(ps2Shader, "uResolution");
	float res[2] = { (float)SW, (float)SH };
	SetShaderValue(ps2Shader, locRes, res, SHADER_UNIFORM_VEC2);
	
	int detected_procs = 4;
#ifdef _OPENMP
	detected_procs = omp_get_num_procs();
#endif
	cpu_threads = detected_procs - 1; 
	if (cpu_threads < 2) cpu_threads = 2; 
	if (cpu_threads > 8) cpu_threads = 8; 
#ifdef _OPENMP
	omp_set_num_threads(cpu_threads);
#endif
	
	SetTargetFPS(0); 
	EnableCursor(); // Мышка для меню
	
	while (!WindowShouldClose()) {
		float dt = GetFrameTime();
		if (dt > 0.05f) dt = 0.05f;
		
		if (hasMusic) UpdateMusicStream(music);
		
		// ============================================================
		// 1. ЛОГИКА (МЕНЮ, ПЕРЕХОД, ИГРА, ПАУЗА)
		// ============================================================
		if (currentState == STATE_MENU) {
			if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP)) menuSelection--;
			if (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN)) menuSelection++;
			if (menuSelection < 0) menuSelection = 1;
			if (menuSelection > 1) menuSelection = 0;
			
			if (IsKeyPressed(KEY_ENTER)) {
				if (menuSelection == 0) {
					currentState = STATE_TRANSITION;
					fadeToWhite = true;
					transitionAlpha = 0.0f;
				} else if (menuSelection == 1) {
					break; 
				}
			}
		} 
		else if (currentState == STATE_TRANSITION) {
			if (fadeToWhite) {
				transitionAlpha += dt * 1.0f; // Плавное затухание в белый (1 секунда)
				if (transitionAlpha >= 1.0f) {
					transitionAlpha = 1.0f;
					fadeToWhite = false;
					DisableCursor(); 
				}
			} else {
				transitionAlpha -= dt * 0.6f; // Медленное проявление (около 1.6 секунды)
				if (transitionAlpha <= 0.0f) {
					transitionAlpha = 0.0f;
					currentState = STATE_PLAYING;
				}
			}
		}
		
		else if (currentState == STATE_PLAYING) {
			// Если нажали ESC — мгновенно выходим в главное меню
			if (IsKeyPressed(KEY_ESCAPE)) {
				currentState = STATE_MENU;
				EnableCursor();
				GenMap(); // Сбрасываем карту и игрока
			}
			
		UpdatePlayer(dt); 
			
			if (volume_ui_timer > 0.0f) volume_ui_timer -= dt;
			if (stamina_ui_timer > 0.0f) stamina_ui_timer -= dt;
			if (IsKeyDown(KEY_LEFT_BRACKET)) { music_volume -= 0.5f * dt; if (music_volume < 0.0f) music_volume = 0.0f; volume_ui_timer = 2.0f; }
			if (IsKeyDown(KEY_RIGHT_BRACKET)) { music_volume += 0.5f * dt; if (music_volume > 1.0f) music_volume = 1.0f; volume_ui_timer = 2.0f; }
			if (hasMusic) SetMusicVolume(music, music_volume);
		}
		
		// ============================================================
		// 2. РЕНДЕР 3D МИРА
		// ============================================================
		// Рендерим 3D мир ТОЛЬКО когда мы в игре, на паузе, 
		// ИЛИ когда белый экран уже пошёл на спад (чтобы не было видно игру сквозь меню)
		bool need3DWorld = (currentState == STATE_PLAYING || (currentState == STATE_TRANSITION && !fadeToWhite));
		
		if (need3DWorld) {
			RenderFrame(pixels);
			UpdateTexture(screenTexture, pixels);
			
			if (showMap && (++minimapTick >= 10)) { minimapTick = 0; DrawMiniMap(minimapRT); }
			
			BeginTextureMode(postRT);
			ClearBackground(BLACK);
			DrawTexture(screenTexture, 0, 0, WHITE);
			
			// --- СПРАЙТ ПИСТОЛЕТА ---
			Texture2D currentTex = pistol.texIdle; 
			if (pistol.isReloading) currentTex = pistol.texReload;
			else if (pistol.currentFrame == 1) currentTex = pistol.texFire1;
			else if (pistol.currentFrame == 2) currentTex = pistol.texFire2;
			else if (pistol.currentFrame == 3) currentTex = pistol.texFire3;
			
			if (currentTex.id > 0) {
				float scale = 0.4f;
				int offsetX = 50;
				int offsetY = 0;
				int gunX = SW - (int)(currentTex.width * scale) - offsetX + (int)(player.crosshairX * 0.5f);
				int gunY = SH - (int)(currentTex.height * scale) + offsetY + (int)(player.crosshairY * 0.5f); 
				int recoilOffset = (int)(pistol.recoil * 40.0f);
				if (pistol.isReloading) {
					float reloadProgress = 1.0f - (pistol.reloadTimer / pistol.reloadTime);
					recoilOffset += (int)(sinf(reloadProgress * PI) * 200.0f); 
				}
				DrawTextureEx(currentTex, (Vector2){ (float)gunX, (float)(gunY + recoilOffset) }, 0.0f, scale, WHITE);
			}
			EndTextureMode();
		}
		
		// ============================================================
		// 3. ВЫВОД НА ЭКРАН
		// ============================================================
		BeginDrawing();
		ClearBackground(BLACK);
		
		// Если мы в меню ИЛИ в первой фазе перехода (затухание в белый) — рисуем МЕНЮ
		if (currentState == STATE_MENU || (currentState == STATE_TRANSITION && fadeToWhite)) {
			DrawMainMenu();
		} 
		// Иначе (игра, пауза, или вторая фаза перехода) — рисуем ИГРУ
		else {
			float nowTime = (float)GetTime();
			SetShaderValue(ps2Shader, locTime, &nowTime, SHADER_UNIFORM_FLOAT);
			
			BeginShaderMode(ps2Shader);
			DrawTextureRec(postRT.texture, (Rectangle){0, 0, (float)SW, (float)-SH}, (Vector2){0, 0}, WHITE);
			EndShaderMode();
			
			// HUD
			if (currentState == STATE_PLAYING) {
				
				if (showMap) {
					DrawTextureRec(minimapRT.texture, (Rectangle){0, 0, 200, -200}, (Vector2){SW - 220, 20}, WHITE);
				}
				
				if (volume_ui_timer > 0.0f) {
					DrawRectangle(20, SH - 40, 200, 20, Fade(BLACK, 0.5f));
					DrawRectangle(25, SH - 35, (int)(190 * music_volume), 10, BLUE);
					DrawText(TextFormat("Vol: %d%% ([ / ])", (int)(music_volume * 100)), 25, SH - 55, 12, WHITE);
				}
				
				// --- ПАТРОНЫ ---
				int magX = SW - 50;
				int magBottomY = SH - 40;
				int magHeight = 140;
				int magWidth = 28;
				float magYOffset = 0.0f;
				bool magIsFull = false;
				
				if (pistol.isReloading) {
					float rp = 1.0f - (pistol.reloadTimer / pistol.reloadTime);
					if (rp < 0.40f) {
						float t = rp / 0.40f;
						magYOffset = (t * t) * 380.0f;
					} else if (rp > 0.60f) {
						float t = (rp - 0.60f) / 0.40f;
						float ease = 1.0f - t;
						magYOffset = (ease * ease) * 380.0f;
						magIsFull = true;
					} else {
						magYOffset = 380.0f;
					}
				}
				
				int actualMagY = magBottomY + (int)magYOffset;
				DrawText("AMMO", magX, magBottomY - magHeight - 18, 10, (Color){150, 130, 90, 200});
				
				if (actualMagY < SH + magHeight) {
					DrawRectangle(magX, actualMagY - magHeight, magWidth, magHeight, (Color){10, 10, 10, 200});
					DrawRectangleLines(magX, actualMagY - magHeight, magWidth, magHeight, (Color){100, 85, 60, 220});
					float bulletH = (float)magHeight / pistol.maxAmmo;
					for (int i = 0; i < pistol.maxAmmo; i++) {
						int y = actualMagY - (int)((i + 1) * bulletH);
						bool isLoaded = magIsFull || (i < pistol.ammo);
						Color col = isLoaded ? (Color){220, 195, 70, 255} : (Color){35, 35, 35, 150};
						DrawRectangle(magX + 4, y + 2, magWidth - 8, (int)bulletH - 3, col);
					}
				}
				
				// --- ПЕРЕЗАРЯДКА ---
				int indX = magX - 75;
				int indY = magBottomY - 50;
				float circleAlpha = 0.0f;
				float numberAlpha = 1.0f;
				
				if (pistol.isReloading) {
					float rp = 1.0f - (pistol.reloadTimer / pistol.reloadTime);
					if (rp < 0.05f) {
						float t = rp / 0.05f; t = t * t * (3.0f - 2.0f * t);
						circleAlpha = t; numberAlpha = 1.0f - t;
					} else if (rp < 0.82f) {
						circleAlpha = 1.0f; numberAlpha = 0.0f;
					} else if (rp < 0.92f) {
						float t = (rp - 0.82f) / 0.10f; t = t * t * (3.0f - 2.0f * t);
						circleAlpha = 1.0f - t; numberAlpha = 0.0f;
					} else {
						float t = (rp - 0.92f) / 0.08f; t = t * t * (3.0f - 2.0f * t);
						circleAlpha = 0.0f; numberAlpha = t;
					}
				}
				
				if (circleAlpha > 0.01f) {
					float rp = 1.0f - (pistol.reloadTimer / pistol.reloadTime);
					int indR = 35;
					Color bg = (Color){10, 10, 10, (unsigned char)(200 * circleAlpha)};
					Color border = (Color){100, 85, 60, (unsigned char)(220 * circleAlpha)};
					Color fill = (Color){200, 170, 60, (unsigned char)(255 * circleAlpha)};
					Color label = (Color){150, 130, 90, (unsigned char)(220 * circleAlpha)};
					
					DrawCircle(indX, indY, indR, bg);
					DrawCircleLines(indX, indY, indR, border);
					int fillAngle = (int)(360.0f * rp);
					if (fillAngle > 0) DrawCircleSector((Vector2){indX, indY}, indR - 3, -90.0f, -90.0f + fillAngle, 32, fill);
					DrawText("RELOAD", indX - 24, indY + indR + 8, 11, label);
				}
				
				if (numberAlpha > 0.01f) {
					int ammoToShow = pistol.isReloading ? pistol.maxAmmo : pistol.ammo;
					unsigned char a1 = (unsigned char)(255 * numberAlpha);
					unsigned char a2 = (unsigned char)(220 * numberAlpha);
					DrawText(TextFormat("%d", ammoToShow), magX - 95, magBottomY - 70, 64, (Color){250, 230, 170, a1});
					DrawText(TextFormat("/%d", pistol.maxAmmo), magX - 55, magBottomY - 18, 18, (Color){150, 120, 80, a2});
				}
				
				// --- СТАМИНА ---
				if (stamina_ui_timer > 0.0f) {
					int stW = 200, stH = 6;
					int stX = SW / 2 - stW / 2;
					int stY = SH - 30;
					DrawRectangle(stX, stY, stW, stH, Fade(BLACK, 0.7f));
					float staminaPercent = player.stamina / player.maxStamina;
					int fillW = (int)(stW * staminaPercent);
					Color stCol;
					if (staminaPercent > 0.6f) stCol = (Color){80, 200, 80, 255};
					else if (staminaPercent > 0.3f) stCol = (Color){220, 180, 60, 255};
					else stCol = (Color){220, 80, 60, 255};
					DrawRectangle(stX, stY, fillW, stH, stCol);
				}
				
				// --- ПРИЦЕЛ ---
				int cx = SW / 2 + (int)player.crosshairX;
				int cy = SH / 2 + (int)player.crosshairY;
				Color chColor = (Color){255, 255, 255, 220}; 
				DrawLine(cx, cy - 3, cx, cy + 3, chColor);
				int gapX = 12, gapY = 4, len = 4;
				DrawLine(cx - gapX, cy - gapY, cx - gapX, cy + gapY, chColor);
				DrawLine(cx - gapX, cy - gapY, cx - gapX - len, cy - gapY, chColor);
				DrawLine(cx + gapX, cy - gapY, cx + gapX, cy + gapY, chColor);
				DrawLine(cx + gapX, cy - gapY, cx + gapX + len, cy - gapY, chColor);
			}
			
			// БЕЛАЯ ВСПЫШКА
			if (currentState == STATE_TRANSITION) {
				DrawRectangle(0, 0, SW, SH, Fade(WHITE, transitionAlpha));
			}
		}
		
		EndDrawing();
	}
	
	// --- ВЫГРУЗКА ПАМЯТИ ---
	if (pistol.texIdle.id > 0)   UnloadTexture(pistol.texIdle);
	if (pistol.texFire1.id > 0)  UnloadTexture(pistol.texFire1);
	if (pistol.texFire2.id > 0)  UnloadTexture(pistol.texFire2);
	if (pistol.texFire3.id > 0)  UnloadTexture(pistol.texFire3);
	if (pistol.texReload.id > 0) UnloadTexture(pistol.texReload);
	if (sndShellFloor.frameCount > 0) UnloadSound(sndShellFloor);
	if (sndWallImpactBeton.frameCount > 0) UnloadSound(sndWallImpactBeton);
	if (sndWallImpactDerevo.frameCount > 0) UnloadSound(sndWallImpactDerevo);
	if (sndWallImpactZhelezo.frameCount > 0) UnloadSound(sndWallImpactZhelezo);
	if (sndWallImpactPesok.frameCount > 0) UnloadSound(sndWallImpactPesok);
	if (sndFootstep.frameCount > 0) UnloadSound(sndFootstep);
	if (sndStaminaOut.frameCount > 0) UnloadSound(sndStaminaOut);
	if (sndStaminaBack.frameCount > 0) UnloadSound(sndStaminaBack);
	
	UnloadShader(ps2Shader);
	UnloadRenderTexture(postRT);
	if (hasMusic) UnloadMusicStream(music);
	UnloadRenderTexture(minimapRT);
	UnloadTexture(screenTexture); 
	UnloadImage(renderBuffer);
	CloseAudioDevice(); 
	CloseWindow();
	return 0;
}
