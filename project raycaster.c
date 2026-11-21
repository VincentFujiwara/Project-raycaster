#include "raylib.h"
#include "raymath.h"

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#define MMNOSOUND
#include <winsock2.h>
#include <ws2tcpip.h>

typedef struct tagMSG MSG, *LPMSG;
#include <iphlpapi.h>

#undef near
#undef far

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif
	
int cpu_threads = 4;

// ------------------------------------------------------------
//  ОСНОВНЫЕ КОНСТАНТЫ
// ------------------------------------------------------------

#define MAX_PITCH 1.0f  
#define SW 1080
#define SH 720
#define MW 192
#define MH 192
#define TS 256
#define SKY_W 4096
#define SKY_H 2048

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
#define PLAYER_HEIGHT 1.30f // taller remote player models

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
	bool isMagazine;
    float rotation;
    float spinSpeed;
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
	STATE_LOBBY,       // Экран выбора: Host или Join
	STATE_SERVER_LIST, // Список найденных серверов
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
	Sound sndHit;
	Sound sndDeath;

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

// ------------------------------------------------------------
//  СЕТЕВОЙ КОД
// ------------------------------------------------------------
#define MAX_NET_PLAYERS 8
#define SERVER_PORT 27015

#pragma pack(push, 1)
typedef struct {
	int id;
	float x, y, z;
	float pa;
	int health;
	int faceID;
	bool isDead;
	int kills;
	int deaths;
} NetworkPlayer;

typedef enum {
	PKT_DISCOVERY = 0,
	PKT_SERVER_INFO,
	PKT_JOIN,
	PKT_STATE_UPDATE,
	PKT_SHOOT,
	PKT_HIT,
	PKT_KILL,
	PKT_CHAT,
	PKT_DISCONNECT
} PacketType;

typedef struct {
	int type; // Используем int вместо enum, чтобы размер всегда был ровно 4 байта
	int senderID;
	NetworkPlayer playerData;
	int targetID;
	int damage;
	char message[128];
} NetworkPacket;
#pragma pack(pop)

NetworkPlayer netPlayers[MAX_NET_PLAYERS];
int myPlayerID = -1;
SOCKET udpSocket = INVALID_SOCKET;
struct sockaddr_in serverAddr;
struct sockaddr_in clientAddrs[MAX_NET_PLAYERS];
bool isServer = false;
bool isClient = false;
int connectedClients = 0;
int currentServerNumber = 1;
int discoveredServersCount = 0;
char myLocalIP[32] = "127.0.0.1";
char manualIP[32] = "";
bool typingIP = false;
char discoveredServersIPs[10][32];
int discoveredServerNums[10];
bool showTabMenu = false;
bool showPauseMenu = false;
bool showSettingsMenu = false;
// CS-style input scaling; this adjusts in-game aiming, not physical mouse hardware DPI.
float mouseSensitivity = 1.0f;
int virtualMouseDPI = 800;
static bool blockFireUntilRelease = false;

static void SaveControls(void) {
    FILE *f=fopen("controls.cfg","w");
    if(f) {fprintf(f,"sensitivity=%.3f\ndpi=%d\n",mouseSensitivity,virtualMouseDPI);fclose(f);}
}
static void LoadControls(void) {
    FILE *f=fopen("controls.cfg","r");
    if(!f)return;
    char line[100];
    while(fgets(line,sizeof(line),f)) {
        float v;int dpi;
        if(sscanf(line,"sensitivity=%f",&v)==1 && isfinite(v) && v>=0.1f && v<=5.0f)mouseSensitivity=v;
        if(sscanf(line,"dpi=%d",&dpi)==1 && dpi>=400 && dpi<=3200)virtualMouseDPI=dpi;
    }
    fclose(f);
}

bool showKeyHints = true;
bool showChatInput = false;
char chatInput[128] = "";
#define MAX_CHAT_LINES 10
#define MAX_KILL_LINES 6
char chatLines[MAX_CHAT_LINES][192];
float chatLineTimes[MAX_CHAT_LINES];
char killLines[MAX_KILL_LINES][96];
float killLineTimes[MAX_KILL_LINES];
bool netActive[MAX_NET_PLAYERS];
float lastHeard[MAX_NET_PLAYERS];
float deathAnimStart[MAX_NET_PLAYERS];
bool deathAnimPending[MAX_NET_PLAYERS];
float respawnTimer = 0.0f;
float deathFlash = 0.0f;

// Pictures: drop 1.png ... 8.png (or .jpg) into posters/ and restart.
#define MAX_POSTERS 8
#define POSTER_RES 96
static Color posterData[MAX_POSTERS][POSTER_RES][POSTER_RES];
static bool posterLoaded[MAX_POSTERS];
static signed char posterAt[MW][MH];
static bool showMeteors = true;
static bool quitRequested = false;
static Font chatFont;
static bool customChatFont = false;
static void InitChatFont(void) {
    const char *names[] = { "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/tahoma.ttf" };
    int cps[512],count=0;
    for(int c=32;c<=126;++c) cps[count++]=c;
    for(int c=0x0400;c<=0x052F;++c) cps[count++]=c;
    for(int i=0;i<3;++i) if(FileExists(names[i])) {
        chatFont=LoadFontEx(names[i],21,cps,count);
        if(chatFont.texture.id>0) {customChatFont=true;SetTextureFilter(chatFont.texture,TEXTURE_FILTER_BILINEAR);break;}
    }
    if(!customChatFont) chatFont=GetFontDefault();
}
static void DrawChatText(const char *str, int x,int y,int fs,Color color) {
    DrawTextEx(chatFont,str,(Vector2){(float)x,(float)y},(float)fs,1.0f,color);
}


static void AddChatLine(const char *msg) {
    for (int i = MAX_CHAT_LINES - 1; i > 0; --i) {
        memcpy(chatLines[i], chatLines[i-1], sizeof(chatLines[i]));
        chatLineTimes[i] = chatLineTimes[i-1];
    }
    snprintf(chatLines[0], sizeof(chatLines[0]), "%s", msg);
    chatLineTimes[0] = (float)GetTime();
}
static void AddKillLine(int killer, int victim) {
    for (int i = MAX_KILL_LINES-1; i > 0; --i) {
        memcpy(killLines[i], killLines[i-1], sizeof(killLines[i]));
        killLineTimes[i] = killLineTimes[i-1];
    }
    snprintf(killLines[0], sizeof(killLines[0]), "Player %d killed Player %d", killer+1, victim+1);
    killLineTimes[0] = (float)GetTime();
}
static void ClearPlayers(void) {
    memset(netPlayers, 0, sizeof(netPlayers));
    memset(netActive, 0, sizeof(netActive));
    memset(clientAddrs, 0, sizeof(clientAddrs));
    memset(lastHeard, 0, sizeof(lastHeard));
    memset(deathAnimPending, 0, sizeof(deathAnimPending));
    connectedClients = 0;
}


// 8 гарантированно свободных точек спавна в разных комнатах карты
static const float spawnPoints[8][2] = {
	{100.5f, 110.5f}, // Центральный зал (юг)
	{90.5f,  90.5f},  // Центральный зал (север)
	{45.5f,  100.5f}, // Левая комната
	{155.5f, 100.5f}, // Правая комната (сдвинули с ящика 150,100)
	{100.5f, 35.5f},  // Верхняя комната
	{100.5f, 140.5f}, // Нижняя комната (туннели)
	{45.5f,  40.5f},  // Северо-западная комната (сдвинули с ящика 50,40)
	{145.5f, 160.5f}  // Юго-восточная комната (сдвинули с ящика 150,160)
};

static void OnMyDeath(void) {
    if (myPlayerID < 0 || myPlayerID >= MAX_NET_PLAYERS) return;
    if (netPlayers[myPlayerID].isDead) return;
    netPlayers[myPlayerID].health = 0;
    netPlayers[myPlayerID].isDead = true;
    netPlayers[myPlayerID].deaths++;
    deathAnimPending[myPlayerID] = true;
    deathAnimStart[myPlayerID] = (float)GetTime();
    deathFlash = 1.0f;
    respawnTimer = 0.0f;
    pistol.ammo = 0; // old ammunition is lost on death
    pistol.isReloading = false;
    pistol.reloadTimer = 0.0f;
    pistol.animTimer = 0.0f;
    blockFireUntilRelease = true;
    showTabMenu=false;
    showPauseMenu=false;
    showSettingsMenu=false;
    if (sndDeath.frameCount > 0) PlaySound(sndDeath);
    EnableCursor();
}

void RespawnMe(void) {
	int idx = (myPlayerID >= 0 && myPlayerID < 8) ? myPlayerID : (rand() % 8);
	player.x = spawnPoints[idx][0];
	player.y = spawnPoints[idx][1];
	player.z = Z_FLOOR;
	player.vx = 0.0f;
	player.vy = 0.0f;
	player.vz = 0.0f;
	player.onGround = true;
	player.stamina = player.maxStamina;
    isFullyExhausted = false;
    pistol.ammo = pistol.maxAmmo;
    pistol.isReloading = false;
    pistol.reloadTimer = 0.0f;
    pistol.currentFrame = 0;
    pistol.animTimer = 0.0f;
    pistol.recoil = 0.0f;
    deathFlash = 0.0f;
    deathAnimPending[myPlayerID >= 0 && myPlayerID < MAX_NET_PLAYERS ? myPlayerID : 0] = false;
    // A respawn button was clicked: do not fire until the button is released.
    blockFireUntilRelease = true;
    pistol.lastShotTime = (float)GetTime();
    if (!showTabMenu && !showChatInput && !showPauseMenu && !showSettingsMenu) DisableCursor();
	
	if (myPlayerID >= 0 && myPlayerID < MAX_NET_PLAYERS) {
		netPlayers[myPlayerID].x = player.x;
		netPlayers[myPlayerID].y = player.y;
		netPlayers[myPlayerID].z = player.z;
		netPlayers[myPlayerID].health = 100;
		netPlayers[myPlayerID].isDead = false;
	}
}

bool InitNetwork(void) {
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
	if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
	udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udpSocket == INVALID_SOCKET) return false;
	
	int broadcast = 1;
	setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));
	
	int reuse = 1;
	setsockopt(udpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
	
	u_long mode = 1;
	ioctlsocket(udpSocket, FIONBIO, &mode);
	
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	return true;
}

void UpdateMyLocalIP(void);

void StartServer(int serverNumber) {
	isServer = true;
	isClient = false;
	myPlayerID = 0;
	currentServerNumber = serverNumber;
	UpdateMyLocalIP();
	
	struct sockaddr_in bindAddr;
	memset(&bindAddr, 0, sizeof(bindAddr));
	bindAddr.sin_family = AF_INET;
	bindAddr.sin_port = htons(SERVER_PORT);
	bindAddr.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(udpSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
		printf("SERVER BIND ERROR: %d\n", WSAGetLastError());
	}
	
	ClearPlayers();
	netPlayers[0].id = 0;
	netPlayers[0].faceID = 0;
	netActive[0] = true;
	lastHeard[0] = (float)GetTime();
	connectedClients = 1;
	RespawnMe();
}

void UpdateMyLocalIP(void) {
	ULONG outBufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
		for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != NULL; pCurr = pCurr->Next) {
			if (pCurr->OperStatus != IfOperStatusUp) continue;
			if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
			
			for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != NULL; pUnicast = pUnicast->Next) {
				struct sockaddr_in* sa = (struct sockaddr_in*)pUnicast->Address.lpSockaddr;
				char ipStr[32];
				inet_ntop(AF_INET, &(sa->sin_addr), ipStr, sizeof(ipStr));
				// Приоритет отдаем Wi-Fi интерфейсу (IF_TYPE_IEEE80211)
				if (pCurr->IfType == IF_TYPE_IEEE80211 || strcmp(myLocalIP, "127.0.0.1") == 0) {
					strcpy(myLocalIP, ipStr);
				}
			}
		}
	}
	if (pAddresses) free(pAddresses);
}

void SendDiscoveryBroadcast(void) {
	if (udpSocket == INVALID_SOCKET) return;
	
	NetworkPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.type = PKT_DISCOVERY;
	
	struct sockaddr_in bcastAddr;
	memset(&bcastAddr, 0, sizeof(bcastAddr));
	bcastAddr.sin_family = AF_INET;
	bcastAddr.sin_port = htons(SERVER_PORT);
	
	// 1. Общий broadcast и localhost
	bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&bcastAddr, sizeof(bcastAddr));
	
	bcastAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&bcastAddr, sizeof(bcastAddr));
	
	// 2. Перебор ВСЕХ активных адаптеров (включая Wi-Fi и точку доступа с телефона)
	ULONG outBufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
		for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != NULL; pCurr = pCurr->Next) {
			if (pCurr->OperStatus != IfOperStatusUp) continue;
			if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
			
			for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != NULL; pUnicast = pUnicast->Next) {
				struct sockaddr_in* sa = (struct sockaddr_in*)pUnicast->Address.lpSockaddr;
				ULONG mask = 0;
				ConvertLengthToIpv4Mask(pUnicast->OnLinkPrefixLength, &mask);
				
				// Вычисляем точный широковещательный адрес подсети: IP | (~Mask)
				bcastAddr.sin_addr.s_addr = (sa->sin_addr.s_addr & mask) | (~mask);
				sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&bcastAddr, sizeof(bcastAddr));
			}
		}
	}
	if (pAddresses) free(pAddresses);
}

