
// *******************************************************************************************
// *******************************************************************************************
//
//		File:		debugger.c
//		Date:		5th September 2019
//		Purpose:	Debugger code
//		Author:		Paul Robson (paul@robson.org.uk)
//
// *******************************************************************************************
// *******************************************************************************************

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <SDL.h>
#include "../glue.h"
#include "disasm.h"
#include "../memory.h"
#include "../video.h"
#include "../cpu/fake6502.h"
#include "debugger.h"
#include "rendertext.h"
#include "../console/SDL_console.h"
#include "../console/DT_drawtext.h"
#include "../console/split.h"
#include "../iniparser/iniparser.h"
#include "commands.h"
#include "symbols.h"

static void DEBUGHandleKeyEvent(SDL_Keycode key,int isShift);

static void DEBUGRenderData(int y, int memaddr);
static void DEBUGRenderRegisters();
static void DEBUGRenderVRAM(int y, int vmemaddr);
static void DEBUGRenderCode(int lines,int initialPC);
static void DEBUGRenderStack(int bytesCount);

static void DEBUG_Command_Handler(ConsoleInformation *console, char* command);

// *******************************************************************************************
//
//		This is the minimum-interference flag. It's designed so that when
//		its non-zero DEBUGRenderDisplay() is called.
//
//			if (showDebugOnRender != 0) {
//				DEBUGRenderDisplay(SCREEN_WIDTH,SCREEN_HEIGHT,renderer);
//				SDL_RenderPresent(renderer);
//				return true;
//			}
//
//		before the SDL_RenderPresent call in video_update() in video.c
//
//		This controls what is happening. It is at the top of the main loop in main.c
//
//			if (isDebuggerEnabled != 0) {
//				int dbgCmd = DEBUGGetCurrentStatus();
//				if (dbgCmd > 0) continue;
//				if (dbgCmd < 0) break;
//			}
//
//		Both video.c and main.c require debugger.h to be included.
//
//		isDebuggerEnabled should be a flag set as a command line switch - without it
//		it will run unchanged. It should not be necessary to test the render code
//		because showDebugOnRender is statically initialised to zero and will only
//		change if DEBUGGetCurrentStatus() is called.
//
// *******************************************************************************************

//
//				0-9A-F sets the program address, with shift sets the data address.
//
#define DBGKEY_HOME 	SDLK_F1 								// F1 is "Goto PC"
#define DBGKEY_RESET 	SDLK_F2 								// F2 resets the 6502
#define DBGKEY_RUN 		SDLK_F5 								// F5 is run.
#define DBGKEY_SETBRK 	SDLK_F9									// F9 sets breakpoint
#define DBGKEY_STEP 	SDLK_F11 								// F11 is step into.
#define DBGKEY_STEPOVER	SDLK_F10 								// F10 is step over.
#define DBGKEY_PAGE_NEXT	SDLK_KP_PLUS
#define DBGKEY_PAGE_PREV	SDLK_KP_MINUS

#define DBGSCANKEY_BRK 	SDL_SCANCODE_F12 						// F12 is break into running code.
#define DBGSCANKEY_SHOW	SDL_SCANCODE_TAB 						// Show screen key.
																// *** MUST BE SCAN CODES ***

#define DBGMAX_ZERO_PAGE_REGISTERS 20

// RGB colours
// const SDL_Color col_bkgnd= {0, 0, 0, 255};
SDL_Color col_label= {0, 255, 0, 255};
SDL_Color col_data= {0, 255, 255, 255};
SDL_Color col_highlight= {255, 255, 0, 255};

const SDL_Color col_vram_tilemap = {0, 255, 255, 255};
const SDL_Color col_vram_tiledata = {0, 255, 0, 255};
const SDL_Color col_vram_special  = {255, 92, 92, 255};
const SDL_Color col_vram_other  = {128, 128, 128, 255};

