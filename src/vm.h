#pragma once

#include <stdint.h>
#include <stddef.h>

#include "data_win.h"
#include "rvalue.h"
#include "utils.h"

// ===[ GML Data Types (4-bit type codes) ]===
#define GML_TYPE_DOUBLE   0x0
#define GML_TYPE_FLOAT    0x1
#define GML_TYPE_INT32    0x2
#define GML_TYPE_INT64    0x3
#define GML_TYPE_BOOL     0x4
#define GML_TYPE_VARIABLE 0x5
#define GML_TYPE_STRING   0x6
#define GML_TYPE_INT16    0xF

// ===[ Instance Types (signed 16-bit) ]===
#define INSTANCE_SELF      (-1)
#define INSTANCE_OTHER     (-2)
#define INSTANCE_ALL       (-3)
#define INSTANCE_NOONE     (-4)
#define INSTANCE_GLOBAL    (-5)
#define INSTANCE_LOCAL     (-7)
#define INSTANCE_STACKTOP  (-9)

// ===[ Variable Types (upper 5 bits of varRef, extracted with (varRef >> 24) & 0xF8) ]===
#define VARTYPE_ARRAY     0x00
#define VARTYPE_STACKTOP  0x80
#define VARTYPE_NORMAL    0xA0
#define VARTYPE_INSTANCE  0xE0

// ===[ GML Math Epsilon (used for floating-point comparisons) ]===
// The real GameMaker runner uses epsilon-based comparison for all numeric CMP operations.
// Default value matches the HTML5 runner's g_GMLMathEpsilon.
#define GML_MATH_EPSILON 1e-5

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

// ===[ CallFrame - Saved state for script-to-script calls ]===
typedef struct CallFrame {
    uint32_t savedIP;
    uint32_t savedCodeEnd;
    uint8_t* savedBytecodeBase;
    RValue* savedLocals;
    uint32_t savedLocalsCount;
    const char* savedCodeName;
    ArrayMapEntry* savedLocalArrayMap;
    RValue* savedScriptArgs;
    int32_t savedScriptArgCount;
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
#define VM_STACK_SIZE 16384

typedef struct {
    RValue slots[VM_STACK_SIZE];
    int32_t top;
} VMStack;

// Forward declaration for Runner
struct Runner;

// ===[ VMContext - Holds all VM state ]===
typedef struct VMContext {
    DataWin* dataWin;
    struct Runner* runner;
    uint8_t* bytecodeBase;
    uint32_t ip;
    uint32_t codeEnd;
    VMStack stack;
    RValue* localVars;
    uint32_t localVarCount;
    RValue* globalVars;
    uint32_t globalVarCount;
    int32_t selfId;
    int32_t otherId;
    struct Instance* currentInstance;
    struct Instance* otherInstance; // "other" instance for collision events
    CallFrame* callStack;
    int32_t callDepth;
    EnvFrame* envStack; // Environment stack for with-statements (PushEnv/PopEnv)
    const char* currentCodeName;
    // Array variable maps: key = ((int64_t)varID << 32) | (uint32_t)arrayIndex
    ArrayMapEntry* globalArrayMap;
    ArrayMapEntry* localArrayMap;
    // Tracks which global varIDs have array data (for array aliasing)
    struct { int32_t key; int32_t value; }* globalArrayVarTracker;
    RValue* scriptArgs;       // Arguments passed to current script (nullptr for non-script code)
    int32_t scriptArgCount;   // Number of arguments passed
    // funcName -> codeIndex hash map (stb_ds)
    struct { char* key; int32_t value; }* funcMap;
    // varName -> varID hash map for global variables (stb_ds)
    struct { char* key; int32_t value; }* globalVarNameMap;
    // "codeName\tfuncName" -> true, for deduplicating unknown function warnings
    StringBooleanEntry* loggedUnknownFuncs;
    // "codeName\tfuncName" -> true, for deduplicating stubbed function warnings
    StringBooleanEntry* loggedStubbedFuncs;
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* tilesToBeTraced;
    // Current event context (set by Runner_executeEvent, -1 when not in an event)
    int32_t currentEventType;
    int32_t currentEventSubtype;
    int32_t currentEventObjectIndex; // objectIndex of the object that owns the executing event handler
    bool traceEventInherited;
    bool hasFixedSeed;
    bool actionRelativeFlag; // D&D action relative flag (set by action_set_relative)
    // Cached varID for the built-in "creator" self variable (-1 if not found)
    int32_t creatorVarID;
    // Cross-reference map for disassembler: targetCodeIndex -> stb_ds array of callerCodeIndex
    struct { int32_t key; int32_t* value; }* crossRefMap;
} VMContext;

// ===[ Public API ]===
VMContext* VM_create(DataWin* dataWin);
RValue VM_executeCode(VMContext* ctx, int32_t codeIndex);
RValue VM_callCodeIndex(VMContext* ctx, int32_t codeIndex, RValue* args, int32_t argCount);
CodeLocals* VM_resolveCodeLocals(VMContext* ctx, const char* codeName);
void VM_free(VMContext* ctx);
bool VM_isObjectOrDescendant(DataWin* dataWin, int32_t objectIndex, int32_t targetObjectIndex);
void VM_buildCrossReferences(VMContext* ctx);
void VM_disassemble(VMContext* ctx, int32_t codeIndex);

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