void DiscoverServers(void) {
	isClient = true;
	isServer = false;
	discoveredServersCount = 0;
	SendDiscoveryBroadcast();
}

void ConnectToServer(const char* ip, int srvNum) {
	isClient = true;
	isServer = false;
	myPlayerID = -1;
	currentServerNumber = srvNum;
	
	ClearPlayers();
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.s_addr = inet_addr(ip);
	
	NetworkPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.type = PKT_JOIN;
	sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	RespawnMe();
}


void SendMyState(NetworkPlayer myState) {
	if (udpSocket == INVALID_SOCKET || myPlayerID < 0) return;
	NetworkPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.type = PKT_STATE_UPDATE;
	pkt.senderID = myPlayerID;
	pkt.playerData = myState;
	
	if (isServer) {
		for (int i = 1; i < MAX_NET_PLAYERS; i++) {
			if (clientAddrs[i].sin_addr.s_addr != 0) {
				sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
			}
		}
	} else if (isClient) {
		sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	}
}

void SendEvent(PacketType type, int targetID, int damage) {
	if (udpSocket == INVALID_SOCKET || myPlayerID < 0) return;
	NetworkPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.type = type;
	pkt.senderID = myPlayerID;
	pkt.targetID = targetID;
	pkt.damage = damage;
	
	if (isServer) {
		for (int i = 1; i < MAX_NET_PLAYERS; i++) {
			if (clientAddrs[i].sin_addr.s_addr != 0) {
				sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
			}
		}
	} else if (isClient) {
		sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	}
}