int showDebugOnRender = 0;										// Used to trigger rendering in video.c
int showFullDisplay = 0; 										// If non-zero show the whole thing.
int currentPC = -1;												// Current PC value.
int currentData = 0;											// Current data display address.
int currentPCBank = -1;
int currentBank = 0;
int currentMode = DMODE_RUN;									// Start running.
int breakPoint = -1; 											// User Break
int stepBreakPoint = -1;										// Single step break.
int dumpmode          = DDUMP_RAM;
int showFullConsole = 0;
int breakpoints[DBG_MAX_BREAKPOINTS];
int breakpointsCount= 0;
int isWindowVisible = 0;
int mouseZone= 0;
int disasmLine1Size= 0;

int dbg_height = DBG_HEIGHT;
int win_height = 800;
int win_width = 640;
int con_height = 50;
int layout= 0;

int DBG_STCK= 			80;											// Debug stack starts here.
const int DBG_ZP_REG=   80;                             			// Zero page registers start here
const int DBG_REG=   	48;                            				// Registers start here
const int DBG_BP_REG= 	62;                             			// Breakpoins registers start here
SDL_Rect smallDataZoneRect= { 0, 290, 525, 455 };
SDL_Rect smallCodeZoneRect= { 0, 0, 310, 280 };
SDL_Rect largeCodeZoneRect= { 0, 0, 330, 745 };
SDL_Rect emptyZoneRect= { 0, 0, 0, 0 };

SDL_Rect *codeZoneRect= &smallCodeZoneRect;
SDL_Rect *dataZoneRect= &smallDataZoneRect;

//
//		This flag controls
//

SDL_Renderer *dbgRenderer; 										// Renderer passed in.
static SDL_Window* dbgWindow;
static uint32_t dbgWindowID;
ConsoleInformation *console;

// *******************************************************************************************
//
//									Write Hex/Bin Constant
//
// *******************************************************************************************
const char *bit_rep[16] = {
    [ 0] = "0000", [ 1] = "0001", [ 2] = "0010", [ 3] = "0011",
    [ 4] = "0100", [ 5] = "0101", [ 6] = "0110", [ 7] = "0111",
    [ 8] = "1000", [ 9] = "1001", [10] = "1010", [11] = "1011",
    [12] = "1100", [13] = "1101", [14] = "1110", [15] = "1111",
};

static void DEBUGNumber(int x, int y, int n, int w, SDL_Color colour) {
	char fmtString[8],buffer[16];
	if(w<0) {
		snprintf(buffer, sizeof(buffer), "%s%s", bit_rep[n >> 4], bit_rep[n & 0x0F]);
	} else {
		snprintf(fmtString, sizeof(fmtString), "%%0%dX", w);
		snprintf(buffer, sizeof(buffer), fmtString, n);
	}
	DEBUGString(dbgRenderer, x, y, buffer, colour);
}

// *******************************************************************************************
//
//									Write Bank:Address
//
// *******************************************************************************************
static void DEBUGAddress(int x, int y, int bank, int addr, SDL_Color colour) {
	char buffer[4];

	if(addr >= 0xA000) {
		snprintf(buffer, sizeof(buffer), "%.2X:", bank);
	} else {
		strcpy(buffer, "--:");
	}

	DEBUGString(dbgRenderer, x, y, buffer, colour);

	DEBUGNumber(x+3, y, addr, 4, colour);

}

static void
DEBUGVAddress(int x, int y, int addr, SDL_Color colour)
{
	DEBUGNumber(x, y, addr, 5, colour);
}

// *******************************************************************************************
//
//			This is used to determine who is in control. If it returns zero then
//			everything runs normally till the next call.
//			If it returns +ve, then it will update the video, and loop round.
//			If it returns -ve, then exit.
//
// *******************************************************************************************

bool isOnBreakpoint(int addr) {
	for(int idx= 0; idx<breakpointsCount; idx++) {
		if(breakpoints[idx] == addr)
			return true;
	}
	return false;
}

