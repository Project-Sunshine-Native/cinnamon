#pragma once

#include "common.h"
#include <stdint.h>
#include <stddef.h>

#include "data_win.h"
#include "rvalue.h"
#include "utils.h"

// ===[ Instance Types (signed 16-bit) ]===
#define INSTANCE_SELF      (-1)
#define INSTANCE_OTHER     (-2)
#define INSTANCE_ALL       (-3)
#define INSTANCE_NOONE     (-4)
#define INSTANCE_GLOBAL    (-5)
#define INSTANCE_LOCAL     (-7)
#define INSTANCE_STACKTOP  (-9)
#define INSTANCE_ARG       (-15)

// ===[ Variable Types (upper 5 bits of varRef, extracted with (varRef >> 24) & 0xF8) ]===
#define VARTYPE_ARRAY     0x00
#define VARTYPE_STACKTOP  0x80
#define VARTYPE_NORMAL    0xA0
#define VARTYPE_INSTANCE  0xE0

// ===[ Room Constants ]===
#define ROOM_RESTARTGAME (-200) // The reason why it is -200 is because the GameMaker-HTML5 runner uses -200 too (see Globals.js)

// ===[ GML Math Epsilon (used for floating-point comparisons) ]===
// The real GameMaker runner uses epsilon-based comparison for all numeric CMP operations.
// Default value matches the HTML5 runner's g_GMLMathEpsilon (1e-5 for double precision).
// When using single-precision floats, we use 1e-4 to work around accumulated rounding errors from
// non-IEEE FPUs (example: PS2's R5900 which rounds toward zero instead of round-to-nearest) can
// exceed the default epsilon.
#ifdef USE_FLOAT_REALS
#define GML_MATH_EPSILON 1e-4
#else
#define GML_MATH_EPSILON 1e-5
#endif

// GMS 1.4 supports up to 16 arguments per script call
#define GML_MAX_ARGUMENTS 16

// ===[ Comparison Kinds ]===
#define CMP_LT  1
#define CMP_LTE 2
#define CMP_EQ  3
#define CMP_NEQ 4
#define CMP_GTE 5
#define CMP_GT  6

// ===[ Opcodes ]===
#define OP_CONV     0x07
#define OP_MUL      0x08
#define OP_DIV      0x09
#define OP_REM      0x0A
#define OP_MOD      0x0B
#define OP_ADD      0x0C
#define OP_SUB      0x0D
#define OP_AND      0x0E
#define OP_OR       0x0F
#define OP_XOR      0x10
#define OP_NEG      0x11
#define OP_NOT      0x12
#define OP_SHL      0x13
#define OP_SHR      0x14
#define OP_CMP      0x15
#define OP_POP      0x45
#define OP_PUSHI    0x84
#define OP_DUP      0x86
#define OP_RET      0x9C
#define OP_EXIT     0x9D
#define OP_POPZ     0x9E
#define OP_B        0xB6
#define OP_BT       0xB7
#define OP_BF       0xB8
#define OP_PUSHENV  0xBA
#define OP_POPENV   0xBB
#define OP_PUSH     0xC0
#define OP_PUSHLOC  0xC1
#define OP_PUSHGLB  0xC2
#define OP_PUSHBLTN 0xC3
#define OP_CALL     0xD9
#define OP_BREAK    0xFF

// ===[ Extended BREAK Sub-Opcodes (bytecode version 17+) ]===
// Encoded in bits 0-15 of the BREAK instruction (instrInstanceType field, as int16_t)
#define BREAK_CHKINDEX     (-1)  // Validate array index bounds
#define BREAK_PUSHAF       (-2)  // Pop array ref + index, push element (final dimension)
#define BREAK_POPAF        (-3)  // Pop value + array ref + index, store at index
#define BREAK_PUSHAC       (-4)  // Pop array ref + index, push sub-array ref (intermediate dimension)
#define BREAK_SETOWNER     (-5)  // Pop and discard (copy-on-write owner tracking)
#define BREAK_ISSTATICOK   (-6)  // Push bool: has static init already run for this function?
#define BREAK_SETSTATIC    (-7)  // Mark current function's static as initialized
#define BREAK_SAVEAREF     (-8)  // Save top-of-stack array ref for compound assignment
#define BREAK_RESTOREAREF  (-9)  // Push previously saved array ref