static void SendChat(const char *msg) {
    if (myPlayerID < 0 || udpSocket == INVALID_SOCKET || !msg || !msg[0]) return;
    NetworkPacket pkt = {0};
    pkt.type = PKT_CHAT;
    pkt.senderID = myPlayerID;
    snprintf(pkt.message, sizeof(pkt.message), "%s", msg);
    if (isServer) {
        for (int i=1; i<MAX_NET_PLAYERS; ++i)
            if (netActive[i]) sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0,
                                      (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
    } else if (isClient) {
        sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0,
               (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    }
}

static void LeaveServer(void) {
    if (udpSocket != INVALID_SOCKET && myPlayerID >= 0) SendEvent(PKT_DISCONNECT, -1, 0);
    if (udpSocket != INVALID_SOCKET) { closesocket(udpSocket); udpSocket = INVALID_SOCKET; }
    WSACleanup();
    isServer = false;
    isClient = false;
    myPlayerID = -1;
    ClearPlayers();
    showTabMenu = false;
    showPauseMenu = false;
    showSettingsMenu = false;
    showChatInput = false;
    EnableCursor();
}

static void ReturnToMainMenu(void) {
    LeaveServer();
    currentState = STATE_MENU;
    menuSelection = 0;
}

void UpdateNetwork(void) {
	if (udpSocket == INVALID_SOCKET) return;
	NetworkPacket pkt;
	struct sockaddr_in fromAddr;
	int fromLen = sizeof(fromAddr);
	
	while (true) {
		int bytesReceived = recvfrom(udpSocket, (char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&fromAddr, &fromLen);
        if (bytesReceived <= 0) break;
        if (bytesReceived != sizeof(pkt)) continue;
        fromLen=sizeof(fromAddr);
		
		if (pkt.type == PKT_DISCOVERY && isServer) {
			NetworkPacket reply;
			memset(&reply, 0, sizeof(reply));
			reply.type = PKT_SERVER_INFO;
			reply.senderID = currentServerNumber;
			sendto(udpSocket, (const char*)&reply, sizeof(reply), 0, (struct sockaddr*)&fromAddr, fromLen);
		}
		else if (pkt.type == PKT_SERVER_INFO && isClient) {
			char ipStr[32];
			inet_ntop(AF_INET, &(fromAddr.sin_addr), ipStr, 32);
			bool exists = false;
			for (int i = 0; i < discoveredServersCount; i++) {
				if (strcmp(discoveredServersIPs[i], ipStr) == 0) { exists = true; break; }
			}
			if (!exists && discoveredServersCount < 10) {
				strcpy(discoveredServersIPs[discoveredServersCount], ipStr);
				discoveredServerNums[discoveredServersCount] = pkt.senderID;
				discoveredServersCount++;
			}
		}
		else if (pkt.type == PKT_JOIN && isServer) {
			int assignedID = -1;
			// Проверяем, не подключен ли уже этот клиент (повторный пакет)
			for (int i = 1; i < MAX_NET_PLAYERS; i++) {
				if (clientAddrs[i].sin_addr.s_addr == fromAddr.sin_addr.s_addr &&
					clientAddrs[i].sin_port == fromAddr.sin_port) {
					assignedID = i;
					break;
				}
			}
			// Если новый — ищем свободный слот
			if (assignedID == -1) {
				for (int i = 1; i < MAX_NET_PLAYERS; i++) {
					if (clientAddrs[i].sin_addr.s_addr == 0) {
						assignedID = i;
						clientAddrs[i] = fromAddr;
						netPlayers[i].id = i;
						netPlayers[i].health = 100;
						netPlayers[i].isDead = false;
                        netPlayers[i].faceID = i;
                        netActive[i] = true;
                        lastHeard[i] = (float)GetTime();
                        char joined[96]; snprintf(joined, sizeof(joined), "Player %d joined", i+1);
                        AddChatLine(joined);
						connectedClients++;
						break;
					}
				}
			}
            if(assignedID != -1) lastHeard[assignedID]=(float)GetTime();
			if (assignedID != -1) {
				NetworkPacket ack;
				memset(&ack, 0, sizeof(ack));
				ack.type = PKT_JOIN;
				ack.senderID = assignedID;
				sendto(udpSocket, (const char*)&ack, sizeof(ack), 0, (struct sockaddr*)&fromAddr, fromLen);
			}
		}
		
		else if (pkt.type == PKT_JOIN && isClient) {
			myPlayerID = pkt.senderID;
			if (myPlayerID >= 0 && myPlayerID < MAX_NET_PLAYERS) {
				netPlayers[myPlayerID].id = myPlayerID;
				netPlayers[myPlayerID].health = 100;
				netPlayers[myPlayerID].faceID = myPlayerID;
                netActive[myPlayerID] = true;
                lastHeard[myPlayerID] = (float)GetTime();
                RespawnMe(); // spawn point matches newly assigned player slot
            }
		}
		else if (pkt.type == PKT_HIT) {
			if (pkt.targetID == myPlayerID && myPlayerID >= 0 && !netPlayers[myPlayerID].isDead) {
				netPlayers[myPlayerID].health -= pkt.damage;
				if (netPlayers[myPlayerID].health <= 0) {
                    OnMyDeath();
                } else if (sndHit.frameCount > 0) {
					PlaySound(sndHit);
				}
			}
			if (isServer) {
				for (int i = 1; i < MAX_NET_PLAYERS; i++) {
					if (i != pkt.senderID && clientAddrs[i].sin_addr.s_addr != 0) {
						sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
					}
				}
			}
		}
        else if (pkt.type == PKT_KILL) {
            if (pkt.senderID >= 0 && pkt.senderID < MAX_NET_PLAYERS &&
                pkt.targetID >= 0 && pkt.targetID < MAX_NET_PLAYERS) {
                AddKillLine(pkt.senderID, pkt.targetID);
                if (isServer) {
                    for (int i=1; i<MAX_NET_PLAYERS; ++i)
                        if (netActive[i] && i != pkt.senderID)
                            sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0,
                                   (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
                }
            }
        }
        else if (pkt.type == PKT_CHAT) {
            pkt.message[sizeof(pkt.message)-1] = '\0';
            if (pkt.senderID >= 0 && pkt.senderID < MAX_NET_PLAYERS && netActive[pkt.senderID]) {
                char line[192];
                snprintf(line, sizeof(line), "Player %d: %.128s", pkt.senderID+1, pkt.message);
                AddChatLine(line);
                if (isServer) {
                    for (int i=1; i<MAX_NET_PLAYERS; ++i)
                        if (netActive[i] && i != pkt.senderID)
                            sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0,
                                   (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
                }
            }
        }
        else if (pkt.type == PKT_DISCONNECT) {
            int id = pkt.senderID;
            if (id < 0 || id >= MAX_NET_PLAYERS) continue;
            if (isServer && id > 0) {
                if (!netActive[id] || clientAddrs[id].sin_addr.s_addr!=fromAddr.sin_addr.s_addr ||
                    clientAddrs[id].sin_port!=fromAddr.sin_port)continue;
                memset(&clientAddrs[id], 0, sizeof(clientAddrs[id]));
                if (connectedClients > 1) --connectedClients;
                for (int i=1; i<MAX_NET_PLAYERS; ++i)
                    if (netActive[i] && i != id)
                        sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0,
                               (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
            }
            if(isServer && id==0)continue;
            char line[96]; snprintf(line, sizeof(line), "Player %d left the server", id+1);
            AddChatLine(line);
            netActive[id] = false;
            memset(&netPlayers[id], 0, sizeof(netPlayers[id]));
            deathAnimPending[id] = false;
            if (isClient && id == 0) { ReturnToMainMenu(); return; }
        }
		else if (pkt.type == PKT_STATE_UPDATE) {
			if (pkt.senderID >= 0 && pkt.senderID < MAX_NET_PLAYERS && pkt.senderID != myPlayerID) {
                int id = pkt.senderID;
                if (isServer && (id == 0 || !netActive[id])) continue;
                if (!netActive[id]) {
                    char line[96]; snprintf(line, sizeof(line), "Player %d joined", id+1);
                    AddChatLine(line);
                }
                if (!netPlayers[id].isDead && pkt.playerData.isDead) {
                    deathAnimStart[id] = (float)GetTime();
                    deathAnimPending[id] = true;
                }
                netPlayers[id] = pkt.playerData;
                netPlayers[id].faceID = id; // each occupied slot has one unique expression
                netActive[id] = true;
                lastHeard[id] = (float)GetTime();
				if (isServer) {
					for (int i = 1; i < MAX_NET_PLAYERS; i++) {
						if (i != pkt.senderID && clientAddrs[i].sin_addr.s_addr != 0) {
							sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&clientAddrs[i], sizeof(clientAddrs[i]));
						}
					}
				}
			}
        }
    }
    // UDP has no reliable disconnect: drop stale clients after 8 seconds.
    if(isClient && myPlayerID>=0) {
        for(int i=0;i<MAX_NET_PLAYERS;++i) {
            if(i==myPlayerID || !netActive[i] || (float)GetTime()-lastHeard[i]<=8.0f)continue;
            char msg[96];snprintf(msg,sizeof(msg),"Player %d timed out",i+1);
            AddChatLine(msg);
            netActive[i]=false;
            memset(&netPlayers[i],0,sizeof(netPlayers[i]));
            deathAnimPending[i]=false;
            if(i==0) {ReturnToMainMenu();return;}
        }
    }
    if(isServer) for(int i=1;i<MAX_NET_PLAYERS;++i) {
        if(netActive[i] && (float)GetTime()-lastHeard[i]>8.0f) {
            NetworkPacket gone={0};gone.type=PKT_DISCONNECT;gone.senderID=i;
            netActive[i]=false;
            memset(&clientAddrs[i],0,sizeof(clientAddrs[i]));
            memset(&netPlayers[i],0,sizeof(netPlayers[i]));
            deathAnimPending[i]=false;
            if(connectedClients>1)--connectedClients;
            char msg[96];snprintf(msg,sizeof(msg),"Player %d timed out",i+1);AddChatLine(msg);
            for(int j=1;j<MAX_NET_PLAYERS;++j) if(netActive[j])
                sendto(udpSocket,(const char*)&gone,sizeof(gone),0,(struct sockaddr*)&clientAddrs[j],sizeof(clientAddrs[j]));
        }
    }
}


// Буферы для двухпроходного рендера (ВЫРАВНЕНЫ ПО 64 БАЙТАМ ДЛЯ OPENMP)
float fov     = 60.0f * (PI / 180.0f);
float depth   = 64.0f;


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
float moonLightBuf[SW];
int wallMapXBuf[SW],wallMapYBuf[SW];
unsigned char posterFlipBuf[SW];

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
//  Спрайт игрока
// ------------------------------------------------------------

#define TOFU_SIZE 64
Color tofuData[8][TOFU_SIZE][TOFU_SIZE]; // Массив для пиксельного рендера

// Eight deliberate, recognizably different expressions; index == player slot.
// No random face reassignments when a player sends position packets.
void GenerateTofuTextures(void) {
    const Color skins[8]={
        {236,216,190,255},{218,198,171,255},{233,213,185,255},{215,204,183,255},
        {239,215,182,255},{222,207,191,255},{229,201,159,255},{186,209,217,255}
    };
    for(int f=0;f<8;++f)for(int y=0;y<TOFU_SIZE;++y)for(int x=0;x<TOFU_SIZE;++x) {
        float xx=(x-31.5f)/29.0f, yy=(y-32.0f)/30.0f;
        float oval=xx*xx+yy*yy;
        Color c=skins[f];
        c.a=(oval<=1.0f)?255:0;
        if(!c.a){tofuData[f][y][x]=c;continue;}
        int shade=(int)((hash2(x+f*53,y+13)&7u));
        c.r-=shade;c.g-=shade;c.b-=shade;
        if(oval>0.86f)c=(Color){120,107,96,255}; // soft outline
        bool eyeL=(abs(x-21)<4 && abs(y-25)<4);
        bool eyeR=(abs(x-43)<4 && abs(y-25)<4);
        bool browL=(x>=15 && x<=26 && abs(y-(f==1?18+(x-15)/5:17))<2);
        bool browR=(x>=37 && x<=49 && abs(y-(f==1?21-(x-37)/5:17))<2);
        if(f==3) {eyeL=(x>=17&&x<=25&&y==26);eyeR=(x>=39&&x<=47&&y==26);}
        if(f==5)eyeR=(x>=39&&x<=47&&y==26); // wink
        if(f==7) {eyeL=(abs(x-21)<5&&abs(y-25)<5);eyeR=(abs(x-43)<5&&abs(y-25)<5);}
        if(eyeL||eyeR||browL||browR)c=(Color){26,27,29,255};
        // Contrasting eyes help the slot designs read even at a distance.
        if(f==7 && (eyeL||eyeR))c=(Color){75,224,238,255};
        if(f==4 && ((((abs(x-21)==8||abs(x-43)==8)&&y>18&&y<34) || (y==18&&x>12&&x<52))))
            c=(Color){29,31,31,255}; // glasses
        if(f==6 && ((x>=27&&x<=37&&y==35)||(abs(x-31)<2&&y==34)))c=(Color){160,91,78,255};
        // The mouth styles use unique, easily distinguishable silhouettes.
        if(f==0 && x>=19 && x<=45 && abs(y-(41+(int)(5.0f*(1.0f-fabsf((float)(x-32))/13.0f))))<=1)c=(Color){45,30,35,255}; // smile
        if(f==1 && x>=20 && x<=44 && abs(y-(48-(int)(5.0f*(1.0f-fabsf((float)(x-32))/12.0f))))<=1)c=(Color){40,30,30,255}; // angry frown
        if(f==2 && abs(x-32)<=7 && abs(y-46)<=9 && ((x-32)*(x-32)/49.0f+(y-46)*(y-46)/81.0f)<1.0f)c=(Color){48,28,35,255}; // surprised
        if(f==3 && x>=24&&x<=40&&y==45)c=(Color){56,42,40,255}; // sleepy
        if(f==4 && x>=22&&x<=43&&y>=43&&y<=47)c=(Color){68,37,31,255}; // teeth
        if(f==4 && x>=22&&x<=43&&y==45)c=WHITE;
        if(f==5 && x>=23&&x<=42&&abs(y-(41+(x-23)/6))<2)c=(Color){40,36,36,255}; // smirk
        if(f==6 && x>=20&&x<=44&&abs(y-(45+(int)(4.0f*(1-fabsf((float)(x-32))/12.0f))))<2)c=(Color){41,28,34,255};
        if(f==6 && x>=31&&x<=36&&y>=48&&y<=55)c=(Color){220,104,115,255}; // tongue
        if(f==7 && x>=20&&x<=44&&y>=43&&y<=48)c=(Color){48,66,72,255}; // robotic grille
        if(f==7 && y==45&&x%6==0&&x>=20&&x<=44)c=(Color){158,221,218,255};
        if(f==0 && ((abs(x-12)<2&&abs(y-34)<2)||(abs(x-52)<2&&abs(y-34)<2)))c=(Color){194,104,105,255};
        if(f==3 && x>=15&&x<=49&&y==12)c=(Color){61,59,64,255}; // hair
        tofuData[f][y][x]=c;
    }
}

static void LoadPosters(void) {
    memset(posterLoaded, 0, sizeof(posterLoaded));
    for (int i=0; i<MAX_POSTERS; ++i) {
        char path[128];
        snprintf(path,sizeof(path),"posters/%d.png",i+1);
        if (!FileExists(path)) snprintf(path,sizeof(path),"posters/%d.jpg",i+1);
        if (!FileExists(path)) snprintf(path,sizeof(path),"posters/%d.jpeg",i+1);
        if (!FileExists(path)) continue;
        Image im=LoadImage(path);
        if (!im.data || im.width<=0 || im.height<=0) {if(im.data)UnloadImage(im);continue;}
        // CONTAIN / LETTERBOX: preserve the ENTIRE photo and its aspect ratio.
        // Landscape images get bars top/bottom; portraits get bars left/right.
        float scale=fminf((float)(POSTER_RES-4)/im.width,(float)(POSTER_RES-4)/im.height);
        int iw=(int)floorf(im.width*scale+0.5f),ih=(int)floorf(im.height*scale+0.5f);
        if(iw<1)iw=1;
        if(ih<1)ih=1;
        int ox=(POSTER_RES-iw)/2,oy=(POSTER_RES-ih)/2;
        for(int y=0;y<POSTER_RES;++y)for(int x=0;x<POSTER_RES;++x){
            Color c={29,28,26,255}; // thin poster backing
            if(x>=ox && x<ox+iw && y>=oy && y<oy+ih) {
                int ix=(int)((x-ox+0.5f)*im.width/iw);
                int iy=(int)((y-oy+0.5f)*im.height/ih);
                if(ix>=im.width)ix=im.width-1;
                if(iy>=im.height)iy=im.height-1;
                c=GetImageColor(im,ix,iy);
            }
            posterData[i][y][x]=c;
        }
        posterLoaded[i]=true;
        UnloadImage(im);
    }
}

static void PlacePosters(void) {
    memset(posterAt,-1,sizeof(posterAt));
    int available[MAX_POSTERS],total=0;
    for(int i=0;i<MAX_POSTERS;++i)if(posterLoaded[i])available[total++]=i;
    if(total==0)return;
    for(int x=1;x<MW-1;++x) for(int y=1;y<MH-1;++y) {
        if(map[x][y].type!=CELL_WALL || (hash2(x,y)%31u)!=0u) continue;
        bool visible=(map[x-1][y].type==CELL_EMPTY || map[x+1][y].type==CELL_EMPTY ||
                      map[x][y-1].type==CELL_EMPTY || map[x][y+1].type==CELL_EMPTY);
        if(visible) posterAt[x][y]=(signed char)available[hash2(x+37,y+11)%(unsigned int)total];
    }
}

// ------------------------------------------------------------
//  ОБНОВЛЕННЫЕ ТЕКСТУРЫ СТЕН (256x256)
// ------------------------------------------------------------

// Текстура 0: Песочный кирпич (Крупный)
static void GenerateWall_SandBrick(Color tex[TS][TS]) {
	Color brickA   = (Color){205, 175, 130, 255};
	Color brickB   = (Color){190, 158, 112, 255};
	
	int brickH = 64;  // Размер кирпича по высоте
	int brickW = 128; // Размер кирпича по ширине
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			int row = y / brickH;
			int offset = (row % 2) * (brickW / 2);
			int bx = (x + offset) % brickW;
			int by = y % brickH;
			
			bool isMortar = (bx < 8 || by < 8); // Толстый шов
			
			if (isMortar) {
				float n = fbm(x * 0.1f, y * 0.1f, 2);
				unsigned char v = (unsigned char)(150 + n * 30);
				tex[y][x] = (Color){v, (unsigned char)(v * 0.82f), (unsigned char)(v * 0.58f), 255};
			} else {
				int variant = ((bx * 7 + by * 13 + row * 17) % 5);
				Color base = (variant < 3) ? brickA : brickB;
				float n = fbm(x * 0.15f + row * 5.0f, y * 0.15f, 2);
				float shade = 0.85f + n * 0.3f;
				
				if (by == 8) shade *= 0.92f;
				if (bx == 8) shade *= 0.95f;
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
			float n = fbm(x * 0.05f, y * 0.05f, 3); // Уменьшили частоту шума для 256
			float cracks = fbm(x * 0.1f, y * 0.1f, 2);
			
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

// Текстура 2: Каменная кладка (Крупные камни)
static void GenerateWall_Stone(Color tex[TS][TS]) {
	Color mortar = (Color){88, 78, 62, 255};
	Color stoneA = (Color){130, 118, 96, 255};
	Color stoneB = (Color){110, 98, 80, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			int row = y / 64; 
			int offset = (row % 2) * 64;
			int bx = (x + offset) % 128;
			int by = y % 64;
			
			bool isMortar = (bx < 8 || by < 8);
			
			if (isMortar) {
				tex[y][x] = mortar;
			} else {
				int variant = (hash2(x, y + row * 17) & 3);
				Color base = (variant < 2) ? stoneA : stoneB;
				float n = fbm(x * 0.1f + row * 3.0f, y * 0.1f, 2);
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

// Текстура 3: Деревянный ящик
static void GenerateWall_WoodCrate(Color tex[TS][TS]) {
	Color plankLight = (Color){168, 122, 72, 255};
	Color plankDark  = (Color){120, 82, 45, 255};
	Color edgeDark   = (Color){70, 45, 22, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			bool isEdge = (x < 16 || x >= TS - 16 || y < 16 || y >= TS - 16);
			bool isBrace = (abs((x - TS / 2) - (y - TS / 2)) < 16) ||
			(abs((x - TS / 2) + (y - TS / 2) - TS / 2) < 16);
			
			if (isEdge) {
				tex[y][x] = edgeDark;
			} else if (isBrace) {
				float n = fbm(x * 0.1f, y * 0.1f, 2);
				Color c = MixColor(plankDark, edgeDark, 0.4f);
				c.r = (unsigned char)(c.r * (0.85f + n * 0.3f));
				c.g = (unsigned char)(c.g * (0.85f + n * 0.3f));
				c.b = (unsigned char)(c.b * (0.85f + n * 0.3f));
				tex[y][x] = c;
			} else {
				int plank = y / 64; 
				Color base = (plank % 2 == 0) ? plankLight : plankDark;
				float grain = sinf((float)x * 0.1f + fbm(x * 0.05f, y * 0.1f, 2) * 6.0f) * 0.5f + 0.5f;
				float shade = 0.8f + grain * 0.4f;
				
				// Заклепки (гвозди) в углах
				if (x >= 32 && x <= 48 && y >= 32 && y <= 48) shade *= 0.5f;
				if (x >= TS-49 && x <= TS-33 && y >= 32 && y <= 48) shade *= 0.5f;
				if (x >= 32 && x <= 48 && y >= TS-49 && y <= TS-33) shade *= 0.5f;
				if (x >= TS-49 && x <= TS-33 && y >= TS-49 && y <= TS-33) shade *= 0.5f;
				
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
			bool isFrame = (x < 16 || x >= TS - 16 || y < 16 || y >= TS - 16);
			
			if (isFrame) {
				tex[y][x] = metalDark;
			} else {
				float band = ((x / 32) % 2 == 0) ? 1.05f : 0.9f; 
				float n = fbm(x * 0.1f, y * 0.1f, 2);
				float shade = band * (0.8f + n * 0.3f);
				Color c = MixColor(metalDark, metalLight, n);
				
				float rustNoise = fbm(x * 0.05f + 10.0f, y * 0.05f + 10.0f, 3);
				if (rustNoise > 0.65f) {
					c = MixColor(c, rust, (rustNoise - 0.65f) * 1.8f);
				}
				
				// Замочная скважина
				if (x >= TS/2 - 24 && x <= TS/2 + 24 && y >= TS/2 - 16 && y <= TS/2 + 16) {
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
			
			// Обручи бочки
			if (y % 64 == 0 || y % 64 == 4) { 
				c = MixColor(c, (Color){40, 40, 45, 255}, 0.7f);
			}
			
			float n = fbm(x * 0.05f + 20.0f, y * 0.05f + 20.0f, 3);
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
			float n = fbm(x * 0.05f + 5.0f, y * 0.05f + 5.0f, 3);
			Color c = MixColor(dark, base, n);
			
			float dirt = fbm(x * 0.02f, y * 0.02f, 2);
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
	
	for (int y = 80; y < 176; y++) {
		for (int x = 64; x < 192; x++) {
			if ((x + y) % 24 == 0 && x > 80 && x < 176 && y > 96 && y < 160) {
				tex[y][x] = (Color){140, 45, 35, 255};
			}
		}
	}
}

// ------------------------------------------------------------
//  ОБНОВЛЕННЫЕ ТЕКСТУРЫ ПОЛОВ (256x256)
// ------------------------------------------------------------

// Пол 0: Песок (улицы)
static void GenerateFloor_Sand(Color tex[TS][TS]) {
	Color sandLight = (Color){200, 175, 130, 255};
	Color sandDark  = (Color){160, 135, 95, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			float n = fbm(x * 0.05f, y * 0.05f, 3);
			Color c = MixColor(sandDark, sandLight, n);
			
			if ((hash2(x, y) & 0x7F) < 6) {
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
			bool isGrout = (x % 128 == 0 || y % 128 == 0); 
			
			if (isGrout) {
				tex[y][x] = grout;
			} else {
				int tileX = x / 128;
				int tileY = y / 128;
				float variance = ((hash2(tileX, tileY) & 0xFF) / 255.0f - 0.5f) * 0.2f;
				
				float n = fbm(x * 0.1f, y * 0.1f, 2);
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
			bool isGrout = (x % 64 == 0 || y % 64 == 0); 
			
			if (isGrout) {
				tex[y][x] = grout;
			} else {
				int tx = x / 64;
				int ty = y / 64;
				float v = ((hash2(tx + 100, ty + 100) & 0xFF) / 255.0f);
				float n = fbm(x * 0.15f, y * 0.15f, 2);
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
			float n = fbm(x * 0.2f, y * 0.2f, 3);
			Color c = base;
			
			if ((hash2(x, y) & 0x3F) < 12) { 
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

// Пол 4: Деревянный пол (внутри зданий)
static void GenerateFloor_Wood(Color tex[TS][TS]) {
	Color wood = (Color){145, 100, 60, 255};
	Color woodDark = (Color){100, 68, 40, 255};
	
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			int plank = y / 32; 
			Color base = (plank % 2 == 0) ? wood : woodDark;
			
			float grain = sinf(x * 0.1f + fbm(x * 0.05f, y * 0.15f, 2) * 5.0f) * 0.5f + 0.5f;
			float shade = 0.85f + grain * 0.3f;
			
			if (y % 32 == 0) shade *= 0.5f; 
			
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
	
	for (int i = 0; i < 160; i++) { 
		unsigned int h = hash2(i * 17, i * 31);
		int cx = (h >> 0) & 0xFF; 
		int cy = (h >> 8) & 0xFF;
		int r  = 8 + ((h >> 16) & 7); 
		
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
			float n = fbm(x * 0.1f, y * 0.1f, 3);
			Color c = MixColor(dirt, dirtLight, n);
			
			float dark = fbm(x * 0.05f + 30.0f, y * 0.05f, 2);
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
			bool isGrout = (x % 128 == 0 || y % 128 == 0); 
			if (isGrout) {
				tex[y][x] = grout;
			} else {
				float n = fbm(x * 0.1f, y * 0.1f, 2);
				tex[y][x] = MixColor(tileDark, tile, n);
			}
		}
	}
}

// Собираем все текстуры в кучу
static void ApplyDirt(Color tex[TS][TS], float intensity, Color dirtColor) {
	for (int y = 0; y < TS; y++) {
		for (int x = 0; x < TS; x++) {
			// Частоту шума уменьшили для 256x256
			float n = fbm(x * 0.05f + 50.0f, y * 0.05f + 50.0f, 3);
			if (n > 0.5f) {
				float t = (n - 0.5f) * 2.0f * intensity;
				if (t > 1.0f) t = 1.0f;
				tex[y][x] = MixColor(tex[y][x], dirtColor, t);
			}
			
			int noise = (hash2(x, y) % 20) - 10; 
			int r = (int)tex[y][x].r + noise;
			int g = (int)tex[y][x].g + noise;
			int b = (int)tex[y][x].b + noise;
			
			// Увеличиваем контраст (тени становятся чернее, света ярче)
			float contrast = 1.25f;
			r = (int)((r - 128) * contrast + 128);
			g = (int)((g - 128) * contrast + 128);
			b = (int)((b - 128) * contrast + 128);
			
			if (r < 0) { r = 0; } if (r > 255) { r = 255; }
			if (g < 0) { g = 0; } if (g > 255) { g = 255; }
			if (b < 0) { b = 0; } if (b > 255) { b = 255; }

			tex[y][x].r = (unsigned char)r;
			tex[y][x].g = (unsigned char)g;
			tex[y][x].b = (unsigned char)b;
		}
	}
}
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

		// ПРОВЕРКА ПОПАДАНИЯ В ИГРОКА
		for (int i = 0; i < MAX_NET_PLAYERS; i++) {
			if (i == myPlayerID || !netActive[i] || netPlayers[i].isDead) continue;
			
			// Координаты игрока
			float dxP = netPlayers[i].x - startX;
			float dyP = netPlayers[i].y - startY;
			
			// Проекция на луч (расстояние от камеры до игрока)
			float projDistP = dxP * dirX + dyP * dirY;
			
			// Если игрок впереди и ближе, чем стена (perpDist)
			if (projDistP > 0 && projDistP < perpDist) {
				// Расстояние от луча до центра игрока (перпендикуляр)
				float sideDistP = fabsf(dxP * (-dirY) + dyP * dirX);
				
				// Если луч прошел близко к центру (радиус Тофу ~0.4f)
				if (sideDistP < PLAYER_RADIUS &&
                    startZ + dirZ*projDistP > netPlayers[i].z &&
                    startZ + dirZ*projDistP < netPlayers[i].z + PLAYER_HEIGHT) {
                    netPlayers[i].health -= 25;
                    if (netPlayers[i].health <= 0) {
                        netPlayers[i].health = 0;
                        netPlayers[i].isDead = true;
                        netPlayers[myPlayerID].kills++;
                        deathAnimPending[i] = true;
                        deathAnimStart[i] = (float)GetTime();
                        AddKillLine(myPlayerID, i);
                        SendEvent(PKT_KILL, i, 0);
                    } else if (sndHit.frameCount > 0) PlaySound(sndHit);
                    SendEvent(PKT_HIT, i, 25);
                    return; // Пуля попала в игрока, дальше не летит
				}
			}
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

static void SpawnDroppedMagazine(void) {
    float rightX=-sinf(player.pa), rightY=cosf(player.pa);
    for(int i=0;i<MAX_SHELLS;++i) if(!shells[i].active) {
        Shell *a=&shells[i];
        a->active=true; a->isMagazine=true;
        a->x=player.x+cosf(player.pa)*0.28f+rightX*0.18f;
        a->y=player.y+sinf(player.pa)*0.28f+rightY*0.18f;
        a->z=player.z+EYE_HEIGHT-0.18f;
        a->vx=cosf(player.pa)*0.45f+rightX*0.7f;
        a->vy=sinf(player.pa)*0.45f+rightY*0.7f;
        a->vz=0.7f; a->life=9.0f;
        a->rotation=0.15f; a->spinSpeed=5.8f;
        break;
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
            shells[i].isMagazine = false;
            shells[i].rotation=0.0f; shells[i].spinSpeed=7.0f;
			break;
		}
	}
}

	void UpdateShells(float dt) {
	for (int i = 0; i < MAX_SHELLS; i++) {
		if (!shells[i].active) continue;
		
        shells[i].life -= dt;
        shells[i].rotation += shells[i].spinSpeed*dt;
        if (shells[i].life <= 0) { shells[i].active = false; continue; }
		
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
                    shells[i].spinSpeed *= 0.30f;
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
	    // Fixed crosshair and direct mouse look (CS-style instead of floating crosshair).
    Vector2 md=GetMouseDelta();
    player.crosshairX=0.0f;
    player.crosshairY=0.0f;
    player.aimSpreadX=0.0f;
    player.aimSpreadY=0.0f;
    float mouseScale=0.0025f*mouseSensitivity*(virtualMouseDPI/800.0f);
    player.pa+=md.x*mouseScale;
    player.pitch-=md.y*mouseScale;
    player.pitch=fmaxf(-MAX_PITCH,fminf(MAX_PITCH,player.pitch));
    if(player.pa<0.0f)player.pa+=2.0f*PI;
    if(player.pa>=2.0f*PI)player.pa-=2.0f*PI;

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
		
		// Логика скорости
		if (isFullyExhausted) {
			maxSpeed *= 0.55f; // Медленная ходьба при истощении
		} else if (wantsToSprint && isMoving && player.stamina > 0.0f) {
			isSprinting = true;
			maxSpeed *= 1.6f; 
			player.stamina -= 10.0f * dt; 
			if (player.stamina < 0.0f) player.stamina = 0.0f;
		}
		
		// Восстановление стамины
		if (!isSprinting) {
			float regenRate = isFullyExhausted ? 10.0f : 15.0f;
			player.stamina += regenRate * dt;
			if (player.stamina > player.maxStamina) player.stamina = player.maxStamina;
		}
		
		if (isSprinting || player.stamina < player.maxStamina) {
			stamina_ui_timer = 2.0f;
		}
		
		// --- ЗВУКИ СТАМИНЫ И ВЫХОД ИЗ ИСТОЩЕНИЯ ---
		// 1. Упали в ноль
		if (player.stamina <= 0.1f && !isFullyExhausted) {
			if (sndStaminaOut.frameCount > 0) PlaySound(sndStaminaOut);
			isFullyExhausted = true; 
		}
		
		// 2. Восстановились (например, до 40%, чтобы снова можно было бегать)
		if (player.stamina >= player.maxStamina * 0.4f && isFullyExhausted) {
			if (sndStaminaBack.frameCount > 0) PlaySound(sndStaminaBack);
			isFullyExhausted = false; 
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
	
		if (IsKeyDown(KEY_SPACE) && player.onGround && !isFullyExhausted && player.stamina >= 15.0f) {
			player.vz = JUMP_SPEED * 1.15f; 
			player.onGround = false; 
			player.stamina -= 15.0f;
			stamina_ui_timer = 2.0f;
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
	if (blockFireUntilRelease) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsKeyDown(KEY_LEFT_CONTROL))
            blockFireUntilRelease=false;
    } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsKeyDown(KEY_LEFT_CONTROL)) {
        FireWeapon();
    }
	
	// --- ПЕРЕЗАРЯДКА ---
	if (IsKeyPressed(KEY_R) && !pistol.isReloading && pistol.ammo < pistol.maxAmmo) {
		pistol.isReloading = true;
        pistol.reloadTimer = pistol.reloadTime;
        SpawnDroppedMagazine();
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
	
}

// ------------------------------------------------------------
//  СЕКТОР 11: РЕНДЕР (ПОЛНОСТЬЮ ИСПРАВЛЕННЫЙ, МОНОЛИТНЫЙ)
// ------------------------------------------------------------

// Stylized non-graphic burst: polygon-like fragments fly from the player model.
static void RenderDeathBurst(Color *pixels, const NetworkPlayer *np,int slot,
                             float camX,float camY,float camZ,float dirX,float dirY,
                             float planeLen,float projPlane,float horizon) {
    float elapsed=(float)GetTime()-deathAnimStart[slot];
    if(elapsed<0.0f || elapsed>1.45f){deathAnimPending[slot]=false;return;}
    float life=1.0f-elapsed/1.45f;
    float spread=0.2f+elapsed*2.1f;
    for(int k=0;k<34;++k) {
        unsigned int h=hash2((unsigned int)(slot*47+k*17+3),(unsigned int)(k*71+23));
        float angle=((h&1023u)/1023.0f)*2.0f*PI;
        float velocity=0.35f+((h>>10)&255u)/255.0f*0.75f;
        float ox=cosf(angle)*spread*velocity,oy=sinf(angle)*spread*velocity;
        float dz=0.33f+((h>>18)&255u)/255.0f*0.9f + elapsed*(0.25f+((k*37)%13)*0.07f)-2.7f*elapsed*elapsed;
        float wx=np->x+ox,wy=np->y+oy,wz=np->z+dz;
        float dx=wx-camX,dy=wy-camY;
        float forward=dx*dirX+dy*dirY;
        if(forward<0.12f || forward>depth)continue;
        float side=dx*(-dirY)+dy*dirX;
        int cx=(int)((side/(forward*planeLen)+1.0f)*SW*0.5f);
        int cy=(int)(horizon-(wz-camZ)*projPlane/forward);
        float physical=(k%4==0?0.13f:0.075f)*life;
        int size=(int)fmaxf(1.0f,fminf(28.0f,physical*projPlane/forward));
        Color fragment;
        switch(k%5){
            case 0:fragment=(Color){224,211,176,255};break;
            case 1:fragment=(Color){171,172,161,255};break;
            case 2:fragment=(Color){84,107,116,255};break;
            case 3:fragment=(Color){235,175,97,255};break;
            default:fragment=(Color){209,222,220,255};break;
        }
        float a=fminf(1.0f,life*1.7f)*(0.75f+0.25f*sinf(k*4.0f));
        for(int yy=cy-size/2;yy<=cy+size/2;++yy) {
            if(yy<0||yy>=SH)continue;
            for(int xx=cx-size/2;xx<=cx+size/2;++xx) {
                if(xx<0||xx>=SW||forward>=zbuf[xx]-0.02f)continue;
                // Small facets rather than a flat rectangular explosion.
                if(abs(xx-cx)+abs(yy-cy)>size)continue;
                int at=yy*SW+xx;
                pixels[at]=MixColor(pixels[at],fragment,a);
            }
        }
    }
}

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
	
// ===== ПРОХОД 1: DDA (СТЕНЫ + ПРЕДРАСЧЁТ НЕБА) =====
#pragma omp parallel for num_threads(cpu_threads) schedule(static)
	for (int x = 0; x < SW; x++) {
		// 1. Предрасчёт координаты неба один раз на столбец
		float rawCamXc = 2.0f * ((float)x / (float)SW) - 1.0f;
		float normAngle = fmodf(player.pa + rawCamXc * (fov * 0.5f), 2.0f * PI);
		if (normAngle < 0.0f) normAngle += 2.0f * PI;
		int sX = (int)((normAngle / (2.0f * PI)) * (float)SKY_W);
		if (sX < 0) sX = 0; else if (sX >= SKY_W) sX = SKY_W - 1;
		skyXBuf[x] = sX;
		
		// 2. DDA для стен
		float camXc = rawCamXc + bobH / (float)SW;
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
        wallMapXBuf[x]=mapX;
        wallMapYBuf[x]=mapY;
        // U runs left to right as perceived from every visible wall face.
        posterFlipBuf[x]=(unsigned char)((side==0 && rayDirX<0.0f) || (side==1 && rayDirY>0.0f));
		
		float wallFloorZ = Z_FLOOR, wallCeilZ = Z_CEIL; int wallTexID = 0;
		float lightMultiplier = 1.0f;
		if (side == 0) lightMultiplier = (rayDirX > 0) ? 0.65f : 1.0f;
		else           lightMultiplier = (rayDirY > 0) ? 0.65f : 1.0f;
        lightBuf[x] = lightMultiplier;
        // The lit side of a wall receives weak cool moonlight.
        float normalX=(side==0)?-stepX:0.0f;
        float normalY=(side==1)?-stepY:0.0f;
        float moonDot=normalX*cosf(2.5f)+normalY*sinf(2.5f);
        moonLightBuf[x] = fmaxf(0.0f, moonDot) * 0.16f;
		
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
	
	// ===== ПРОХОД 2: РАСТЕРИЗАЦИЯ В ОДИН ПРОХОД ПО X (БЕЗ ПЕРЕКРЫТИЯ НЕБА) =====
	float radiusSq = (float)(moonR * moonR);
	float glowRadiusSq = radiusSq * 2.5f;
	float invMoonR = 1.0f / (float)moonR;
	
#pragma omp parallel for num_threads(cpu_threads) schedule(dynamic, 8)
	for (int y = 0; y < SH; y++) {
		Color *row = pixels + y * SW;
		int skyY = (int)((float)y - horizon + SKY_H * 0.5f);
		if (skyY < 0) { skyY = 0; }
		if (skyY >= SKY_H) { skyY = SKY_H - 1; }
		const Color *skyRow = skyTex[skyY];
		
		int dyMoon = y - moonY;
		float dyMoonSq = (float)(dyMoon * dyMoon);
		bool moonNearY = (dyMoonSq < glowRadiusSq);
		
		// Предрасчёт дистанции пола для всей строки Y (одно деление вместо 1080 делений!)
		float p = (float)y - horizon;
		bool hasFloor = (p > 0.0f);
		float rd = 0.0f, fshade = 0.2f;
		if (hasFloor) {
			rd = (camZ - Z_FLOOR) * projPlane / p;
			if (rd > depth) rd = depth;
			fshade = 1.0f - (rd / depth) * 0.85f;
			if (fshade < 0.2f) fshade = 0.2f;
		}
		
		for (int x = 0; x < SW; x++) {
			int ds = drawStartBuf[x], de = drawEndBuf[x];
			
			if (y < ds) {
				// --- 1. НЕБО И ЛУНА (рисуем только выше стены!) ---
				int skyX = skyXBuf[x];
				Color c = skyRow[skyX];
				
				if (moonNearY) {
					int dx = x - moonX;
					float distSq = (float)(dx * dx) + dyMoonSq;
					if (distSq < radiusSq) {
						float nx = (float)dx * invMoonR;
						float ny = (float)dyMoon * invMoonR;
						float shading = 1.0f - sqrtf(nx * nx + ny * ny) * 0.3f;
						float craterNoise = fbm(nx * 3.0f + 10.0f, ny * 3.0f + 10.0f, 2);
						float craterDark = (craterNoise > 0.45f && craterNoise < 0.6f) ? 0.25f : 0.1f;
						float mul = shading * (1.0f - craterDark);
						c = (Color){ (unsigned char)(240 * mul), (unsigned char)(240 * mul), (unsigned char)(220 * shading * (1.0f - craterDark * 0.8f)), 255 };
					} else if (distSq < glowRadiusSq) {
						float glow = 1.0f - (sqrtf(distSq) - (float)moonR) / ((float)moonR * 1.5f);
						if (glow > 0.0f) {
							float g4 = glow * 0.4f;
							c.r = (unsigned char)(c.r + (100 - c.r) * g4);
							c.g = (unsigned char)(c.g + (120 - c.g) * g4);
							c.b = (unsigned char)(c.b + (160 - c.b) * g4);
						}
					}
				}
				row[x] = c;
			} 
			else if (y < de) {
				// --- 2. СТЕНА ---
				int lineHeight = rawEndBuf[x] - rawStartBuf[x];
				if (lineHeight < 1) lineHeight = 1;
				int ty = (int)(((float)(y - rawStartBuf[x]) / (float)lineHeight) * TS);
				if (ty < 0) ty = 0; else if (ty >= TS) ty = TS - 1;
				
				Color c = wallTex[texIDBuf[x]][ty][txBuf[x]];
				float shade = 1.0f - (perpBuf[x] / depth) * 0.85f;
				if (shade < 0.2f) shade = 0.2f;
                shade *= lightBuf[x];
                if (sideBuf[x] == 1) shade *= 0.75f;
                // Poster is rendered as a decal on the real wall face, not a free-floating sprite.
                int wcX=wallMapXBuf[x];
                int wcY=wallMapYBuf[x];
                if (wcX>=0 && wcX<MW && wcY>=0 && wcY<MH) {
                    int pi=(int)posterAt[wcX][wcY];
                    if(pi>=0 && pi<MAX_POSTERS && posterLoaded[pi]) {
                        float u=(posterFlipBuf[x] ? TS-1-txBuf[x] : txBuf[x])/(float)(TS-1);
                        float worldZ=camZ+(horizon-y)*perpBuf[x]/projPlane;
                        float z0=map[wcX][wcY].floorZ+0.9f;
                        if(u>=0.14f && u<0.86f && worldZ>=z0 && worldZ<z0+0.72f) {
                            int pu=(int)((u-0.14f)/0.72f*POSTER_RES);
                            int pv=(int)((z0+0.72f-worldZ)/0.72f*POSTER_RES);
                            if(pu>=0 && pu<POSTER_RES && pv>=0 && pv<POSTER_RES) {
                                c=posterData[pi][pv][pu];
                                if(pu<2 || pv<2 || pu>=POSTER_RES-2 || pv>=POSTER_RES-2)
                                    c=(Color){26,25,24,255};
                            }
                        }
                    }
                }
                float moon=moonLightBuf[x];
                row[x]=(Color){(unsigned char)fminf(255.0f,c.r*shade*(1.0f+moon*0.60f)),
                                (unsigned char)fminf(255.0f,c.g*shade*(1.0f+moon*0.80f)),
                                (unsigned char)fminf(255.0f,c.b*shade*(1.0f+moon*1.55f)),255};
			} 
			else if (hasFloor) {
				// --- 3. ПОЛ (без деления внутри цикла по X) ---
				float camXc = 2.0f * ((float)x / (float)SW) - 1.0f;
				float floorX = camX + (dirX + planeX * camXc) * rd;
				float floorY = camY + (dirY + planeY * camXc) * rd;
				
				int cellX = (int)floorf(floorX), cellY = (int)floorf(floorY);
				int ftex = 0;
				if (cellX >= 0 && cellX < MW && cellY >= 0 && cellY < MH) {
					ftex = map[cellX][cellY].floorTex;
					if (ftex >= N_FLOOR_TEX) ftex = 0;
				}
				int ftx = (int)((floorX - (float)cellX) * (float)TS) & (TS - 1);
				int fty = (int)((floorY - (float)cellY) * (float)TS) & (TS - 1);
				
                Color c=floorTex[ftex][fty][ftx];
                // Disguise the harsh floor texture seam directly below walls/crates.
                if(cellX>0 && cellY>0 && cellX<MW-1 && cellY<MH-1) {
                    int tx=-1;
                    float fu=floorX-cellX, fv=floorY-cellY;
                    float nearest=1.0f;
                    if(map[cellX-1][cellY].type==CELL_WALL && fu<nearest){nearest=fu;tx=map[cellX-1][cellY].wallTex;}
                    if(map[cellX+1][cellY].type==CELL_WALL && 1.0f-fu<nearest){nearest=1.0f-fu;tx=map[cellX+1][cellY].wallTex;}
                    if(map[cellX][cellY-1].type==CELL_WALL && fv<nearest){nearest=fv;tx=map[cellX][cellY-1].wallTex;}
                    if(map[cellX][cellY+1].type==CELL_WALL && 1.0f-fv<nearest){nearest=1.0f-fv;tx=map[cellX][cellY+1].wallTex;}
                    if(tx>=0 && tx<N_WALL_TEX && nearest<0.12f) {
                        Color base=wallTex[tx][TS-12][(int)((fu+fv)*127.0f)&(TS-1)];
                        c=MixColor(c,base,(0.12f-nearest)/0.12f*0.75f);
                    }
                }
                row[x]=(Color){(unsigned char)fminf(255.0f,c.r*fshade*1.03f),
                                (unsigned char)fminf(255.0f,c.g*fshade*1.04f),
                                (unsigned char)fminf(255.0f,c.b*fshade*1.10f),255};
			}
		}
	}
	
// ===== ПРОХОД 3: ПЛАВНОЕ ЗАТУХАНИЕ СПРАЙТОВ (НЕЛИНЕЙНЫЙ МАСШТАБ + ALPHA FADE) =====
	
// 3. Other players are depth-tested cylinders with an expression on the facing side.
    for(int i=0;i<MAX_NET_PLAYERS;++i) {
        if(i==myPlayerID || !netActive[i]) continue;
        NetworkPlayer *np=&netPlayers[i];
        if(np->isDead) {
            if(deathAnimPending[i])
                RenderDeathBurst(pixels,np,i,camX,camY,camZ,dirX,dirY,planeLen,projPlane,horizon);
            continue; // no underground sinking, model bursts apart
        }
        float dx=np->x-camX,dy=np->y-camY;
        float forward=dx*dirX+dy*dirY;
        float side=dx*(-dirY)+dy*dirX;
        if(forward<=0.15f || forward>depth+1.0f)continue;
        float cx=(side/(forward*planeLen)+1.0f)*SW*0.5f;
        float width=(PLAYER_RADIUS/forward)*projPlane*2.4f;
        int xl=(int)floorf(cx-width),xr=(int)ceilf(cx+width);
        if(xl<0)xl=0;
        if(xr>=SW)xr=SW-1;
        for(int sx=xl;sx<=xr;++sx) {
            float cameraColumn=2.0f*sx/(float)SW-1.0f;
            float rx=dirX+planeX*cameraColumn, ry=dirY+planeY*cameraColumn;
            float ox=camX-np->x,oy=camY-np->y;
            float a=rx*rx+ry*ry,b=2.0f*(ox*rx+oy*ry);
            float d=b*b-4.0f*a*(ox*ox+oy*oy-PLAYER_RADIUS*PLAYER_RADIUS);
            if(d<0.0f) continue;
            float hit=(-b-sqrtf(d))/(2.0f*a);
            if(hit<0.08f || hit>depth || hit>=zbuf[sx]-0.02f)continue;
            float hitx=camX+rx*hit-np->x,hity=camY+ry*hit-np->y;
            float normalX=hitx/PLAYER_RADIUS,normalY=hity/PLAYER_RADIUS;
            int top=(int)(horizon-((np->z+PLAYER_HEIGHT)-camZ)/hit*projPlane);
            int bottom=(int)(horizon-((np->z)-camZ)/hit*projPlane);
            if(top>=SH || bottom<0 || top>=bottom)continue;
            if(top<0)top=0;
            if(bottom>=SH)bottom=SH-1;
            float faceForward=cosf(np->pa)*normalX+sinf(np->pa)*normalY;
            float faceSide=-sinf(np->pa)*normalX+cosf(np->pa)*normalY;
            float shade=fmaxf(0.22f,1.0f-hit/depth*0.80f);
            shade*=0.72f+0.28f*fmaxf(0.0f,normalX*cosf(2.5f)+normalY*sinf(2.5f));
            for(int sy=top;sy<=bottom;++sy) {
                float worldZ=camZ+(horizon-sy)*hit/projPlane;
                float v=(np->z+PLAYER_HEIGHT-worldZ)/PLAYER_HEIGHT;
                Color c={183,180,163,255};
                // The face decal only appears in the direction the player looks.
                if(faceForward>0.70f && fabsf(faceSide)<0.68f && v>0.16f && v<0.82f) {
                    float u=0.5f+faceSide/1.36f;
                    float fv=(v-0.16f)/0.66f;
                    int tx=(int)(u*TOFU_SIZE),ty=(int)(fv*TOFU_SIZE);
                    if(tx>=0&&tx<TOFU_SIZE&&ty>=0&&ty<TOFU_SIZE) {
                        Color decal=tofuData[((unsigned int)np->faceID)%8][ty][tx];
                        if(decal.a) c=decal;
                    }
                }
                pixels[sy*SW+sx]=(Color){(unsigned char)(c.r*shade),
                                            (unsigned char)(c.g*shade),
                                            (unsigned char)(c.b*shade),255};
            }
        }
    }

    // Occasional falling stars, drawn only in uncovered sky pixels.
    if(showMeteors) {
        float tm=(float)GetTime();
        for(int m=0;m<3;++m){
            float phase=fmodf(tm+m*3.79f,11.0f);
            if(phase>0.9f)continue;
            float baseAngle=0.55f+m*1.93f;
            float angle=baseAngle-player.pa;
            while(angle>PI)angle-=2*PI;
            while(angle< -PI)angle+=2*PI;
            if(fabsf(angle)>fov*0.58f)continue;
            int mx=(int)(SW*0.5f+tanf(angle)*(SW*0.5f)/tanf(fov*0.5f)+phase*120.0f);
            int my=(int)(horizon-200.0f+phase*165.0f);
            for(int q=0;q<25;++q){
                int xx=mx-q, yy=my-q/3;
                if(xx<0||xx>=SW||yy<0||yy>=SH||yy>=drawStartBuf[xx])continue;
                float alpha=(25-q)/25.0f;
                pixels[yy*SW+xx]=MixColor(pixels[yy*SW+xx],(Color){190,215,255,255},alpha);
            }
        }
    }

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
		if (wallDist >= depth) continue;
		
        // Physically projected bullet impact: shrinks proportionally to 1/distance.
        int radius=(int)roundf(0.007f*projPlane/fmaxf(0.15f,wallDist));
        if(radius<1)radius=1;
        if(radius>30)radius=30;

		// Плавный Fade Out (прозрачность) на дистанции от 75 до 100 метров (smoothstep)
		float alpha = 1.0f;
		if (wallDist > depth*0.78f) {
            float ft = (depth - wallDist) / (depth*0.22f);
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
		if (projDist < 0.1f || projDist >= depth) continue;
		
		float sideDist = dx * (-dirY) + dy * dirX;
		float camXc = sideDist / (projDist * planeLen);
		
		float screenX = (camXc + 1.0f) * ((float)SW * 0.5f) - (bobH * 0.5f);
		int px = (int)screenX;
		
		if (px < 0 || px >= SW) continue;
		if (projDist > zbuf[px] + 0.1f) continue;
		
        // True perspective sizes in world units. No constant-size sprites at a distance.
        float itemW=shells[i].isMagazine?0.085f:0.023f;
        float itemH=shells[i].isMagazine?0.185f:0.014f;
        int w=(int)fmaxf(1.0f,roundf(itemW*projPlane/fmaxf(0.12f,projDist)));
        int h=(int)fmaxf(1.0f,roundf(itemH*projPlane/fmaxf(0.12f,projDist)));
        if(w>SW)w=SW;
        if(h>SH)h=SH;

		// Плавный Fade Out от 75 до 100 метров + затухание в конце жизни гильзы (последняя 1.0 секунда)
		float alpha = 1.0f;
		if (projDist > depth*0.78f) {
            float ft = (depth - projDist) / (depth*0.22f);
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
        if (shells[i].isMagazine) {
            col_rim=(Color){(unsigned char)(65*distShade),(unsigned char)(69*distShade),(unsigned char)(78*distShade),255};
            col_body=(Color){(unsigned char)(135*distShade),(unsigned char)(143*distShade),(unsigned char)(154*distShade),255};
            col_shine=(Color){(unsigned char)(180*distShade),(unsigned char)(185*distShade),(unsigned char)(194*distShade),255};
            col_shade=(Color){(unsigned char)(94*distShade),(unsigned char)(100*distShade),(unsigned char)(113*distShade),255};
        }
		
		int py = (int)((float)horizon - (shells[i].z - camZ) / projDist * projPlane);
		
        if(shells[i].isMagazine) {
            // Rotating, bevelled steel magazine: upper feed lips, body seam and grip ridges.
            float rot=shells[i].rotation;
            float cs=cosf(rot),sn=sinf(rot);
            int bound=(int)(sqrtf((float)(w*w+h*h))*0.55f+3.0f);
            for(int sy=py-bound;sy<=py+bound;++sy) {
                if(sy<0||sy>=SH)continue;
                for(int sx=px-bound;sx<=px+bound;++sx) {
                    if(sx<0||sx>=SW||projDist>=zbuf[sx]-0.05f)continue;
                    float xx=(sx-px)*cs+(sy-py)*sn;
                    float yy=-(sx-px)*sn+(sy-py)*cs;
                    float u=xx/fmaxf(1.0f,w*.5f),v=yy/fmaxf(1.0f,h*.5f);
                    if(fabsf(u)>1.0f||fabsf(v)>1.0f)continue;
                    float edge=fabsf(u);
                    Color steel=(Color){(unsigned char)(118*distShade),
                                         (unsigned char)(128*distShade),
                                         (unsigned char)(139*distShade),255};
                    if(edge>0.85f) steel=(Color){(unsigned char)(48*distShade),(unsigned char)(55*distShade),(unsigned char)(64*distShade),255};
                    else if(u< -0.35f)steel=(Color){(unsigned char)(156*distShade),(unsigned char)(165*distShade),(unsigned char)(173*distShade),255};
                    if(v< -0.76f)steel=(Color){(unsigned char)(62*distShade),(unsigned char)(68*distShade),(unsigned char)(75*distShade),255}; // feed lips
                    else if(v>0.79f)steel=(Color){(unsigned char)(38*distShade),(unsigned char)(44*distShade),(unsigned char)(51*distShade),255}; // base plate
                    else if(v>0.30f && ((int)((v+1.0f)*32.0f)%5)==0)steel=(Color){(unsigned char)(64*distShade),(unsigned char)(76*distShade),(unsigned char)(88*distShade),255};
                    if(fabsf(u+0.65f)<0.035f && v>-0.6f && v<0.65f)steel=(Color){(unsigned char)(194*distShade),(unsigned char)(197*distShade),(unsigned char)(200*distShade),255}; // highlight
                    if(v>0.57f && v<0.72f && fabsf(u)<0.47f)steel=(Color){(unsigned char)(46*distShade),(unsigned char)(53*distShade),(unsigned char)(59*distShade),255}; // base marking
                    pixels[sy*SW+sx]=MixColor(pixels[sy*SW+sx],steel,alpha);
                }
            }
            continue;
        }
        for (int sx_loop = px - w / 2; sx_loop <= px + w / 2; sx_loop++) {
			if (sx_loop < 0 || sx_loop >= SW) continue;
			if (projDist > zbuf[sx_loop] + 0.1f) continue;
			
			int current_h = h;
            if (!shells[i].isMagazine && sx_loop > px + w / 4) current_h = h - 1;
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
//  СЕКТОР 12: MAIN MENU / SETTINGS
// ------------------------------------------------------------

// Minimap intentionally removed: navigation is entirely first-person.

	bool isFullscreen = false;
	void ToggleScreenMode(void) {
		isFullscreen = !isFullscreen;
		if (isFullscreen) {
			int monitor = GetCurrentMonitor();
			int monW = GetMonitorWidth(monitor);
			int monH = GetMonitorHeight(monitor);
			SetWindowSize(monW, monH);
			if (!IsWindowFullscreen()) ToggleFullscreen();
		} else {
			if (IsWindowFullscreen()) ToggleFullscreen();
			SetWindowSize(SW, SH);
			int monitor = GetCurrentMonitor();
			SetWindowPosition(
							  (GetMonitorWidth(monitor) - SW) / 2,
							  (GetMonitorHeight(monitor) - SH) / 2
							  );
		}
	}
	
// Dusty stone, warm copper and cold teal: inspired by the map's brick/metal palette.
static const Color uiSand={225,210,173,255};
static const Color uiCopper={194,146,88,255};
static const Color uiTeal={116,178,170,255};
static const Color uiDark={22,28,30,250};

static void DrawTacticalFrame(int x,int y,int w,int h,const char *kicker) {
    DrawRectangle(x+7,y+8,w,h,(Color){7,10,10,210});
    DrawRectangle(x,y,w,h,uiDark);
    DrawRectangle(x+7,y+7,w-14,h-14,(Color){35,41,40,250});
    DrawRectangle(x+13,y+13,w-26,h-26,(Color){29,35,35,250});
    DrawRectangle(x,y,w,7,(Color){124,102,72,255});
    DrawRectangle(x,y+7,w,3,(Color){76,83,79,255});
    DrawRectangleLinesEx((Rectangle){(float)x,(float)y,(float)w,(float)h},2,uiCopper);
    for(int i=0;i<4;++i){
        int px=x+(i%2?w-15:15),py=y+(i/2?h-15:15);
        DrawCircle(px,py,3,(Color){117,110,94,255});
        DrawLine(px-2,py+1,px+2,py-1,(Color){40,39,34,255});
    }
    // Non-animated fixed grime and relief to match the environment.
    for(int i=0;i<62;++i) {
        unsigned int r=hash2((unsigned int)(i+47),(unsigned int)(w+h));
        int px=x+17+(int)(r%(unsigned int)(w-34));
        int py=y+18+(int)((r>>16)%(unsigned int)(h-36));
        DrawPixel(px,py,(Color){93,84,66,45});
    }
    if(kicker && kicker[0])DrawText(kicker,x+27,y+18,13,uiTeal);
}

static void DrawSectionSeparator(int x,int y,int w){
    DrawRectangle(x,y,w,2,(Color){111,95,71,255});
    DrawRectangle(x,y+3,w,1,(Color){53,69,70,255});
}

void DrawMainMenu(void) {
    int curW=GetScreenWidth(),curH=GetScreenHeight();
    ClearBackground((Color){22,28,27,255});
    int w=600,h=510,x=(curW-w)/2,y=(curH-h)/2;
    DrawTacticalFrame(x,y,w,h,"SURVIVAL // LAN ARENA");
    DrawText("BATTLE SIMULATION",x+46,y+65,36,uiSand);
    DrawText("IN HARD TIMES",x+49,y+113,19,uiCopper);
    DrawSectionSeparator(x+42,y+148,w-84);
    const char* opts[4]={"START GAME",isFullscreen?"DISPLAY: FULLSCREEN":"DISPLAY: WINDOWED","MOUSE SETTINGS","EXIT GAME"};
    for(int i=0;i<4;++i){
        int yy=y+168+i*66;
        bool selected=i==menuSelection;
        DrawRectangle(x+43,yy,w-86,52,selected?(Color){73,83,69,255}:(Color){43,51,48,255});
        DrawRectangle(x+43,yy,selected?5:2,52,selected?uiCopper:(Color){90,99,93,255});
        DrawText(opts[i],x+68,yy+14,23,selected?uiSand:(Color){168,168,148,255});
        if(selected)DrawText(">",x+w-86,yy+13,22,uiCopper);
        Rectangle hit={(float)(x+43),(float)yy,(float)(w-86),52.0f};
        if(currentState==STATE_MENU && !showSettingsMenu &&
           CheckCollisionPointRec(GetMousePosition(),hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            menuSelection=i;
            if(i==0) {menuSelection=0;currentState=STATE_LOBBY;}
            else if(i==1)ToggleScreenMode();
            else if(i==2)showSettingsMenu=true;
            else quitRequested=true;
        }
    }
    DrawText("W/S  SELECT     ENTER  CONFIRM / CLICK",x+45,y+h-34,17,uiTeal);
}

// Screen-space HUD helpers: raycaster world stays separate from game UI.
static bool DrawMenuButton(Rectangle rect, const char *caption, Color hoverColor) {
    bool hover=CheckCollisionPointRec(GetMousePosition(),rect);
    Color fill=hover?hoverColor:(Color){64,75,69,255};
    DrawRectangleRec(rect,fill);
    DrawRectangle((int)rect.x,(int)rect.y,5,(int)rect.height,hover?uiSand:uiCopper);
    DrawRectangleLinesEx(rect,2,hover?uiSand:(Color){126,119,99,255});
    int fs=18;
    DrawText(caption,(int)(rect.x+(rect.width-MeasureText(caption,fs))*0.5f),
             (int)(rect.y+(rect.height-fs)*0.5f),fs,uiSand);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static int activeSettingSlider=-1;
static float DrawTacticalSlider(int id,int x,int y,int w,const char* label,float val,float low,float high) {
    Rectangle hit={(float)x,(float)(y+32),(float)w,32.0f};
    Vector2 mp=GetMousePosition();
    if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp,hit))activeSettingSlider=id;
    if(!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && activeSettingSlider==id){activeSettingSlider=-1;SaveControls();}
    if(activeSettingSlider==id && IsMouseButtonDown(MOUSE_BUTTON_LEFT)){
        val=low+(high-low)*fmaxf(0.0f,fminf(1.0f,(mp.x-x)/w));
    }
    float t=(val-low)/(high-low);
    DrawText(label,x,y,18,uiSand);
    DrawRectangle(x,y+39,w,9,(Color){62,64,57,255});
    DrawRectangle(x,y+39,(int)(w*t),9,uiTeal);
    DrawRectangle(x+(int)(w*t)-6,y+32,12,24,uiCopper);
    DrawRectangleLinesEx((Rectangle){(float)x,(float)(y+39),(float)w,9},1,(Color){123,118,95,255});
    return val;
}

static void DrawSettingsPanel(int curW,int curH) {
    DrawRectangle(0,0,curW,curH,Fade(BLACK,0.64f));
    int w=600,h=398,x=curW/2-w/2,y=curH/2-h/2;
    DrawTacticalFrame(x,y,w,h,"INPUT CONFIGURATION // CS-STYLE");
    DrawText("MOUSE SETTINGS",x+35,y+54,28,uiSand);
    DrawSectionSeparator(x+32,y+101,w-64);
    mouseSensitivity=DrawTacticalSlider(0,x+38,y+121,w-76,"SENSITIVITY",mouseSensitivity,0.1f,5.0f);
    DrawText(TextFormat("%.2f",mouseSensitivity),x+w-116,y+120,18,uiCopper);
    float newDpi=DrawTacticalSlider(1,x+38,y+205,w-76,"VIRTUAL DPI / AIM MULTIPLIER",(float)virtualMouseDPI,400.0f,3200.0f);
    virtualMouseDPI=(int)(roundf(newDpi/100.0f)*100);
    DrawText(TextFormat("%d",virtualMouseDPI),x+w-116,y+204,18,uiCopper);
    DrawText("Affects game aim only, not your mouse hardware DPI.",x+37,y+288,15,(Color){166,173,156,255});
    Rectangle back={(float)(x+171),(float)(y+h-70),258,44};
    if(DrawMenuButton(back,"SAVE & BACK",(Color){95,113,84,255})) {
        showSettingsMenu=false;activeSettingSlider=-1;
        SaveControls();
        if(currentState==STATE_PLAYING && !showPauseMenu && !showTabMenu)
            {blockFireUntilRelease=true;DisableCursor();}
    }
}

static void DrawPausePanel(int curW,int curH) {
    DrawRectangle(0,0,curW,curH,Fade(BLACK,0.54f));
    int w=510,h=420,x=curW/2-w/2,y=curH/2-h/2;
    DrawTacticalFrame(x,y,w,h,"GAME PAUSED // ESC");
    DrawText("PAUSE MENU",x+35,y+55,33,uiSand);
    DrawSectionSeparator(x+33,y+109,w-66);
    Rectangle resume={(float)x+50,(float)y+134,(float)w-100,50};
    Rectangle settings={(float)x+50,(float)y+196,(float)w-100,50};
    Rectangle menu={(float)x+50,(float)y+258,(float)w-100,50};
    Rectangle leave={(float)x+50,(float)y+320,(float)w-100,50};
    if(DrawMenuButton(resume,"RESUME",(Color){83,114,98,255})){
        showPauseMenu=false;blockFireUntilRelease=true;DisableCursor();
    }
    if(DrawMenuButton(settings,"MOUSE SETTINGS",(Color){91,108,96,255}))showSettingsMenu=true;
    if(DrawMenuButton(menu,"MAIN MENU",(Color){104,97,77,255}))ReturnToMainMenu();
    if(DrawMenuButton(leave,"EXIT GAME",(Color){133,78,67,255})){
        LeaveServer();quitRequested=true;
    }
}

static void DrawPlayerHealth(int curW,int curH) {
    if(myPlayerID<0 || myPlayerID>=MAX_NET_PLAYERS) return;
    int health=netPlayers[myPlayerID].health;
    if(health<0) health=0;
    if(health>100) health=100;
    int w=235,h=17,x=24,y=curH-62;
    (void)curW;
    DrawRectangle(x-4,y-4,w+8,h+8,(Color){9,16,24,213});
    DrawRectangle(x,y,w,h,(Color){44,48,56,240});
    Color fill=(health>60)?(Color){61,206,139,255}:(health>25)?(Color){240,183,71,255}:(Color){226,70,71,255};
    DrawRectangle(x+2,y+2,(int)((w-4)*health/100.0f),h-4,fill);
    DrawRectangleLines(x-4,y-4,w+8,h+8,(Color){137,161,181,245});
    DrawText(TextFormat("HP  %d / 100",health),x,y-23,18,RAYWHITE);
}

static void DrawGameChat(int curH) {
    float now=(float)GetTime();
    int lines=0;
    // Oldest first, so messages grow down toward the text field.
    for(int i=MAX_CHAT_LINES-1;i>=0;--i)
        if(chatLines[i][0] && (showChatInput || now-chatLineTimes[i]<10.0f)) lines++;
    int baseY=curH-125-lines*23;
    if(lines>0 || showChatInput) {
        int h=(lines+1)*23+(showChatInput?38:5);
        DrawRectangle(14,baseY-9,480,h,Fade((Color){8,16,23,255},0.70f));
    }
    int y=baseY;
    for(int i=MAX_CHAT_LINES-1;i>=0;--i) {
        if(!chatLines[i][0] || (!showChatInput&&now-chatLineTimes[i]>=10.0f)) continue;
        DrawChatText(chatLines[i],22,y,18,(Color){220,230,236,255});
        y+=23;
    }
    if(showChatInput) {
        DrawRectangle(18,y+3,472,32,(Color){27,46,60,246});
        DrawChatText(TextFormat("[T] %s_",chatInput),25,y+8,19,RAYWHITE);
    }
}

static void DrawKillsFeed(int curW) {
    float now=(float)GetTime();
    int row=0;
    for(int i=0;i<MAX_KILL_LINES;++i) {
        if(!killLines[i][0] || now-killLineTimes[i]>6.5f) continue;
        int x=curW-402, y=20+row*30;
        DrawRectangle(x-7,y-5,385,29,Fade((Color){8,16,23,255},0.75f));
        DrawText(killLines[i],x,y,18,(Color){255,214,154,255});
        row++;
    }
}

static void DrawKeyHints(void) {
    if(!showKeyHints)return;
    int x=18,y=18;
    const char *lines[]={"W A S D   MOVE","SHIFT     SPRINT","SPACE     JUMP","R         RELOAD","TAB       PLAYERS","T         CHAT","5         HIDE HINTS","ESC       PAUSE"};
    DrawRectangle(x-8,y-9,225,246,Fade((Color){12,19,27,255},0.77f));
    DrawRectangleLines(x-8,y-9,225,246,(Color){100,131,148,225});
    DrawText("CONTROLS",x+3,y,18,(Color){155,215,248,255});
    for(int i=0;i<8;++i) DrawText(lines[i],x+3,y+31+i*24,17,RAYWHITE);
}

static void DrawScoreboard(int curW,int curH) {
    int count=0;
    for(int i=0;i<MAX_NET_PLAYERS;++i)if(netActive[i])count++;
    const int w=640,h=235+(count>0?count:1)*35;
    int x=curW/2-w/2,y=curH/2-h/2;
    DrawRectangle(0,0,curW,curH,Fade(BLACK,0.55f));
    DrawTacticalFrame(x,y,w,h,"MULTIPLAYER // TAB");
    DrawText(TextFormat("SERVER #%d",currentServerNumber),x+32,y+52,29,uiSand);
    DrawText(TextFormat("ONLINE: %d / 8",count),x+w-194,y+58,18,uiTeal);
    if(isServer)DrawText(TextFormat("HOST: %s",myLocalIP),x+34,y+92,16,uiCopper);
    DrawSectionSeparator(x+28,y+121,w-56);
    DrawText("PLAYER",x+42,y+137,16,uiCopper);
    DrawText("K",x+386,y+137,16,uiCopper);
    DrawText("D",x+451,y+137,16,uiCopper);
    DrawText("KD%",x+526,y+137,16,uiCopper);
    int visibleRow=0;
    for(int i=0;i<MAX_NET_PLAYERS;++i){
        if(!netActive[i])continue; // empty slots stay completely invisible
        int yy=y+164+visibleRow*35;
        DrawRectangle(x+29,yy-6,w-58,32,visibleRow%2?(Color){44,51,48,255}:(Color){53,62,54,255});
        Color fg=(i==myPlayerID)?(Color){166,228,177,255}:uiSand;
        DrawText(TextFormat("%02d   Player %d%s",i+1,i+1,i==myPlayerID?"  [YOU]":""),x+43,yy,18,fg);
        int k=netPlayers[i].kills,d=netPlayers[i].deaths;
        int kd=(k+d>0)?(int)(100.0f*k/(k+d)+0.5f):0;
        DrawText(TextFormat("%d",k),x+383,yy,18,fg);
        DrawText(TextFormat("%d",d),x+447,yy,18,fg);
        DrawText(TextFormat("%d%%",kd),x+519,yy,18,fg);
        visibleRow++;
    }
    if(count==0)DrawText("WAITING FOR PLAYERS...",x+44,y+170,17,uiSand);
    Rectangle menuBtn={(float)(x+34),(float)(y+h-63),260,43};
    Rectangle exitBtn={(float)(x+346),(float)(y+h-63),260,43};
    if(DrawMenuButton(menuBtn,"MAIN MENU",(Color){94,102,79,255}))ReturnToMainMenu();
    if(DrawMenuButton(exitBtn,"EXIT GAME",(Color){133,77,65,255})){
        LeaveServer();quitRequested=true;
    }
}

static void DrawDeathScreen(int curW,int curH) {
    DrawRectangle(0,0,curW,curH,Fade(WHITE,0.95f));
    DrawText("YOU DIED",curW/2-MeasureText("YOU DIED",52)/2,curH/2-105,52,(Color){43,55,67,255});
    DrawText("Your next life starts with a fresh magazine",curW/2-MeasureText("Your next life starts with a fresh magazine",18)/2,
             curH/2-34,18,(Color){84,91,97,255});
    Rectangle b={(float)(curW/2-148),(float)(curH/2+30),296,62};
    if(DrawMenuButton(b,"RESPAWN",(Color){49,132,100,255})) RespawnMe();
}

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
		
		srand((unsigned int)time(NULL));
		
	
	InitWindow(SW, SH, "Battle simulation in hard times");
	SetExitKey(KEY_NULL);
    InitAudioDevice();
    InitChatFont();
    LoadControls();
	GenerateAllTextures();
	GenerateTofuTextures();
	GenMap();
    LoadPosters();
    PlacePosters();
		
	
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
	sndHit               = LoadSound("soundfiles/hit.mp3");    // <-- ДОБАВИТЬ
	sndDeath             = LoadSound("soundfiles/death.mp3");  // <-- ДОБАВИТЬ
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
	
	// Minimap disabled; no minimap render texture is allocated.
	
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
	    if(dt>0.05f)dt=0.05f;
    if(dt<0.00001f)dt=0.00001f;
		
        if (hasMusic) UpdateMusicStream(music);
        if(quitRequested)break;
		
		// ============================================================
		// 1. ЛОГИКА (МЕНЮ, ЛОББИ, СПИСОК СЕРВЕРОВ, ИГРА)
		// ============================================================
		if (currentState == STATE_MENU) {
            if(showSettingsMenu) {
                if(IsKeyPressed(KEY_ESCAPE)) {showSettingsMenu=false;SaveControls();}
            } else {
                if(IsKeyPressed(KEY_W)||IsKeyPressed(KEY_UP))menuSelection--;
                if(IsKeyPressed(KEY_S)||IsKeyPressed(KEY_DOWN))menuSelection++;
                if(menuSelection<0)menuSelection=3;
                if(menuSelection>3)menuSelection=0;
                if(IsKeyPressed(KEY_ENTER)) {
                    switch(menuSelection){
                        case 0:menuSelection=0;currentState=STATE_LOBBY;break;
                        case 1:ToggleScreenMode();break;
                        case 2:showSettingsMenu=true;EnableCursor();break;
                        case 3:quitRequested=true;break;
                    }
                }
            }
        }
		else if (currentState == STATE_LOBBY) {
			if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP)) menuSelection--;
			if (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN)) menuSelection++;
			if (menuSelection < 0) menuSelection = 2;
			if (menuSelection > 2) menuSelection = 0;
			
			if (IsKeyPressed(KEY_ENTER)) {
				if (menuSelection == 0) { // Создать сервер
					InitNetwork();
					StartServer((rand() % 99) + 1);
					currentState = STATE_TRANSITION;
					fadeToWhite = true;
					transitionAlpha = 0.0f;
							} else if (menuSelection == 1) { // Найти серверы в LAN
								InitNetwork();
								DiscoverServers();
								menuSelection = 0;
								typingIP = false;
								currentState = STATE_SERVER_LIST;
							} else if (menuSelection == 2) { // Назад
					menuSelection = 0;
					currentState = STATE_MENU;
				}
			}
		}

				else if (currentState == STATE_SERVER_LIST) {
					UpdateNetwork();
					
					if (IsKeyPressed(KEY_I)) {
						typingIP = !typingIP;
					}
					
					if (typingIP) {
						int key = GetCharPressed();
						while (key > 0) {
							int len = (int)strlen(manualIP);
							if (((key >= '0' && key <= '9') || key == '.') && len < 15) {
								manualIP[len] = (char)key;
								manualIP[len + 1] = '\0';
							}
							key = GetCharPressed();
						}
						if (IsKeyPressed(KEY_BACKSPACE)) {
							int len = (int)strlen(manualIP);
							if (len > 0) manualIP[len - 1] = '\0';
						}
						if (IsKeyPressed(KEY_ENTER) && strlen(manualIP) >= 7) {
							typingIP = false;
							ConnectToServer(manualIP, 1);
							currentState = STATE_TRANSITION;
							fadeToWhite = true;
							transitionAlpha = 0.0f;
						}
					} else {
						// Автоматически повторяем поиск каждую 1 секунду
						static float scanTimer = 0.0f;
						scanTimer += dt;
						if (scanTimer >= 1.0f) {
							scanTimer = 0.0f;
							SendDiscoveryBroadcast();
						}
						
						if (IsKeyPressed(KEY_R)) DiscoverServers();
	                    if (IsKeyPressed(KEY_BACKSPACE)) {
                            LeaveServer();
							menuSelection = 0;
							currentState = STATE_LOBBY;
						}
						if (discoveredServersCount > 0) {
							if (IsKeyPressed(KEY_W) || IsKeyPressed(KEY_UP)) menuSelection--;
							if (IsKeyPressed(KEY_S) || IsKeyPressed(KEY_DOWN)) menuSelection++;
							if (menuSelection < 0) menuSelection = discoveredServersCount - 1;
							if (menuSelection >= discoveredServersCount) menuSelection = 0;
							
							if (IsKeyPressed(KEY_ENTER)) {
								ConnectToServer(discoveredServersIPs[menuSelection], discoveredServerNums[menuSelection]);
								currentState = STATE_TRANSITION;
								fadeToWhite = true;
								transitionAlpha = 0.0f;
							}
						}
					}
				}
		
		else if (currentState == STATE_TRANSITION) {
			UpdateNetwork();
			if (fadeToWhite) {
				transitionAlpha += dt * 1.0f;
				if (transitionAlpha >= 1.0f) {
					transitionAlpha = 1.0f;
					fadeToWhite = false;
					DisableCursor();
				}
			} else {
				transitionAlpha -= dt * 0.6f;
				if (transitionAlpha <= 0.0f) {
					transitionAlpha = 0.0f;
					currentState = STATE_PLAYING;
				}
			}
		}
        else if (currentState == STATE_PLAYING) {
            UpdateNetwork();
            if (currentState != STATE_PLAYING) continue; // Host may have disconnected.

            // Если клиент еще не получил ID от сервера — повторяем запрос входа раз в 0.5 сек
			if (isClient && myPlayerID < 0) {
				static float joinRetryTimer = 0.0f;
				joinRetryTimer += dt;
				if (joinRetryTimer >= 0.5f) {
					joinRetryTimer = 0.0f;
					NetworkPacket pkt;
					memset(&pkt, 0, sizeof(pkt));
					pkt.type = PKT_JOIN;
					sendto(udpSocket, (const char*)&pkt, sizeof(pkt), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
				}
			}
			
            if(IsKeyPressed(KEY_FIVE)) showKeyHints=!showKeyHints;
            if(IsKeyPressed(KEY_ESCAPE) && !showChatInput) {
                if(showSettingsMenu) {showSettingsMenu=false;SaveControls();}
                else if(showTabMenu) {showTabMenu=false;showPauseMenu=true;}
                else showPauseMenu=!showPauseMenu;
                blockFireUntilRelease=true;
                if(showPauseMenu || showTabMenu || showSettingsMenu || netPlayers[myPlayerID>=0?myPlayerID:0].isDead)EnableCursor();
                else DisableCursor();
            }
            if(IsKeyPressed(KEY_TAB) && !showChatInput && !showSettingsMenu && !showPauseMenu) {
                showTabMenu=!showTabMenu;
                blockFireUntilRelease=true;
                if(showTabMenu || (myPlayerID>=0 && netPlayers[myPlayerID].isDead))EnableCursor();
                else DisableCursor();
            }
            if(IsKeyPressed(KEY_T) && !showChatInput && !showTabMenu && !showPauseMenu && !showSettingsMenu && myPlayerID>=0 && !netPlayers[myPlayerID].isDead) {
                showChatInput=true;
                chatInput[0]='\0';
                // Drain the T character that opened the chat.
                while(GetCharPressed()>0){}
            } else if(showChatInput) {
                int cp=GetCharPressed();
                while(cp>0) {
                    if(cp>=32 && cp<0x10000) {
                        int length=0;
                        const char *utf8=CodepointToUTF8(cp,&length);
                        size_t old=strlen(chatInput);
                        if(length>0 && old+(size_t)length<sizeof(chatInput)-1) {
                            memcpy(chatInput+old,utf8,(size_t)length);
                            chatInput[old+length]='\0';
                        }
                    }
                    cp=GetCharPressed();
                }
                if(IsKeyPressed(KEY_BACKSPACE)) {
                    size_t n=strlen(chatInput);
                    if(n) {--n;while(n>0&&((unsigned char)chatInput[n]&0xC0)==0x80)--n;chatInput[n]='\0';}
                }
                if(IsKeyPressed(KEY_ESCAPE)) {showChatInput=false;chatInput[0]='\0';}
                if(IsKeyPressed(KEY_ENTER)) {
                    if(chatInput[0]) {
                        char line[192];snprintf(line,sizeof(line),"Player %d: %.128s",myPlayerID+1,chatInput);
                        AddChatLine(line);
                        SendChat(chatInput);
                    }
                    showChatInput=false;chatInput[0]='\0';
                }
            }
            if(myPlayerID>=0 && myPlayerID<MAX_NET_PLAYERS) {
                if(!netPlayers[myPlayerID].isDead && !showTabMenu && !showPauseMenu && !showSettingsMenu && !showChatInput) {
                    UpdatePlayer(dt);
                }
                netPlayers[myPlayerID].x=player.x;
                netPlayers[myPlayerID].y=player.y;
                netPlayers[myPlayerID].z=player.z;
                netPlayers[myPlayerID].pa=player.pa;
                SendMyState(netPlayers[myPlayerID]);
            }
            deathFlash=fmaxf(0.0f,deathFlash-dt*0.9f);
            if(quitRequested)break;

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
			
			
			BeginTextureMode(postRT);
			ClearBackground(BLACK);
			DrawTexture(screenTexture, 0, 0, WHITE);
			
			// --- СПРАЙТ ПИСТОЛЕТА ---
			Texture2D currentTex = pistol.texIdle; 
			if (pistol.isReloading) currentTex = pistol.texReload;
			else if (pistol.currentFrame == 1) currentTex = pistol.texFire1;
			else if (pistol.currentFrame == 2) currentTex = pistol.texFire2;
			else if (pistol.currentFrame == 3) currentTex = pistol.texFire3;
			
            if (currentTex.id > 0 && (myPlayerID < 0 || !netPlayers[myPlayerID].isDead)) {
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
		
		// 1. Главное меню
        if (currentState == STATE_MENU || (currentState == STATE_TRANSITION && fadeToWhite)) {
            DrawMainMenu();
            if(currentState==STATE_MENU && showSettingsMenu)
                DrawSettingsPanel(GetScreenWidth(),GetScreenHeight());
        } 
		// 2. Экран выбора: Создать или Найти сервер
		else if (currentState == STATE_LOBBY) {
			int curW = GetScreenWidth();
			int curH = GetScreenHeight();
			ClearBackground((Color){15, 15, 15, 255});
			
			const char* title = "MULTIPLAYER (LAN)";
			DrawText(title, curW / 2 - MeasureText(title, 44) / 2, curH / 3 - 20, 44, (Color){220, 200, 150, 255});
			
			const char* opts[3] = { "CREATE SERVER", "JOIN LAN SERVER", "BACK" };
			Color activeCol = (Color){255, 230, 150, 255};
			Color idleCol   = (Color){80, 75, 60, 255};
			
			for (int i = 0; i < 3; i++) {
				int w = MeasureText(opts[i], 36);
				int y = curH / 2 + i * 60;
				Color col = (menuSelection == i) ? activeCol : idleCol;
				DrawText(opts[i], curW / 2 - w / 2, y, 36, col);
				if (menuSelection == i) DrawText(">", curW / 2 - w / 2 - 35, y, 36, col);
			}
			
			const char* hint = "W / S - NAVIGATE   |   ENTER - SELECT";
			DrawText(hint, curW / 2 - MeasureText(hint, 15) / 2, curH - 50, 15, (Color){60, 60, 60, 255});
		}
		
			// 3. Список найденных серверов в локальной сети
		else if (currentState == STATE_SERVER_LIST) {
			int curW = GetScreenWidth();
			int curH = GetScreenHeight();
			ClearBackground((Color){15, 15, 15, 255});
			
			const char* title = "AVAILABLE SERVERS";
			DrawText(title, curW / 2 - MeasureText(title, 40) / 2, 80, 40, (Color){220, 200, 150, 255});
			
			const char* sub = "R - REFRESH   |   BACKSPACE - BACK";
			DrawText(sub, curW / 2 - MeasureText(sub, 18) / 2, 135, 18, (Color){150, 140, 110, 255});
			
			if (discoveredServersCount == 0) {
				const char* waitTxt = "Searching for servers...";
				DrawText(waitTxt, curW / 2 - MeasureText(waitTxt, 26) / 2, curH / 2, 26, GRAY);
			} else {
				Color activeCol = (Color){255, 230, 150, 255};
				Color idleCol   = (Color){100, 95, 80, 255};
				for (int i = 0; i < discoveredServersCount; i++) {
					const char* label = TextFormat("SERVER #%d   [%s]", discoveredServerNums[i], discoveredServersIPs[i]);
					int w = MeasureText(label, 30);
					int y = 210 + i * 50;
					Color col = (menuSelection == i) ? activeCol : idleCol;
					DrawText(label, curW / 2 - w / 2, y, 30, col);
					if (menuSelection == i) DrawText(">", curW / 2 - w / 2 - 35, y, 30, col);
				}
			}
			
			// Подсказка и поле ручного ввода IP внизу экрана
			const char* ipPrompt = typingIP 
			? TextFormat("ENTER HOST IP: %s_  (ENTER TO CONNECT)", manualIP)
			: "PRESS [I] TO ENTER HOST IP MANUALLY";
			DrawText(ipPrompt, curW / 2 - MeasureText(ipPrompt, 20) / 2, curH - 80, 20, YELLOW);
		}
		
		// 4. Игра и проявление экрана
		else {
			int curW = GetScreenWidth();
			int curH = GetScreenHeight();
			
			float nowTime = (float)GetTime();
			SetShaderValue(ps2Shader, locTime, &nowTime, SHADER_UNIFORM_FLOAT);
			
			BeginShaderMode(ps2Shader);
			DrawTexturePro(
						   postRT.texture,
						   (Rectangle){0, 0, (float)SW, (float)-SH},
						   (Rectangle){0, 0, (float)curW, (float)curH},
						   (Vector2){0, 0},
						   0.0f,
						   WHITE
						   );
			EndShaderMode();
			
			// HUD
			if (currentState == STATE_PLAYING) {
				if (volume_ui_timer > 0.0f) {
					DrawRectangle(20, curH - 40, 200, 20, Fade(BLACK, 0.5f));
					DrawRectangle(25, curH - 35, (int)(190 * music_volume), 10, BLUE);
					DrawText(TextFormat("Vol: %d%% ([ / ])", (int)(music_volume * 100)), 25, curH - 55, 12, WHITE);
				}
				
				// --- ПАТРОНЫ ---
				int magX = curW - 50;
				int magBottomY = curH - 40;
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
				
				if (actualMagY < curH + magHeight) {
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
					if (fillAngle > 0) DrawCircleSector((Vector2){(float)indX, (float)indY}, indR - 3, -90.0f, -90.0f + fillAngle, 32, fill);
					DrawText("RELOAD", indX - 24, indY + indR + 8, 11, label);
				}
				
				if (numberAlpha > 0.01f) {
					int ammoToShow = pistol.isReloading ? (magIsFull ? pistol.maxAmmo : pistol.ammo) : pistol.ammo;
					unsigned char a1 = (unsigned char)(255 * numberAlpha);
					unsigned char a2 = (unsigned char)(220 * numberAlpha);
					DrawText(TextFormat("%d", ammoToShow), magX - 95, magBottomY - 70, 64, (Color){250, 230, 170, a1});
					DrawText(TextFormat("/%d", pistol.maxAmmo), magX - 55, magBottomY - 18, 18, (Color){150, 120, 80, a2});
				}
				
				// --- СТАМИНА ---
				if (stamina_ui_timer > 0.0f) {
					int stW = 200, stH = 6;
					int stX = curW / 2 - stW / 2;
					int stY = curH - 30;
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
				if (!showTabMenu && !showPauseMenu && !showSettingsMenu && (myPlayerID<0 || !netPlayers[myPlayerID].isDead)) {
					float scaleX = (float)curW / (float)SW;
					float scaleY = (float)curH / (float)SH;
					int cx = curW / 2 + (int)(player.crosshairX * scaleX);
					int cy = curH / 2 + (int)(player.crosshairY * scaleY);
					
					Color chColor = (Color){255, 255, 255, 220}; 
					DrawLine(cx, cy - 3, cx, cy + 3, chColor);
					int gapX = 12, gapY = 4, len = 4;
					DrawLine(cx - gapX, cy - gapY, cx - gapX, cy + gapY, chColor);
					DrawLine(cx - gapX, cy - gapY, cx - gapX - len, cy - gapY, chColor);
					DrawLine(cx + gapX, cy - gapY, cx + gapX, cy + gapY, chColor);
					DrawLine(cx + gapX, cy - gapY, cx + gapX + len, cy - gapY, chColor);
				}
				
                DrawPlayerHealth(curW,curH);
                DrawKillsFeed(curW);
                if(!showTabMenu && !showPauseMenu && !showSettingsMenu){
                    DrawKeyHints();
                    DrawGameChat(curH);
                }
                if(showTabMenu) DrawScoreboard(curW,curH);
                if(showPauseMenu) DrawPausePanel(curW,curH);
                if(showSettingsMenu) DrawSettingsPanel(curW,curH);
                if(myPlayerID>=0 && netPlayers[myPlayerID].isDead) DrawDeathScreen(curW,curH);

			}
			
			// БЕЛАЯ ВСПЫШКА
			if (currentState == STATE_TRANSITION) {
				DrawRectangle(0, 0, curW, curH, Fade(WHITE, transitionAlpha));
			}
		}
		
        EndDrawing();
        if(quitRequested)break;
    }

    // Notify others even when the window was closed with the system X button.
    if(udpSocket!=INVALID_SOCKET && myPlayerID>=0) SendEvent(PKT_DISCONNECT,-1,0);

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
	if (sndHit.frameCount > 0) UnloadSound(sndHit);
    if (sndDeath.frameCount > 0) UnloadSound(sndDeath);
    if (pistol.sndShot.frameCount > 0) UnloadSound(pistol.sndShot);
    if (pistol.sndReloadStart.frameCount > 0) UnloadSound(pistol.sndReloadStart);
    if (pistol.sndReloadEnd.frameCount > 0) UnloadSound(pistol.sndReloadEnd);
    if (pistol.sndEmpty.frameCount > 0) UnloadSound(pistol.sndEmpty);
    if (customChatFont) UnloadFont(chatFont);
    SaveControls();
	if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
	WSACleanup();
	
	UnloadShader(ps2Shader);
	UnloadRenderTexture(postRT);
	if (hasMusic) UnloadMusicStream(music);
	UnloadTexture(screenTexture); 
	UnloadImage(renderBuffer);
	CloseAudioDevice(); 
	CloseWindow();
	return 0;
}
