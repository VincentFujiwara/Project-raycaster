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
#define SKY_W 4024
#define SKY_H 1072

// Высоты (Потолки подняты, чтобы не застревать на ящиках)

#define Z_FLOOR      0.0f
#define Z_STEP       0.125f
#define Z_CRATE      2.5f
#define Z_CEIL       2.5f
#define Z_LOWCEIL    2.5f
#define Z_HIGHCEIL   2.5f

// Игрок
#define PLAYER_RADIUS 0.4f
#define EYE_HEIGHT    0.5f
#define PLAYER_HEIGHT 0.9f

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

float zbuf[SW];

// Буферы для двухпроходного рендера
int rawStartBuf[SW], rawEndBuf[SW];
int   drawStartBuf[SW];
int   drawEndBuf[SW];
int   texIDBuf[SW];
int   txBuf[SW];
int   sideBuf[SW];
float perpBuf[SW];
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

// Собираем все текстуры в кучу
static void GenerateSky(void);
void GenerateAllTextures(void) {
	// Стены (8)
	GenerateWall_SandBrick  (wallTex[0]);
	GenerateWall_Stucco     (wallTex[1]);
	GenerateWall_Stone      (wallTex[2]);
	GenerateWall_WoodCrate  (wallTex[3]);
	GenerateWall_MetalDoor  (wallTex[4]);
	GenerateWall_Barrel     (wallTex[5]);
	GenerateWall_DarkStucco (wallTex[6]);
	GenerateWall_BrickMarked(wallTex[7]);
	
	// Полы (8)
	GenerateFloor_Sand        (floorTex[0]);
	GenerateFloor_SandTile    (floorTex[1]);
	GenerateFloor_StoneTile   (floorTex[2]);
	GenerateFloor_Gravel      (floorTex[3]);
	GenerateFloor_Wood        (floorTex[4]);
	GenerateFloor_SandyStones (floorTex[5]);
	GenerateFloor_Dirt        (floorTex[6]);
	GenerateFloor_RedTile     (floorTex[7]);
	
	// Небо
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
		
		// ЗАЩИТА: Если пуля летит выше потолка (в открытое небо) — убираем её без дырок
		if (hitZ > Z_HIGHCEIL) {
			return;
		}
		
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
		
		if (nz < Z_FLOOR + 0.02f) { // Чуть ниже, чтобы визуально касалась пола
		nz = Z_FLOOR + 0.02f;
		shells[i].vz = 0.0f; // Жестко останавливаем по вертикали
		shells[i].vx = 0.0f; // И по горизонтали (лежит на полу)
		shells[i].vy = 0.0f;
		}
		
		int mapX = (int)floorf(nx);
		int mapY = (int)floorf(ny);
		if (mapX >= 0 && mapX < MW && mapY >= 0 && mapY < MH) {
			if (map[mapX][mapY].type == CELL_WALL || map[mapX][mapY].type == CELL_CRATE) {
				shells[i].vx = -shells[i].vx * 0.5f;
				shells[i].vy = -shells[i].vy * 0.5f;
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
	
	float maxSpeed = 2.5f; 
	
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
		player.bobPhase += dt * 9.0f * (hs / maxSpeed); 
		if (player.bobPhase > 2.0f * PI) player.bobPhase -= 2.0f * PI; 
		player.bobAmount += (1.0f - player.bobAmount) * dt * 6.0f; 
	} else {
		player.bobAmount += (0.0f - player.bobAmount) * dt * 6.0f;
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
	float rayDirZ = tanf(player.pitch);
	
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
#pragma omp parallel for num_threads(cpu_threads)
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
	
// ===== ПРОХОД 3: ЧЕСТНЫЙ 2D-НАКЛАД ДЕКАЛЕЙ И СПРАЙТОВ =====
	
	// 1. ОТРЕСОВКА ДЫРОК ОТ ПУЛЬ
	for (int i = 0; i < MAX_BULLETS; i++) {
		if (!bulletHoles[i].active) continue;
		float dx = bulletHoles[i].x - camX, dy = bulletHoles[i].y - camY;
		float projDist = dx * dirX + dy * dirY;
		if (projDist < 0.2f) continue;
		
		int px = (int)((SW / 2.0f) + (dx * (-dirY) + dy * dirX) / (projDist * planeLen) * (SW * 0.5f));
		if (px < 0 || px >= SW) continue;
		
		// ИСПРАВЛЕНО: проверка глубины ТОЛЬКО по центру, до отрисовки круга
		if (projDist > zbuf[px] + 0.3f) continue;
		
		float pureHorizon = (float)SH / 2.0f + rayDirZ * projPlane;
		int py = (int)(pureHorizon - (bulletHoles[i].z - camZ) / projDist * projPlane + bobV);
		
		int radius = (int)(8.0f / (projDist * 0.2f + 1.0f));
		if (radius < 2) radius = 2; if (radius > 10) radius = 10;
		
		for (int sy = py - radius; sy <= py + radius; sy++) {
			if (sy < 0 || sy >= SH) continue;
			int dy_circle = sy - py;
			int halfWidth = (int)sqrtf((float)(radius * radius - dy_circle * dy_circle));
			Color *row = pixels + sy * SW;
			
			for (int sx_loop = px - halfWidth; sx_loop <= px + halfWidth; sx_loop++) {
				if (sx_loop < 0 || sx_loop >= SW) continue;
				if (abs(sx_loop - px) > halfWidth - 2) row[sx_loop] = (Color){100, 100, 100, 255};
				else row[sx_loop] = (Color){20, 20, 20, 255};
			}
		}
	}
	
// 2. ОТРЕСОВКА ГИЛЬЗ (3D КАПСУЛА НА ПОЛУ)
	for (int i = 0; i < MAX_SHELLS; i++) {
		if (!shells[i].active) continue;
		float dx = shells[i].x - camX, dy = shells[i].y - camY;
		float projDist = dx * dirX + dy * dirY;
		if (projDist < 0.1f) continue;
		
		int px = (int)((SW / 2.0f) + (dx * (-dirY) + dy * dirX) / (projDist * planeLen) * (SW * 0.5f));
		if (px < 0 || px >= SW) continue;
		if (projDist > zbuf[px] + 0.1f) continue;
		
		float pureHorizon = (float)SH / 2.0f + rayDirZ * projPlane;
		int py = (int)(pureHorizon - (shells[i].z - camZ) / projDist * projPlane + bobV);
		
		// Размер гильзы (горизонтально вытянутая)
		float distFactor = 1.0f / (projDist * 0.25f + 1.0f);
		int w = (int)(10.0f * distFactor); // Длина
		int h = (int)(4.0f * distFactor);  // Толщина
		if (w < 2) w = 2; if (w > 16) w = 16;
		if (h < 1) h = 1; if (h > 6) h = 6;
		
		// Цвета (освещение сверху)
		Color col_rim   = (Color){130, 90, 40, 255};   // Донышко (темное)
		Color col_body  = (Color){190, 150, 60, 255};  // Латунь (основной)
		Color col_shine = (Color){255, 230, 150, 255}; // Блик сверху
		Color col_shade = (Color){110, 80, 30, 255};   // Тень снизу
		
		for (int sx_loop = px - w / 2; sx_loop <= px + w / 2; sx_loop++) {
			if (sx_loop < 0 || sx_loop >= SW) continue;
			
			// Форма капсулы: сужается к правому краю (носик пули)
			int current_h = h;
			if (sx_loop > px + w / 4) current_h = h - 1; // Сужение справа
			if (current_h < 1) current_h = 1;
			
			for (int sy = py - current_h / 2; sy <= py + current_h / 2; sy++) {
				if (sy < 0 || sy >= SH) continue;
				Color *row = pixels + sy * SW;
				
				Color c = col_body;
				
				// Левая часть (донышко)
				if (sx_loop < px - w / 2 + 1) c = col_rim;
				// Верхняя половина (блик от света)
				else if (sy < py) c = col_shine;
				// Нижняя половина (тень)
				else if (sy > py) c = col_shade;
				
				row[sx_loop] = c;
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
	ClearBackground(BLACK);
	
	int mmSize = 4;
	int curCellX = (int)player.x;
	int curCellY = (int)player.y;
	
	for (int mx = -25; mx < 25; mx++) {
		for (int my = -25; my < 25; my++) {
			int cx = curCellX + mx;
			int cy = curCellY + my;
			if (cx < 0 || cx >= MW || cy < 0 || cy >= MH) continue;
			
			Cell *c = &map[cx][cy];
			Color col = BLANK;
			if (c->type == CELL_WALL)         col = (Color){120, 100, 70, 255};
			else if (c->type == CELL_CRATE)   col = (Color){150, 100, 50, 255};
			else                              col = (Color){200, 180, 140, 80};
			
			DrawRectangle(100 + mx * mmSize, 100 + my * mmSize, mmSize, mmSize, col);
		}
	}

	
	DrawCircle(100, 100, 4, RED);
	DrawLine(100, 100,
			 100 + (int)(cosf(player.pa) * 15),
			 100 + (int)(sinf(player.pa) * 15), RED);
	
	EndTextureMode();
}

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
	
	// Проверка, что файлы на месте
	if (pistol.texIdle.id == 0) {
		TraceLog(LOG_WARNING, "ОШИБКА: Файлы не найдены в папке pistol_anim!");
	} else {
		TraceLog(LOG_INFO, "УСПЕХ: Спрайты пистолета загружены!");
	}
	
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
	
	// --- УСТАНОВКА ГРОМКОСТИ ЗВУКОВ НА 100% ---
	SetSoundVolume(pistol.sndShot, 1.0f);
	SetSoundVolume(pistol.sndReloadStart, 1.0f);
	SetSoundVolume(pistol.sndReloadEnd, 1.0f);
	SetSoundVolume(pistol.sndEmpty, 1.0f);
	
	if (pistol.sndShot.frameCount == 0) TraceLog(LOG_WARNING, "Звук shot_pistol.mp3 не найден!");
	
	Image renderBuffer = GenImageColor(SW, SH, BLACK);
	Color *pixels = (Color *)renderBuffer.data;
	Texture2D screenTexture = LoadTextureFromImage(renderBuffer);
	
	RenderTexture2D minimapRT = LoadRenderTexture(200, 200);
	int minimapTick = 0;
	
	int detected_procs = 4;
#ifdef _OPENMP
	detected_procs = omp_get_num_procs();
#endif
	cpu_threads = detected_procs;
	if (cpu_threads < 4) cpu_threads = 4; 
	if (cpu_threads > 16) cpu_threads = 16;
#ifdef _OPENMP
	omp_set_num_threads(cpu_threads);
#endif
	
	SetTargetFPS(0); 
	DisableCursor();
	
	while (!WindowShouldClose()) {
		float dt = GetFrameTime();
		if (dt > 0.05f) dt = 0.05f; 
		if (dt > 0.05f) dt = 0.05f;
		
		if (hasMusic) UpdateMusicStream(music);
		UpdatePlayer(dt);
		RenderFrame(pixels);
		UpdateTexture(screenTexture, pixels);
		
		// --- ОБНОВЛЕНИЕ ТАЙМЕРОВ ИНТЕРФЕЙСА ---
		if (volume_ui_timer > 0.0f) volume_ui_timer -= dt;
		if (stamina_ui_timer > 0.0f) stamina_ui_timer -= dt;
		
		// --- УПРАВЛЕНИЕ ГРОМКОСТЬЮ ---
		if (IsKeyDown(KEY_LEFT_BRACKET)) { music_volume -= 0.5f * dt; if (music_volume < 0.0f) music_volume = 0.0f; volume_ui_timer = 2.0f; }
		if (IsKeyDown(KEY_RIGHT_BRACKET)) { music_volume += 0.5f * dt; if (music_volume > 1.0f) music_volume = 1.0f; volume_ui_timer = 2.0f; }
		if (hasMusic) SetMusicVolume(music, music_volume);
		
		// --- МИНИ-КАРТА (обновление текстуры) ---
		if (showMap && (++minimapTick >= 10)) { 
			minimapTick = 0; 
			DrawMiniMap(minimapRT); 
		}
		
		BeginDrawing();
		ClearBackground(BLACK);
		DrawTexture(screenTexture, 0, 0, WHITE);
		
		// --- МИНИ-КАРТА (отрисовка на экран) ---
		if (showMap) {
			DrawTextureRec(minimapRT.texture, (Rectangle){0, 0, 200, -200}, (Vector2){SW - 220, 20}, WHITE);
		}
		
				// --- ШКАЛА ГРОМКОСТИ ---
		if (volume_ui_timer > 0.0f) {
			DrawRectangle(20, SH - 40, 200, 20, Fade(BLACK, 0.5f));
			DrawRectangle(25, SH - 35, (int)(190 * music_volume), 10, BLUE);
			DrawText(TextFormat("Vol: %d%% ([ / ])", (int)(music_volume * 100)), 25, SH - 55, 12, WHITE);
		}
		
		// --- ШКАЛА СТАМИНЫ (если она пропала) ---
		if (stamina_ui_timer > 0.0f) {
			int stX = SW / 2 - 150;
			int stY = SH / 2 + 180; 
			
			DrawRectangle(stX, stY, 300, 20, Fade(BLACK, 0.5f));
			Color stamCol = (player.stamina > 30.0f) ? GREEN : RED;
			DrawRectangle(stX + 5, stY + 5, (int)(290.0f * (player.stamina / player.maxStamina)), 10, stamCol);
			DrawText(TextFormat("Stamina: %d%%", (int)player.stamina), stX + 5, stY - 15, 12, WHITE);
		}
		
		// --- HUD (ВЕРХНЯЯ ЧАСТЬ) ---
		DrawText("WASD: Move / SPACE: Jump / SHIFT: Sprint", 20, 20, 16, RAYWHITE);
		DrawText("[ / ]: Volume  M: Map", 20, 40, 16, GRAY);
		DrawText(TextFormat("Pos: X=%.1f Y=%.1f Z=%.1f", player.x, player.y, player.z), 20, 85, 16, YELLOW);
		DrawText(TextFormat("Speed: %.1f u/s", sqrtf(player.vx * player.vx + player.vy * player.vy)), 20, 105, 16, GREEN);
		DrawText(TextFormat("Ammo: %d / %d [R]", pistol.ammo, pistol.maxAmmo), 20, 125, 16, RAYWHITE);
		DrawFPS(20, 65);
		
		// --- HUD ПЕРЕЗАРЯДКИ ---
		if (pistol.isReloading) {
			int cx = SW / 2;
			int cy = SH / 2;
			float progress = 1.0f - (pistol.reloadTimer / pistol.reloadTime);
			
			DrawCircle(cx, cy + 50, 30, Fade(BLACK, 0.6f));
			DrawCircle(cx, cy + 50, 25, Fade(RAYWHITE, 0.8f));
			DrawRing((Vector2){cx, cy + 50}, 20, 28, 0, 360.0f * progress, 32, GREEN);
			DrawText("RELOADING", cx - 50, cy + 90, 20, RAYWHITE);
		}
		
		// --- ПРИЦЕЛ (КАК В ARMA) ---
		int cx = SW / 2 + (int)player.crosshairX;
		int cy = SH / 2 + (int)player.crosshairY;
		
		Color chColor = (Color){255, 255, 255, 220}; 
		
		// 1. Центральная вертикальная мушка (палка) - меньше на треть
		DrawLine(cx, cy - 3, cx, cy + 3, chColor); // Было -4..+4, стало -3..+3
		
		// 2. Левый угол
		int gapX = 12; // Расстояние от центра
		int gapY = 4;  // Высота угла (было 6, стало 4)
		int len = 4;   // Длина загиба
		
		// Левый угол: вертикальная палка + загиб ВЛЕВО (наружу)
		DrawLine(cx - gapX, cy - gapY, cx - gapX, cy + gapY, chColor); // Вертикаль
		DrawLine(cx - gapX, cy - gapY, cx - gapX - len, cy - gapY, chColor); // Загиб влево (наружу)
		
		// 3. Правый угол (смотрит ВПРАВО, наружу)
		DrawLine(cx + gapX, cy - gapY, cx + gapX, cy + gapY, chColor); // Вертикаль
		DrawLine(cx + gapX, cy - gapY, cx + gapX + len, cy - gapY, chColor); // Загиб вправо (наружу)
		
		// --- СПРАЙТ ПИСТОЛЕТА ---
		Texture2D currentTex = pistol.texIdle; 
		
		if (pistol.isReloading) currentTex = pistol.texReload;
		else if (pistol.currentFrame == 1) currentTex = pistol.texFire1;
		else if (pistol.currentFrame == 2) currentTex = pistol.texFire2;
		else if (pistol.currentFrame == 3) currentTex = pistol.texFire3;
		
		if (currentTex.id > 0) {
			// --- ТВОИ НАСТРОЙКИ РАЗМЕРА И ПОЗИЦИИ ---
			float scale = 0.4f;      // Размер пистолета
			int offsetX = 50;        // Отступ от правого края
			int offsetY = 0;       // Отступ снизу
			// ------------------------------------------
			
			int gunX = SW - (int)(currentTex.width * scale) - offsetX + (int)(player.crosshairX * 0.5f);
			int gunY = SH - (int)(currentTex.height * scale) + offsetY + (int)(player.crosshairY * 0.5f); 
			
			// Отдача (пистолет дергается вниз)
			int recoilOffset = (int)(pistol.recoil * 40.0f);
			
			// Перезарядка (опускаем ствол вниз)
			if (pistol.isReloading) {
				float reloadProgress = 1.0f - (pistol.reloadTimer / pistol.reloadTime);
				recoilOffset += (int)(sinf(reloadProgress * PI) * 200.0f); 
			}
			
			int drawX = gunX;
			int drawY = gunY + recoilOffset;
			
			// Рисуем
			DrawTextureEx(currentTex, (Vector2){ (float)drawX, (float)drawY }, 0.0f, scale, WHITE);
		}
		
		EndDrawing();
			
	}
	
	// --- ВЫГРУЗКА ПАМЯТИ (ВНЕ ЦИКЛА) ---
	if (pistol.texIdle.id > 0)   UnloadTexture(pistol.texIdle);
	if (pistol.texFire1.id > 0)  UnloadTexture(pistol.texFire1);
	if (pistol.texFire2.id > 0)  UnloadTexture(pistol.texFire2);
	if (pistol.texFire3.id > 0)  UnloadTexture(pistol.texFire3);
	if (pistol.texReload.id > 0) UnloadTexture(pistol.texReload);
	
	if (hasMusic) UnloadMusicStream(music);
	UnloadRenderTexture(minimapRT);
	UnloadTexture(screenTexture); 
	UnloadImage(renderBuffer);
	CloseAudioDevice(); 
	CloseWindow();
	return 0;
}