int  DEBUGGetCurrentStatus(void) {

	SDL_Event event;
	if (currentPC < 0) currentPC = pc;							// Initialise current PC displayed.

	if (currentMode == DMODE_STEP) {							// Single step before
		currentPC = pc;											// Update current PC
		currentMode = DMODE_STOP;								// So now stop, as we've done it.
	}

	if ((breakpointsCount && isOnBreakpoint(pc)) || pc == stepBreakPoint) {				// Hit a breakpoint.
		// to allow scroll code while on BP
		if(currentMode != DMODE_STOP)
			currentPC = pc;											// Update current PC
		currentMode = DMODE_STOP;								// So now stop, as we've done it.
		stepBreakPoint = -1;									// Clear step breakpoint.
	}

	if (SDL_GetKeyboardState(NULL)[DBGSCANKEY_BRK]) {			// Stop on break pressed.
		currentMode = DMODE_STOP;
		currentPC = pc; 										// Set the PC to what it is.
	}

	if(currentPCBank<0 && currentPC >= 0xA000) {
		currentPCBank= currentPC < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
	}

	if (currentMode != DMODE_RUN) {								// Not running, we own the keyboard.
		showFullDisplay = 										// Check showing screen.
					SDL_GetKeyboardState(NULL)[DBGSCANKEY_SHOW];
		while (SDL_PollEvent(&event)) { 						// We now poll events here.

			switch(event.type) {
				case SDL_QUIT:									// Time for exit
					return -1;

				case SDL_KEYDOWN:								// Handle key presses.
					DEBUGHandleKeyEvent(event.key.keysym.sym,
										SDL_GetModState() & (KMOD_LSHIFT|KMOD_RSHIFT));
					break;

				case SDL_MOUSEMOTION:
				{
					if(dbgWindowID != event.motion.windowID)
						break;

					SDL_Point mouse_position= {event.motion.x, event.motion.y};;

					if(SDL_PointInRect(&mouse_position, dataZoneRect))
						mouseZone= 2;
					else
					if(SDL_PointInRect(&mouse_position, codeZoneRect))
						mouseZone= 1;
					else
						mouseZone= 0;

					break;
				}

				case SDL_MOUSEWHEEL:
				{
					if(dbgWindowID != event.wheel.windowID || !event.wheel.y)
						break;

					switch(mouseZone) {
						case 1:
						{
							int inc= (event.wheel.y > 0) ? -3 : disasmLine1Size;
							currentPC+= inc;
							break;
						}
						case 2:
						{
							int inc= (event.wheel.y > 0) ? -0x200 : 0x200;
							currentData = (currentData + inc) & (dumpmode == DDUMP_RAM ? 0xFFFF : 0x1FFFF);
							break;
						}
					}
					break;

				}

			}

			CON_Events(&event);
			// if(!CON_Events(&event)) continue;

		}
	}

	showDebugOnRender = (currentMode != DMODE_RUN);				// Do we draw it - only in RUN mode.
	if (currentMode == DMODE_STOP) { 							// We're in charge.
		video_update();

		if(!isWindowVisible) {
			SDL_ShowWindow(dbgWindow);
			isWindowVisible= 1;
		}

		SDL_RenderClear(dbgRenderer);
		DEBUGRenderDisplay(win_width, win_height);
		SDL_RenderPresent(dbgRenderer);
		return 1;
	}

	SDL_HideWindow(dbgWindow);
	isWindowVisible= 0;

	return 0;													// Run wild, run free.
}

// *******************************************************************************************
//
//								Setup fonts and co
//
// *******************************************************************************************
void readSettings() {
	dictionary *iniDict= iniparser_load("x16emu.ini");
	if(iniDict) {
		const char *keys[16];
		int cmdCount= iniparser_getsecnkeys(iniDict, "dbg_ini_script");
		iniparser_getseckeys(iniDict, "dbg_ini_script", keys);
		for(int idx =0; idx < cmdCount; idx++) {
			char *cmd= (char *)iniparser_getstring(iniDict, keys[idx], "");
			CON_Out(console, cmd);
			DEBUG_Command_Handler(console, cmd);
		}

		char buffer[32];
		char *bp;
		for(int idx= 0; idx < DBG_MAX_BREAKPOINTS; idx++) {
			sprintf(buffer, "debugger:BP%d", idx);
			bp= (char *)iniparser_getstring(iniDict, buffer, NULL);
			if(bp && bp[0] != '\0') {
				sprintf(buffer, "bp %s", bp);
				CON_Out(console, buffer);
				DEBUG_Command_Handler(console, buffer);
			}
		}

		iniparser_freedict(iniDict);
	}
}