// ===[ Variable Types for V17 Array Access ]===
#define VARTYPE_ARRAYPUSHAF 0x10  // Push array reference (read context)
#define VARTYPE_ARRAYPOPAF  0x90  // Push array reference (write context)

// ===[ FuncCallCache - Cached resolution for CALL instructions ]===
// Avoids per-call string hash lookups in both the builtin map and funcMap.
// Resolved once during VM_create, then used directly by handleCall.
typedef struct {
    void* builtin; // cached BuiltinFunc pointer, or nullptr
    int32_t scriptCodeIndex; // cached script code index, or -1 if not a script
} FuncCallCache;

// ===[ LocalSlotEntry - varID -> localVars slot index (Bytecode Version 17+) ]===
// stb_ds hmap entry layout: one per CodeLocals, keyed by the local's shared varID (== CodeLocals.locals[i].index).
// Value is the slot position i within that code's localVars.
typedef struct {
    int32_t key;
    uint32_t value;
} LocalSlotEntry;

// ===[ CallFrame - Saved state for script-to-script calls ]===
typedef struct CallFrame {
    uint32_t savedIP;
    uint32_t savedCodeEnd;
    uint8_t* savedBytecodeBase;
    RValue* savedLocals;
    uint32_t savedLocalsCount;
    const char* savedCodeName;
    int32_t savedSavearefBalance;
    CodeLocals* savedCodeLocals;
    LocalSlotEntry* savedCodeLocalsSlotMap;
    RValue* savedScriptArgs;
    int32_t savedScriptArgCount;
    int32_t savedCurrentCodeIndex;
    struct CallFrame* parent;
} CallFrame;

// ===[ EnvFrame - Saved context for with-statement (PushEnv/PopEnv) ]===
typedef struct EnvFrame {
    struct Instance* savedInstance;
    struct Instance* savedOtherInstance; // Saved otherInstance to restore on PopEnv
    struct Instance** instanceList; // stb_ds array of matching instances (nullptr for single-instance)
    int32_t currentIndex;           // Current position in instanceList
    struct EnvFrame* parent;
} EnvFrame;

// ===[ VMStack - Upward-growing array of RValue slots ]===
#define VM_STACK_SIZE 1024

typedef struct {
    int32_t top;
    RValue slots[VM_STACK_SIZE];
} VMStack;

// Forward declarations
struct Runner;
typedef struct VMContext VMContext;

// ===[ Builtin Functions Manager ]===
typedef RValue (*BuiltinFunc)(VMContext* ctx, RValue* args, int32_t argCount);

typedef struct {
    char* key;
    BuiltinFunc value;
} BuiltinEntry;