void DEBUGInitUI(SDL_Renderer *pRenderer) {
	SDL_Rect Con_rect;

	dbgWindow= SDL_CreateWindow("X16 Debugger", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_width, win_height, SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
	dbgRenderer= SDL_CreateRenderer(dbgWindow, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED );

	SDL_SetRenderDrawBlendMode(dbgRenderer, SDL_BLENDMODE_BLEND);

	dbgWindowID= SDL_GetWindowID(dbgWindow);

	// DEBUGInitChars(pRenderer);
	// dbgRenderer = pRenderer;				// Save renderer.
	DEBUGInitChars(dbgRenderer);

	Con_rect.x = 0;
	Con_rect.y = 0;
	Con_rect.w = win_width;
	Con_rect.h = win_height;
	console= CON_Init("ConsoleFont.bmp", dbgRenderer, 50, Con_rect);
	CON_Show(console);
	CON_SetExecuteFunction(console, DEBUG_Command_Handler);

	SDL_ShowCursor(SDL_ENABLE);

	symbol_init();

	readSettings();

}

// *******************************************************************************************
//
//								Setup fonts and co
//
// *******************************************************************************************
void DEBUGFreeUI() {
	CON_Destroy(console);
	symbol_free();
}

// *******************************************************************************************
//
//								Set a new breakpoint address. -1 to disable.
//
// *******************************************************************************************

void DEBUGSetBreakPoint(int newBreakPoint) {
	char command[16];
	sprintf(command, "bp %x", newBreakPoint);
	DEBUG_Command_Handler(console, command);
}

// *******************************************************************************************
//
//								Break into debugger from code.
//
// *******************************************************************************************

void DEBUGBreakToDebugger(void) {
	currentMode = DMODE_STOP;
	currentPC = pc;
}

// *******************************************************************************************
//
//									Handle keyboard state.
//
// *******************************************************************************************

static void DEBUGHandleKeyEvent(SDL_Keycode key,int isShift) {
	int opcode;

	switch(key) {

		case DBGKEY_STEP:									// Single step (F11 by default)
			currentMode = DMODE_STEP; 						// Runs once, then switches back.
			break;

		case DBGKEY_STEPOVER:								// Step over (F10 by default)
			opcode = real_read6502(pc, false, 0);							// What opcode is it ?
			if (opcode == 0x20) { 							// Is it JSR ?
				stepBreakPoint = pc + 3;					// Then break 3 on.
				currentMode = DMODE_RUN;					// And run.
			} else {
				currentMode = DMODE_STEP;					// Otherwise single step.
			}
			break;

		case DBGKEY_RUN:									// F5 Runs until Break.
			currentMode = DMODE_RUN;
			break;

		case DBGKEY_SETBRK:									// F9 Set breakpoint to displayed.
			DEBUGSetBreakPoint(currentPC);
			break;

		case DBGKEY_HOME:									// F1 sets the display PC to the actual one.
			currentPC = pc;
			currentPCBank= currentPC < 0xC000 ? memory_get_ram_bank() : memory_get_rom_bank();
			break;

		case DBGKEY_RESET:									// F2 reset the 6502
			reset6502();
			currentPC = pc;
			currentPCBank= -1;
			break;

		case DBGKEY_PAGE_NEXT:
			currentBank += 1;
			break;

		case DBGKEY_PAGE_PREV:
			currentBank -= 1;
			break;

	}

}

// *******************************************************************************************
//
//							Commands Handler
//
// *******************************************************************************************

void DEBUG_Command_Handler(ConsoleInformation *console, char* command) {
	int argc;
	char* argv[128];
	char* linecopy;

	linecopy= strdup(command);
	argc= splitline(argv, (sizeof argv)/(sizeof argv[0]), linecopy);
	if(!argc) {
		free(linecopy);
		showFullConsole= 1-showFullConsole;
		return;
	}

	for (int idx= 0; cmd_table[idx].name; idx++) {
		if(!strcasecmp(cmd_table[idx].name, argv[0])) {
			if(argc-1 < cmd_table[idx].minargc) {
				CON_Out(console, cmd_table[idx].help);
				return;
			}
			return cmd_table[idx].fn(cmd_table[idx].data, argc, argv);
		}
	}

	CON_Out(console, "%sERR: unknown command%s", DT_color_red, DT_color_default);
}

// *******************************************************************************************
//
//									 Render Data Area
//
// *******************************************************************************************

static void DEBUGRenderData(int y, int memaddr) {

	while (y < dbg_height-2) {									// To bottom of screen
		memaddr &= 0xFFFF;
		DEBUGAddress(DBG_MEMX, y, (uint8_t)currentBank, memaddr, col_label);	// Show label.
		for (int i = 0;i < 16;i++) {
			int addr= memaddr + i;
			// if in RAM or in ROM and outside existing banks, print nothing
			if(!isValidAddr(currentBank, addr))
				continue;
			else {
				int byte= real_read6502(addr, true, currentBank);
				DEBUGNumber(DBG_MEMX+8+i*3, y, byte, 2, col_data);
				DEBUGWrite(dbgRenderer, DBG_MEMX+57+i, y, byte, col_data);
			}
		}
		y++;
		memaddr+= 16;
	}
	if(mouseZone == 2) {
		SDL_SetRenderDrawColor(dbgRenderer, 255, 255, 255, 80);
		SDL_RenderFillRect(dbgRenderer, dataZoneRect);
	}
}

#define VRAM_TYPES_COUNT (4)
static SDL_Colour vramColours[VRAM_TYPES_COUNT]= {
	col_vram_tilemap,
	col_vram_tiledata,
	col_vram_special,
	col_vram_other
};
static void
DEBUGRenderVRAM(int y, int vmemaddr)
{
	while (y < dbg_height - 2) {                                                   // To bottom of screen
		DEBUGVAddress(DBG_MEMX, y, vmemaddr & 0x1FFFF, col_label); // Show label.

		for (int i = 0; i < 16; i++) {
			int addr = (vmemaddr + i) & 0x1FFFF;
			int byte = video_space_read(addr);

			int type= video_get_address_type(addr) % VRAM_TYPES_COUNT;
			DEBUGNumber(DBG_MEMX + 6 + i * 3, y, byte, 2, vramColours[type]);
		}
		y++;
		vmemaddr += 16;
	}
}

// *******************************************************************************************
//
//									 Render Disassembly
//
// *******************************************************************************************
static void DEBUGRenderCode(int lines, int initialPC) {
	char buffer[48];
	char *label;

	for (int y = 0; y < lines; y++) { 							// Each line

		DEBUGAddress(DBG_ASMX, y, currentPCBank, initialPC, col_label);

		if(!isValidAddr(currentPCBank, initialPC)) {
			initialPC++;
			continue;
		}

		int size = disasm(initialPC, RAM, buffer, sizeof(buffer), currentPCBank);	// Disassemble code
		if(y == 0)
			disasmLine1Size= size;

		DEBUGString(dbgRenderer, DBG_ASMX+8+9+13, y, buffer, initialPC == pc ? col_highlight : col_data);

		for(int byteCount= 0; byteCount<size;byteCount++) {
			int byte= real_read6502(initialPC + byteCount, true, currentPCBank);
			DEBUGNumber(DBG_ASMX+8+byteCount*3, y, byte, 2, initialPC == pc ? col_highlight : col_data);
		}
		label= symbol_find_label(currentPCBank, initialPC);
		if(label)
			DEBUGString(dbgRenderer, DBG_ASMX+8+9, y, label, initialPC == pc ? col_highlight : col_data);
		initialPC += size;										// Forward to next
	}

	if(mouseZone == 1) {
		SDL_SetRenderDrawColor(dbgRenderer, 255, 255, 255, 80);
		SDL_RenderFillRect(dbgRenderer, codeZoneRect) ;
	}
}

// *******************************************************************************************
//
//									Render Register Display
//
// *******************************************************************************************

typedef struct {
	char *text;
	int xOffset;
	int yOffset;
} TRegisterLabelPos;
typedef struct {
	int xOffset;
	int yOffset;
} TRegisterValuePos;
typedef struct {
	int regCode;
	int width;
	int showChar;
	TRegisterLabelPos label;
	TRegisterValuePos value;
} TRegisterPos;

static TRegisterPos regs[]= {
	{ REG_P,   2, 0, { "P", DBG_REG+0, 0}, {DBG_REG+0, 1} },
	{ REG_P,  -1, 0, { "NVRBDIZC", DBG_REG+3, 0}, {DBG_REG+3, 1} },
	{ REG_A,   2, 1, { "A",  DBG_REG+0, 2}, {DBG_REG+3, 2} },
	{ REG_X,   2, 1, { "X",  DBG_REG+0, 3}, {DBG_REG+3, 3} },
	{ REG_Y,   2, 1, { "Y",  DBG_REG+0, 4}, {DBG_REG+3, 4} },
	{ REG_PC,  4, 0, { "PC", DBG_REG+0, 5}, {DBG_REG+3, 5} },
	{ REG_SP,  4, 0, { "SP", DBG_REG+0, 6}, {DBG_REG+3, 6} },

	{ REG_BKA, 2, 0, { "BKA", DBG_REG+0, 8}, {DBG_REG+4, 8} },
	{ REG_BKO, 2, 0, { "BKO", DBG_REG+7, 8}, {DBG_REG+11, 8} },

	{ REG_VA,  6, 0, { "VA",  DBG_REG+0, 10}, {DBG_REG+3, 10} },
	{ REG_VD0, 2, 0, { "VD0", DBG_REG+0, 11}, {DBG_REG+0, 12} },
	{ REG_VD1, 2, 0, { "VD1", DBG_REG+4, 11}, {DBG_REG+4, 12} },
	{ REG_VCT, 2, 0, { "VCT", DBG_REG+8, 11}, {DBG_REG+8, 12} },

	{ REG_R0,  4, 0, { "R0",  DBG_ZP_REG, 21+0}, {DBG_ZP_REG+4, 21+0} },
	{ REG_R1,  4, 0, { "R1",  DBG_ZP_REG, 21+1}, {DBG_ZP_REG+4, 21+1} },
	{ REG_R2,  4, 0, { "R2",  DBG_ZP_REG, 21+2}, {DBG_ZP_REG+4, 21+2} },
	{ REG_R3,  4, 0, { "R3",  DBG_ZP_REG, 21+3}, {DBG_ZP_REG+4, 21+3} },

	{ REG_R4,  4, 0, { "R4",  DBG_ZP_REG, 21+5}, {DBG_ZP_REG+4, 21+5} },
	{ REG_R5,  4, 0, { "R5",  DBG_ZP_REG, 21+6}, {DBG_ZP_REG+4, 21+6} },
	{ REG_R6,  4, 0, { "R6",  DBG_ZP_REG, 21+7}, {DBG_ZP_REG+4, 21+7} },
	{ REG_R7,  4, 0, { "R7",  DBG_ZP_REG, 21+8}, {DBG_ZP_REG+4, 21+8} },

	{ REG_R8,  4, 0, { "R8",  DBG_ZP_REG, 21+10}, {DBG_ZP_REG+4, 21+10} },
	{ REG_R9,  4, 0, { "R9",  DBG_ZP_REG, 21+11}, {DBG_ZP_REG+4, 21+11} },
	{ REG_R10, 4, 0, { "R10", DBG_ZP_REG, 21+12}, {DBG_ZP_REG+4, 21+12} },
	{ REG_R11, 4, 0, { "R11", DBG_ZP_REG, 21+13}, {DBG_ZP_REG+4, 21+13} },

	{ REG_R12, 4, 0, { "R12", DBG_ZP_REG, 21+15}, {DBG_ZP_REG+4, 21+15} },
	{ REG_R13, 4, 0, { "R13", DBG_ZP_REG, 21+16}, {DBG_ZP_REG+4, 21+16} },
	{ REG_R14, 4, 0, { "R14", DBG_ZP_REG, 21+17}, {DBG_ZP_REG+4, 21+17} },
	{ REG_R15, 4, 0, { "R15", DBG_ZP_REG, 21+18}, {DBG_ZP_REG+4, 21+18} },

	{ REG_x16, 4, 0, { "x16", DBG_ZP_REG, 21+20}, {DBG_ZP_REG+4, 21+20} },
	{ REG_x17, 4, 0, { "x17", DBG_ZP_REG, 21+21}, {DBG_ZP_REG+4, 21+21} },
	{ REG_x18, 4, 0, { "x18", DBG_ZP_REG, 21+22}, {DBG_ZP_REG+4, 21+22} },
	{ REG_x19, 4, 0, { "x19", DBG_ZP_REG, 21+23}, {DBG_ZP_REG+4, 21+23} },

	{ REG_BP0, 6, 0, { "BP0", DBG_BP_REG, 0}, {DBG_BP_REG+4, 0} },
	{ REG_BP1, 6, 0, { "BP1", DBG_BP_REG, 0+1}, {DBG_BP_REG+4, 0+1} },
	{ REG_BP2, 6, 0, { "BP2", DBG_BP_REG, 0+2}, {DBG_BP_REG+4, 0+2} },
	{ REG_BP3, 6, 0, { "BP3", DBG_BP_REG, 0+3}, {DBG_BP_REG+4, 0+3} },
	{ REG_BP4, 6, 0, { "BP4", DBG_BP_REG, 0+4}, {DBG_BP_REG+4, 0+4} },
	{ REG_BP5, 6, 0, { "BP5", DBG_BP_REG, 0+5}, {DBG_BP_REG+4, 0+5} },
	{ REG_BP6, 6, 0, { "BP6", DBG_BP_REG, 0+6}, {DBG_BP_REG+4, 0+6} },
	{ REG_BP7, 6, 0, { "BP7", DBG_BP_REG, 0+7}, {DBG_BP_REG+4, 0+7} },
	{ REG_BP8, 6, 0, { "BP8", DBG_BP_REG, 0+8}, {DBG_BP_REG+4, 0+8} },
	{ REG_BP9, 6, 0, { "BP9", DBG_BP_REG, 0+9}, {DBG_BP_REG+4, 0+9} },

	{ 0, 0, 0, { NULL, 0, 0}, {0, 0} }
};

int readVirtualRegister(int reg) {
	int reg_addr = 2 + reg * 2;
	return real_read6502(reg_addr+1, true, currentBank)*256+real_read6502(reg_addr, true, currentBank);
}

static void DEBUGRenderRegisters() {
	int value= 0;
	bool wannaShow;

	for(int idx= 0; regs[idx].regCode; idx++) {
		wannaShow= true;
		switch(regs[idx].regCode) {
			case REG_A: value= a; break;
			case REG_X: value= x; break;
			case REG_Y: value= y; break;
			case REG_P: value= status; break;
			case REG_PC: value= pc; break;
			case REG_SP: value= sp|0x100; break;

			case REG_BKA: value= memory_get_ram_bank(); break;
			case REG_BKO: value= memory_get_rom_bank(); break;

			case REG_VA: value= video_read(0, true) | (video_read(1, true)<<8) | (video_read(2, true)<<16); break;
			case REG_VD0: value= video_read(3, true); break;
			case REG_VD1: value= video_read(4, true); break;
			case REG_VCT: value= video_read(5, true); break;

			case REG_R0:
			case REG_R1:
			case REG_R2:
			case REG_R3:
			case REG_R4:
			case REG_R5:
			case REG_R6:
			case REG_R7:
			case REG_R8:
			case REG_R9:
			case REG_R10:
			case REG_R11:
			case REG_R12:
			case REG_R13:
			case REG_R14:
			case REG_R15:
			case REG_x16:
			case REG_x17:
			case REG_x18:
			case REG_x19: value= readVirtualRegister(regs[idx].regCode - REG_R0); break;

			case REG_BP0:
			case REG_BP1:
			case REG_BP2:
			case REG_BP3:
			case REG_BP4:
			case REG_BP5:
			case REG_BP6:
			case REG_BP7:
			case REG_BP8:
			case REG_BP9:
			{
				int bpIdx= regs[idx].regCode - REG_BP0;
				if(bpIdx < breakpointsCount) {
					value= breakpoints[bpIdx];
				} else {
					wannaShow= false;
				}
				break;
			}
		}
		if(wannaShow) {
			DEBUGString(dbgRenderer, regs[idx].label.xOffset, regs[idx].label.yOffset, regs[idx].label.text, col_label);
			DEBUGNumber(regs[idx].value.xOffset, regs[idx].value.yOffset, value, regs[idx].width, col_data);
			if(regs[idx].showChar)
				DEBUGWrite(dbgRenderer, regs[idx].value.xOffset + 3, regs[idx].value.yOffset, value, col_data);
		}
	}

}

// *******************************************************************************************
//
//									Render Top of Stack
//
// *******************************************************************************************

static void DEBUGRenderStack(int bytesCount) {
	int data= (sp-6) | 0x100;
	int y= 0;
	while (y < bytesCount) {
		DEBUGNumber(DBG_STCK, y, data & 0xFFFF, 4, (data & 0xFF) == sp ? col_highlight : col_label);
		int byte = real_read6502((data++) & 0xFFFF, false, 0);
		DEBUGNumber(DBG_STCK+5, y, byte, 2, col_data);
		DEBUGWrite(dbgRenderer, DBG_STCK+8, y, byte, col_data);
		y++;
		data= (data & 0xFF) | 0x100;
	}
}

// *******************************************************************************************
//
//							Render the emulator debugger display.
//
// *******************************************************************************************

void DEBUGRenderDisplay(int width, int height) {

	if (showFullDisplay) return;								// Not rendering debug.

	CON_DrawConsole(console);

	if(showFullConsole)
		return;

	SDL_Rect rc;
	/*
	rc.w = DBG_WIDTH * 6 * CHAR_SCALE;							// Erase background, set up rect
	rc.h = height;
	xPos = width-rc.w;yPos = 0; 								// Position screen
	rc.x = xPos;rc.y = yPos; 									// Set rectangle and black out.
	SDL_SetRenderDrawColor(dbgRenderer, 0, 0, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(dbgRenderer,&rc);
	*/
	rc.w = width;
	rc.h = height - con_height + 2;
	rc.x = rc.y = 0;
	SDL_SetRenderDrawColor(dbgRenderer, 0, 0, 255, SDL_ALPHA_OPAQUE);
	SDL_RenderFillRect(dbgRenderer, &rc);

	// Draw register name and values.
	DEBUGRenderRegisters();

	switch(layout) {
		case 1:
			codeZoneRect= &largeCodeZoneRect;
			dataZoneRect= &emptyZoneRect;
			DEBUGRenderCode(53, currentPC);
			break;

		default:
			codeZoneRect= &smallCodeZoneRect;
			dataZoneRect= &smallDataZoneRect;
			DEBUGRenderCode(20, currentPC);
			if (dumpmode == DDUMP_RAM)
				DEBUGRenderData(21, currentData);
			else
				DEBUGRenderVRAM(21, currentData);
	}

	DEBUGRenderStack(20);

	int mouseX, mouseY;
	char mouseCoord[30];
	SDL_GetMouseState(&mouseX, &mouseY);
	sprintf(mouseCoord, "%d %d", mouseX, mouseY);
	DT_DrawText2(dbgRenderer, mouseCoord, 0, win_width-50, win_height - 20, col_highlight);

}