// ===[ VMContext - Holds all VM state ]===
// Fields are ordered by access frequency so that the hottest data sits in the first bytes of the struct
// This way data can be kept "hot" in the CPU cache or, depending on the platform, in scratchpad RAM
typedef struct VMContext {
    // Hot: touched every instruction in the dispatch loop
    uint8_t* bytecodeBase;
    uint32_t ip;
    uint32_t codeEnd;
    RValue* localVars;
    uint32_t localVarCount;
    RValue* globalVars;
    uint32_t globalVarCount;
    struct Instance* currentInstance;
    struct Instance* otherInstance; // "other" instance for collision events
    DataWin* dataWin;
    struct Runner* runner;
    CodeLocals* currentCodeLocals;
    // BC17+: varID -> localVars slot lookup for the current code.
    // Points into codeLocalsSlotMaps, parallel to currentCodeLocals. Stays in sync with it.
    LocalSlotEntry* currentCodeLocalsSlotMap;
    FuncCallCache* funcCallCache;
    const char* currentCodeName;
    int32_t currentCodeIndex; // Index into code.entries for the currently executing code

    // Warm: touched on calls, variable resolution, event dispatch
    CallFrame* callStack;
    int32_t callDepth;
    EnvFrame* envStack; // Environment stack for with-statements (PushEnv/PopEnv)
    RValue* scriptArgs;       // Arguments passed to current script (nullptr for non-script code)
    int32_t scriptArgCount;   // Number of arguments passed
    int32_t selfId;
    int32_t otherId;
    // Current event context (set by Runner_executeEvent, -1 when not in an event)
    int32_t currentEventType;
    int32_t currentEventSubtype;
    int32_t currentEventObjectIndex; // objectIndex of the object that owns the executing event handler
    // Cached varID for the built-in "creator" self variable (-1 if not found)
    int32_t creatorVarID;
    uint32_t funcCallCacheCount;
    bool traceEventInherited;
    bool hasFixedSeed;
    bool actionRelativeFlag; // D&D action relative flag (set by action_set_relative)

    // V17+ extended BREAK opcode state
    bool* staticInitialized; // Per-code-entry flag for isstaticok/setstatic (allocated in VM_create)
    // BC17+: owner token set by BREAK_SETOWNER. Arrays whose .owner mismatches fork on write.
    void* currentArrayOwner;
    // SAVEAREF/RESTOREAREF balance tracker.
    int32_t savearefBalance;

    // Cold: init-only or rare lookups
    BuiltinEntry* builtinMap;
    bool registeredBuiltinFunctions;
    // funcName -> codeIndex hash map (stb_ds)
    struct { char* key; int32_t value; }* funcMap;
    // codeName -> CodeLocals* hash map (stb_ds)
    struct { char* key; CodeLocals* value; }* codeLocalsMap;
    // BC17+: parallel to dataWin->func.codeLocals[]. Each element is a hmap keyed by the local's CodeLocals.locals[i].index (== its shared varID), mapping to the slot position i
    // within that code's localVars array. Built once in VM_create and read O(1) at dispatch time.
    // In BC17, a single GML local can surface as several VARI chunk entries that share varID, so we key by varID (not VARI chunk index) to unify them to a single slot.
    LocalSlotEntry** codeLocalsSlotMaps;
    // varName -> varID hash map for global variables (stb_ds)
    struct { char* key; int32_t value; }* globalVarNameMap;
    // "codeName\tfuncName" -> true, for deduplicating unknown function warnings
    StringBooleanEntry* loggedUnknownFuncs;
    // "codeName\tfuncName" -> true, for deduplicating stubbed function warnings
    StringBooleanEntry* loggedStubbedFuncs;
    // Cross-reference map for disassembler: targetCodeIndex -> stb_ds array of callerCodeIndex
    struct { int32_t key; int32_t* value; }* crossRefMap;
    bool alwaysLogUnknownFunctions;
    bool alwaysLogStubbedFunctions;
#ifndef DISABLE_VM_TRACING
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* tilesToBeTraced;
    // Minimum frameCount before opcode/stack traces are emitted (default 0)
    int traceBytecodeAfterFrame;
#endif

    // Stack at the end because it is a big chunky boi (we don't want it pushing fields around)
    VMStack stack;
} VMContext;

// ===[ Public API ]===
VMContext* VM_create(DataWin* dataWin);
void VM_reset(VMContext* ctx);
RValue VM_executeCode(VMContext* ctx, int32_t codeIndex);
RValue VM_callCodeIndex(VMContext* ctx, int32_t codeIndex, RValue* args, int32_t argCount);
void VM_free(VMContext* ctx);
bool VM_isObjectOrDescendant(DataWin* dataWin, int32_t objectIndex, int32_t targetObjectIndex);
void VM_buildCrossReferences(VMContext* ctx);
void VM_disassemble(VMContext* ctx, int32_t codeIndex);
void VM_registerBuiltin(VMContext* ctx, const char* name, BuiltinFunc func);
BuiltinFunc VM_findBuiltin(VMContext* ctx, const char* name);
RValue VM_createArray(VMContext* ctx);
void VM_arraySet(VMContext* ctx, RValue* arrayRef, int32_t index, RValue val);

static const char* VM_getCallerName(VMContext* ctx) {
    return ctx->currentCodeName != nullptr ? ctx->currentCodeName : "<unknown>";
}

static char* VM_createDedupKey(const char* callerName, const char* funcName) {
    // Build dedup key: "callerName\tfuncName"
    size_t keyLen = strlen(callerName) + 1 + strlen(funcName) + 1;
    char* dedupKey = safeMalloc(keyLen);
    snprintf(dedupKey, keyLen, "%s\t%s", callerName, funcName);
    return dedupKey;
}
