#include "vm.h"
#include "vm_builtins.h"
#include "instance.h"
#include "runner.h"
#include "binary_utils.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"

__attribute__((weak)) void VM_platformBootLog(const char* message) {
    (void) message;
}

static void VM_bootLog(const char* message) {
    VM_platformBootLog(message);
}

static bool VM_shouldProfileBlackScreenBuiltins(VMContext* ctx) {
    if (ctx == NULL || ctx->currentCodeName == NULL) return false;
    return
        strncmp(ctx->currentCodeName, "gml_Object_obj_time_", 20) == 0 ||
        strncmp(ctx->currentCodeName, "gml_Object_obj_screen_", 22) == 0 ||
        strncmp(ctx->currentCodeName, "gml_Object_obj_f_gamestart_", 27) == 0;
}

// ===[ Stack Operations ]===

static bool shouldTraceStack(VMContext* ctx) {
    if (shlen(ctx->stackToBeTraced) == 0) return false;
    return shgeti(ctx->stackToBeTraced, "*") != -1 || shgeti(ctx->stackToBeTraced, ctx->currentCodeName) != -1;
}

static void stackPush(VMContext* ctx, RValue val) {
    require(VM_STACK_SIZE > ctx->stack.top);
    if (shouldTraceStack(ctx)) {
        char* valStr = RValue_toStringTyped(val);
        fprintf(stderr, "VM: [%s] PUSH %s [stack=%d -> %d]\n", ctx->currentCodeName, valStr, ctx->stack.top, ctx->stack.top + 1);
        free(valStr);
    }
    ctx->stack.slots[ctx->stack.top++] = val;
}

static RValue stackPop(VMContext* ctx) {
    require(ctx->stack.top > 0);
    RValue val = ctx->stack.slots[--ctx->stack.top];
    if (shouldTraceStack(ctx)) {
        char* valStr = RValue_toStringTyped(val);
        fprintf(stderr, "VM: [%s] POP  %s [stack=%d -> %d]\n", ctx->currentCodeName, valStr, ctx->stack.top + 1, ctx->stack.top);
        free(valStr);
    }
    return val;
}

static RValue* stackPeek(VMContext* ctx) {
    require(ctx->stack.top > 0);
    return &ctx->stack.slots[ctx->stack.top - 1];
}

// ===[ Instruction Decoding ]===

static uint8_t instrOpcode(uint32_t instr) {
    return (instr >> 24) & 0xFF;
}

static uint8_t instrType1(uint32_t instr) {
    return (instr >> 16) & 0xF;
}

static uint8_t instrType2(uint32_t instr) {
    return (instr >> 20) & 0xF;
}

static int16_t instrInstanceType(uint32_t instr) {
    return (int16_t) (instr & 0xFFFF);
}

static uint8_t instrCmpKind(uint32_t instr) {
    return (instr >> 8) & 0xFF;
}

static bool instrHasExtraData(uint32_t instr) {
    return (instr & 0x40000000) != 0;
}

// Jump offset for branch instructions: sign-extend 23 bits, multiply by 4
static int32_t instrJumpOffset(uint32_t instr) {
    return ((int32_t) (instr << 9)) >> 7;
}

static uint32_t extraDataSize(uint8_t type1) {
    switch (type1) {
        case GML_TYPE_DOUBLE: return 8;
        case GML_TYPE_INT64:  return 8;
        case GML_TYPE_FLOAT:  return 4;
        case GML_TYPE_INT32:  return 4;
        case GML_TYPE_BOOL:   return 4;
        case GML_TYPE_VARIABLE: return 4;
        case GML_TYPE_STRING: return 4;
        case GML_TYPE_INT16:  return 0;
        default:              return 0;
    }
}

// ===[ Reference Chain Resolution ]===

// Walks reference chains from the bytecode buffer and builds hash maps
// mapping absolute file offsets to resolved operand values.
// The bytecode buffer stays completely read-only.
// Patches bytecode operands in-place so that variable/function reference chain deltas
// are replaced with resolved indices. This avoids needing hash map lookups at runtime.
static void patchReferenceOperands(VMContext* ctx) {
    DataWin* dataWin = ctx->dataWin;
    uint8_t* buf = dataWin->bytecodeBuffer;
    size_t base = dataWin->bytecodeBufferBase;

    // Patch variable operands: replace delta with varIdx (preserving upper 5 bits)
    repeat(dataWin->vari.variableCount, varIdx) {
        Variable* v = &dataWin->vari.variables[varIdx];
        if (v->occurrences == 0) continue;

        uint32_t addr = v->firstAddress;
        repeat(v->occurrences, occ) {
            uint32_t operandAddr = addr + 4;
            uint32_t operand = BinaryUtils_readUint32(&buf[operandAddr - base]);
            uint32_t delta = operand & 0x07FFFFFF;
            uint32_t upperBits = operand & 0xF8000000;

            // Patch in-place: upper bits preserved, lower 27 = varIdx
            BinaryUtils_writeUint32(&buf[operandAddr - base], upperBits | (varIdx & 0x07FFFFFF));

            if (v->occurrences > occ + 1) {
                addr += delta;
            }
        }
    }

    // Patch function operands: replace delta with funcIdx
    repeat(dataWin->func.functionCount, funcIdx) {
        Function* f = &dataWin->func.functions[funcIdx];
        if (f->occurrences == 0) continue;

        uint32_t addr = f->firstAddress;
        repeat(f->occurrences, occ) {
            uint32_t operandAddr = addr + 4;
            uint32_t operand = BinaryUtils_readUint32(&buf[operandAddr - base]);
            uint32_t delta = operand & 0x07FFFFFF;

            // Patch in-place: store funcIdx directly
            BinaryUtils_writeUint32(&buf[operandAddr - base], funcIdx);

            if (f->occurrences > occ + 1) {
                addr += delta;
            }
        }
    }
}

// Resolve a variable operand: returns upper bits | varIndex (read directly from patched bytecode)
static uint32_t resolveVarOperand(const uint8_t* extraData) {
    return BinaryUtils_readUint32(extraData);
}

// Resolve a function operand: returns funcIndex (read directly from patched bytecode)
static uint32_t resolveFuncOperand(const uint8_t* extraData) {
    return BinaryUtils_readUint32(extraData);
}

// ===[ Array Map Helpers ]===

// Key encoding for array maps: upper 32 bits = varID, lower 32 bits = array index
static int64_t arrayMapKey(int32_t varID, int32_t arrayIndex) {
    return ((int64_t) varID << 32) | (uint32_t) arrayIndex;
}

// Read from an array map, returning default RValue_makeReal(0.0) if not found
// Returns a non-owning copy: the array map retains ownership of any owned strings.
static RValue arrayMapGet(ArrayMapEntry* map, int32_t varID, int32_t arrayIndex) {
    int64_t k = arrayMapKey(varID, arrayIndex);
    ptrdiff_t idx = hmgeti(map, k);
    if (0 > idx) return RValue_makeReal(0.0);
    RValue result = map[idx].value;
    result.ownsString = false;
    return result;
}

// Write to an array map
static void arrayMapSet(ArrayMapEntry** map, int32_t varID, int32_t arrayIndex, RValue val) {
    int64_t k = arrayMapKey(varID, arrayIndex);
    // Free old value if it exists
    ptrdiff_t idx = hmgeti(*map, k);
    if (idx >= 0) {
        RValue_free(&(*map)[idx].value);
    }
    // If storing a non-owning string, make an owning copy
    if (val.type == RVALUE_STRING && !val.ownsString && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    }
    hmput(*map, k, val);
}

// ===[ Array Alias Resolution ]===

#define MAX_ARRAY_ALIAS_HOPS 16

// Follows RVALUE_ARRAY_REF chain in scalar variable slots to find the actual source varID.
// Returns the resolved varID (which may be the same as the input if no alias exists).
static int32_t resolveArrayAlias(RValue* vars, uint32_t varCount, int32_t varID) {
    int32_t current = varID;
    int hops = 0;
    while (varCount > (uint32_t) current && vars[current].type == RVALUE_ARRAY_REF) {
        current = vars[current].int32;
        hops++;
        if (hops >= MAX_ARRAY_ALIAS_HOPS) {
            fprintf(stderr, "VM: resolveArrayAlias exceeded %d hops starting from varID %d (circular alias chain?)\n", MAX_ARRAY_ALIAS_HOPS, varID);
            abort();
        }
    }
    return current;
}

// Hashmap version of resolveArrayAlias for sparse self vars (SelfVarEntry* hashmap).
static int32_t resolveArrayAliasHm(SelfVarEntry* vars, int32_t varID) {
    int32_t current = varID;
    int hops = 0;
    while (true) {
        ptrdiff_t idx = hmgeti(vars, current);
        if (0 > idx || vars[idx].value.type != RVALUE_ARRAY_REF) break;
        current = vars[idx].value.int32;
        if (++hops >= MAX_ARRAY_ALIAS_HOPS) {
            fprintf(stderr, "VM: resolveArrayAliasHm exceeded %d hops starting from varID %d (circular alias chain?)\n", MAX_ARRAY_ALIAS_HOPS, varID);
            abort();
        }
    }
    return current;
}

// ===[ Trace Helpers ]===

/**
 * @brief Checks if a variable access should be traced.
 *
 * Matches the trace map entries in order: wildcard "*", bare scope name (e.g. "obj_player" or "global"),
 * alternate scope name (e.g. "self" for any instance), or qualified "scope.var" format
 * (e.g. "obj_player.x", "global.hp", "self.x"). Short-circuits before formatting
 * the qualified name when possible.
 *
 * @param traceMap The string-boolean hash map of trace filters (from --trace-variable-reads/writes).
 * @param scopeName The scope of the variable: an object name (e.g. "obj_player") or "global".
 * @param altScopeName An alternate scope name to also match (e.g. "self" for instance variables), or nullptr.
 * @param varName The variable name being accessed (e.g. "x").
 * @return true if the access matches a trace filter and should be logged.
 */
static bool shouldTraceVariable(StringBooleanEntry* traceMap, const char* scopeName, const char* altScopeName, const char* varName) {
    if (shlen(traceMap) == 0) return false;
    if (shgeti(traceMap, "*") != -1) return true;
    if (shgeti(traceMap, scopeName) != -1) return true;
    if (altScopeName != nullptr && shgeti(traceMap, altScopeName) != -1) return true;
    char formatted[strlen(scopeName) + 1 + strlen(varName) + 1];
    snprintf(formatted, sizeof(formatted), "%s.%s", scopeName, varName);
    if (shgeti(traceMap, formatted) != -1) return true;
    if (altScopeName != nullptr) {
        char altFormatted[strlen(altScopeName) + 1 + strlen(varName) + 1];
        snprintf(altFormatted, sizeof(altFormatted), "%s.%s", altScopeName, varName);
        if (shgeti(traceMap, altFormatted) != -1) return true;
    }
    return false;
}

// ===[ Array Access Helpers ]===

typedef struct {
    int32_t arrayIndex; // -1 when not an array access
    int32_t instanceType; // Instance type from stack (for VARTYPE_ARRAY / VARTYPE_STACKTOP)
    bool isArray;
    bool hasInstanceType; // true when instanceType was popped from stack
} ArrayAccess;

// Pops array index (and optional stacktop value) from the stack if the varRef
// indicates an array or stacktop access. Returns { .arrayIndex = -1, .isArray = false }
// for plain variable access.
static ArrayAccess popArrayAccess(VMContext* ctx, uint32_t varRef) {
    uint8_t varType = (varRef >> 24) & 0xF8;
    if (varType == VARTYPE_ARRAY) {
        // For array reads, GMS pushes: instanceType then arrayIndex (arrayIndex on top)
        RValue indexVal = stackPop(ctx);
        int32_t arrayIndex = RValue_toInt32(indexVal);
        RValue_free(&indexVal);

        RValue instTypeVal = stackPop(ctx);
        int32_t instanceType = RValue_toInt32(instTypeVal);
        RValue_free(&instTypeVal);

        return (ArrayAccess){ .arrayIndex = arrayIndex, .instanceType = instanceType, .isArray = true, .hasInstanceType = true };
    }
    if (varType == VARTYPE_STACKTOP) {
        RValue stacktop = stackPop(ctx);
        int32_t instanceType = RValue_toInt32(stacktop);
        RValue_free(&stacktop);
        return (ArrayAccess){ .arrayIndex = -1, .isArray = false, .hasInstanceType = true, .instanceType = instanceType };
    }
    return (ArrayAccess){ .arrayIndex = -1, .isArray = false, .hasInstanceType = false };
}

// ===[ Variable Resolution ]===
static const char* instanceTypeName(int32_t instanceType) {
    switch (instanceType) {
        case INSTANCE_SELF: return "self";
        case INSTANCE_OTHER: return "other";
        case INSTANCE_GLOBAL: return "global";
        case INSTANCE_LOCAL: return "local";
        default: return "instance";
    }
}

static Variable* resolveVarDef(VMContext* ctx, uint32_t varRef) {
    uint32_t varIndex = varRef & 0x07FFFFFF;
    require(ctx->dataWin->vari.variableCount > varIndex);
    Variable* varDef = &ctx->dataWin->vari.variables[varIndex];
    return varDef;
}

// Finds an active instance by target value.
// target >= 100000: instance ID (find specific instance)
// target >= 0 && target < 100000: object index (find first instance of that object, checking parent chains)
static Instance* findInstanceByTarget(VMContext* ctx, int32_t target) {
    Runner* runner = (Runner*) ctx->runner;
    int32_t instanceCount = (int32_t) arrlen(runner->instances);

    if (target >= 100000) {
        // Instance ID - find specific instance
        for (int32_t i = 0; instanceCount > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active && (int32_t) inst->instanceId == target) return inst;
        }
        return nullptr;
    }

    // Object index - find first matching instance, checking parent chains
    for (int32_t i = 0; instanceCount > i; i++) {
        Instance* inst = runner->instances[i];
        if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, target)) return inst;
    }
    return nullptr;
}

static RValue resolveVariableRead(VMContext* ctx, int32_t instanceType, uint32_t varRef) {
    Variable* varDef = resolveVarDef(ctx, varRef);

    ArrayAccess access = popArrayAccess(ctx, varRef);

    // Use instance type from stack when available (VARTYPE_ARRAY / VARTYPE_STACKTOP)
    int32_t originalInstanceType = instanceType;
    if (access.hasInstanceType) {
        instanceType = access.instanceType;
    }

    // Resolve target instance for object/instance references (instanceType >= 0)
    Instance* targetInstance = (Instance*) ctx->currentInstance;
    if (instanceType >= 0) {
        targetInstance = findInstanceByTarget(ctx, instanceType);
        if (targetInstance == nullptr) {
            uint8_t varType = (varRef >> 24) & 0xF8;
            const char* varTypeName = varType == VARTYPE_ARRAY ? "ARRAY" : varType == VARTYPE_STACKTOP ? "STACKTOP" : varType == VARTYPE_NORMAL ? "NORMAL" : varType == VARTYPE_INSTANCE ? "INSTANCE" : "UNKNOWN";
            if (instanceType < 100000 && (uint32_t) instanceType < ctx->dataWin->objt.count) {
                GameObject* gameObject = &ctx->dataWin->objt.objects[instanceType];
                fprintf(stderr, "VM: [%s] READ var '%s' on object index %d (%s) but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, gameObject->name, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
            } else {
                fprintf(stderr, "VM: [%s] READ var '%s' on instance %d but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
            }
            return RValue_makeReal(0.0);
        }
    } else if (instanceType == INSTANCE_OTHER) {
        if (ctx->otherInstance != nullptr) {
            targetInstance = (Instance*) ctx->otherInstance;
        }
    }

    // Check for built-in variable (varID == -6 sentinel)
    if (varDef->varID == -6) {
        // For object/instance references, temporarily swap currentInstance so VMBuiltins reads the correct instance
        Instance* savedInstance = (Instance*) ctx->currentInstance;
        bool needsInstanceSwap = (instanceType >= 0) || (instanceType == INSTANCE_OTHER);
        if (needsInstanceSwap) ctx->currentInstance = targetInstance;
        RValue result = VMBuiltins_getVariable(ctx, varDef->name, access.arrayIndex);
        if (needsInstanceSwap) ctx->currentInstance = savedInstance;

        // Trace built-in variable reads
        if (instanceType == INSTANCE_GLOBAL) {
            if (shouldTraceVariable(ctx->varReadsToBeTraced, "global", nullptr, varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(result);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] READ global.%s[%d] -> %s (builtin)\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString);
                } else {
                    fprintf(stderr, "VM: [%s] READ global.%s -> %s (builtin)\n", ctx->currentCodeName, varDef->name, rvalueAsString);
                }
                free(rvalueAsString);
            }
        } else if (targetInstance != nullptr) {
            const char* objName = ctx->dataWin->objt.objects[targetInstance->objectIndex].name;
            if (shouldTraceVariable(ctx->varReadsToBeTraced, objName, "self", varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(result);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] READ %s.%s[%d] -> %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, access.arrayIndex, rvalueAsString, targetInstance->instanceId);
                } else {
                    fprintf(stderr, "VM: [%s] READ %s.%s -> %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, rvalueAsString, targetInstance->instanceId);
                }
                free(rvalueAsString);
            }
        }

        return result;
    }

    // Check for array access
    if (access.isArray) {
        switch (instanceType) {
            case INSTANCE_LOCAL:
                return arrayMapGet(ctx->localArrayMap, varDef->varID, access.arrayIndex);
            case INSTANCE_GLOBAL: {
                int32_t resolvedVarID = resolveArrayAlias(ctx->globalVars, ctx->globalVarCount, varDef->varID);
                RValue result = arrayMapGet(ctx->globalArrayMap, resolvedVarID, access.arrayIndex);
                if (shouldTraceVariable(ctx->varReadsToBeTraced, "global", nullptr, varDef->name)) {
                    char* rvalueAsString = RValue_toStringTyped(result);
                    if (access.hasInstanceType && originalInstanceType != instanceType) {
                        fprintf(stderr, "VM: [%s] READ global.%s[%d] -> %s (resolved from stack, instruction said: %s)\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString, instanceTypeName(originalInstanceType));
                    } else {
                        fprintf(stderr, "VM: [%s] READ global.%s[%d] -> %s\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString);
                    }
                    free(rvalueAsString);
                }
                return result;
            }
            case INSTANCE_SELF:
            default: {
                Instance* inst = targetInstance;
                if (inst != nullptr) {
                    int32_t resolvedVarID = resolveArrayAliasHm(inst->selfVars, varDef->varID);
                    RValue result = arrayMapGet(inst->selfArrayMap, resolvedVarID, access.arrayIndex);
                    if (shouldTraceVariable(ctx->varReadsToBeTraced, ctx->dataWin->objt.objects[inst->objectIndex].name, "self", varDef->name)) {
                        char* rvalueAsString = RValue_toStringTyped(result);
                        if (access.hasInstanceType && originalInstanceType != instanceType) {
                            fprintf(stderr, "VM: [%s] READ %s.%s[%d] -> %s (instanceId=%d) (resolved from stack, instruction said: %s)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, access.arrayIndex, rvalueAsString, inst->instanceId, instanceTypeName(originalInstanceType));
                        } else {
                            fprintf(stderr, "VM: [%s] READ %s.%s[%d] -> %s (instanceId=%d)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, access.arrayIndex, rvalueAsString, inst->instanceId);
                        }
                        free(rvalueAsString);
                    }
                    return result;
                }
                uint8_t varType = (varRef >> 24) & 0xF8;
                const char* varTypeName = varType == VARTYPE_ARRAY ? "ARRAY" : varType == VARTYPE_STACKTOP ? "STACKTOP" : varType == VARTYPE_NORMAL ? "NORMAL" : varType == VARTYPE_INSTANCE ? "INSTANCE" : "UNKNOWN";
                fprintf(stderr, "VM: [%s] Array read on self var '%s' but no current instance (instanceType=%d, varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID);
                return RValue_makeReal(0.0);
            }
        }
    }

    RValue result;
    switch (instanceType) {
        case INSTANCE_LOCAL:
            require(ctx->localVarCount > (uint32_t) varDef->varID);
            result = ctx->localVars[varDef->varID];
            break;
        case INSTANCE_GLOBAL:
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            // If the scalar slot already has an array ref, return it as-is
            if (ctx->globalVars[varDef->varID].type == RVALUE_ARRAY_REF) {
                result = ctx->globalVars[varDef->varID];
            } else if (hmgeti(ctx->globalArrayVarTracker, varDef->varID) >= 0) {
                // Variable has array data but scalar slot is uninitialized - return a self-ref
                result = RValue_makeArrayRef(varDef->varID);
            } else {
                result = ctx->globalVars[varDef->varID];
            }
            break;
        case INSTANCE_SELF:
        default: {
            // Use target instance's sparse selfVars hashmap
            RValue selfVal = Instance_getSelfVar(targetInstance, varDef->varID);
            if (selfVal.type == RVALUE_ARRAY_REF) {
                result = selfVal;
            } else if (hmgeti(targetInstance->selfArrayVarTracker, varDef->varID) >= 0) {
                result = RValue_makeArrayRef(varDef->varID);
            } else {
                result = selfVal;
            }
            break;
        }
    }
    // Return a non-owning copy: the variable slot retains ownership
    result.ownsString = false;

    // Read tracing for scalar variables
    if (instanceType == INSTANCE_GLOBAL) {
        if (shouldTraceVariable(ctx->varReadsToBeTraced, "global", nullptr, varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(result);
            fprintf(stderr, "VM: [%s] READ global.%s -> %s\n", ctx->currentCodeName, varDef->name, rvalueAsString);
            free(rvalueAsString);
        }
    } else if (instanceType == INSTANCE_SELF || instanceType >= 0) {
        Instance* inst = targetInstance;
        if (inst != nullptr && shouldTraceVariable(ctx->varReadsToBeTraced, ctx->dataWin->objt.objects[inst->objectIndex].name, "self", varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(result);
            fprintf(stderr, "VM: [%s] READ %s.%s -> %s (instanceId=%d)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, rvalueAsString, inst->instanceId);
            free(rvalueAsString);
        }
    }

    return result;
}

// Helper: write a variable value to a single specific instance (always copies, never moves the original val)
static void writeSingleInstanceVariable(VMContext* ctx, Instance* inst, Variable* varDef, ArrayAccess* access, RValue val) {
    // Built-in variable (varID == -6 sentinel)
    if (varDef->varID == -6) {
        Instance* savedInstance = (Instance*) ctx->currentInstance;
        ctx->currentInstance = inst;
        VMBuiltins_setVariable(ctx, varDef->name, val, access->arrayIndex);
        ctx->currentInstance = savedInstance;
        return;
    }

    // Array write
    if (access->isArray) {
        int32_t resolvedVarID = resolveArrayAliasHm(inst->selfVars, varDef->varID);
        RValue valCopy = (val.type == RVALUE_STRING && val.string != nullptr) ? RValue_makeOwnedString(safeStrdup(val.string)) : val;
        arrayMapSet(&inst->selfArrayMap, resolvedVarID, access->arrayIndex, valCopy);
        hmput(inst->selfArrayVarTracker, resolvedVarID, 1);
        return;
    }

    // Scalar write
    Instance_setSelfVar(inst, varDef->varID, val);
}

static void resolveVariableWrite(VMContext* ctx, int32_t instanceType, uint32_t varRef, RValue val) {
    Variable* varDef = resolveVarDef(ctx, varRef);

    ArrayAccess access = popArrayAccess(ctx, varRef);

    // Use instance type from stack when available (VARTYPE_ARRAY / VARTYPE_STACKTOP)
    int32_t originalInstanceType = instanceType;
    if (access.hasInstanceType) {
        instanceType = access.instanceType;
    }

    // GML: writing through an object reference (obj_foo.var = val) sets the variable on ALL instances of that object
    if (instanceType >= 0 && 100000 > instanceType) {
        Runner* runner = (Runner*) ctx->runner;
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        bool found = false;
        repeat(instanceCount, i) {
            Instance* inst = runner->instances[i];
            if (!inst->active || !VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, instanceType)) continue;
            found = true;
            writeSingleInstanceVariable(ctx, inst, varDef, &access, val);
            if (shouldTraceVariable(ctx->varWritesToBeTraced, ctx->dataWin->objt.objects[inst->objectIndex].name, "self", varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(val);
                fprintf(stderr, "VM: [%s] WRITE %s.%s = %s (instanceId=%d, all-instances object write)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, rvalueAsString, inst->instanceId);
                free(rvalueAsString);
            }
        }
        if (!found) {
            if (ctx->dataWin->objt.count > (uint32_t) instanceType) {
                GameObject* gameObject = &ctx->dataWin->objt.objects[instanceType];
                char* valAsString = RValue_toString(val);
                fprintf(stderr, "VM: [%s] WRITE var '%s' on object %d (%s) but no instances found (value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, gameObject->name, valAsString);
                free(valAsString);
            }
        }
        RValue_free(&val);
        return;
    }

    // Resolve target instance for instance ID references (instanceType >= 100000) or special types
    Instance* targetInstance = (Instance*) ctx->currentInstance;
    if (instanceType >= 0) {
        targetInstance = findInstanceByTarget(ctx, instanceType);
        if (targetInstance == nullptr) {
            uint8_t varType = (varRef >> 24) & 0xF8;
            const char* varTypeName = varType == VARTYPE_ARRAY ? "ARRAY" : varType == VARTYPE_STACKTOP ? "STACKTOP" : varType == VARTYPE_NORMAL ? "NORMAL" : varType == VARTYPE_INSTANCE ? "INSTANCE" : "UNKNOWN";
            char* valAsString = RValue_toString(val);
            fprintf(stderr, "VM: [%s] WRITE var '%s' on instance %d but no instance found (varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID, valAsString);
            free(valAsString);
            return;
        }
    } else if (instanceType == INSTANCE_OTHER) {
        if (ctx->otherInstance != nullptr) {
            targetInstance = (Instance*) ctx->otherInstance;
        }
    }

    // Check for built-in variable (varID == -6 sentinel)
    if (varDef->varID == -6) {
        // For object/instance references, temporarily swap currentInstance so VMBuiltins writes the correct instance
        Instance* savedInstance = (Instance*) ctx->currentInstance;
        bool needsInstanceSwap = (instanceType >= 0) || (instanceType == INSTANCE_OTHER);
        if (needsInstanceSwap) ctx->currentInstance = targetInstance;
        VMBuiltins_setVariable(ctx, varDef->name, val, access.arrayIndex);
        if (needsInstanceSwap) ctx->currentInstance = savedInstance;

        // Trace built-in variable writes
        if (instanceType == INSTANCE_GLOBAL) {
            if (shouldTraceVariable(ctx->varWritesToBeTraced, "global", nullptr, varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(val);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] WRITE global.%s[%d] = %s (builtin)\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString);
                } else {
                    fprintf(stderr, "VM: [%s] WRITE global.%s = %s (builtin)\n", ctx->currentCodeName, varDef->name, rvalueAsString);
                }
                free(rvalueAsString);
            }
        } else if (targetInstance != nullptr) {
            const char* objName = ctx->dataWin->objt.objects[targetInstance->objectIndex].name;
            if (shouldTraceVariable(ctx->varWritesToBeTraced, objName, "self", varDef->name)) {
                char* rvalueAsString = RValue_toStringTyped(val);
                if (access.arrayIndex != -1) {
                    fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, access.arrayIndex, rvalueAsString, targetInstance->instanceId);
                } else {
                    fprintf(stderr, "VM: [%s] WRITE %s.%s = %s (instanceId=%d) (builtin)\n", ctx->currentCodeName, objName, varDef->name, rvalueAsString, targetInstance->instanceId);
                }
                free(rvalueAsString);
            }
        }

        return;
    }

    // Check for array access
    if (access.isArray) {
        switch (instanceType) {
            case INSTANCE_LOCAL:
                arrayMapSet(&ctx->localArrayMap, varDef->varID, access.arrayIndex, val);
                return;
            case INSTANCE_GLOBAL: {
                int32_t resolvedVarID = resolveArrayAlias(ctx->globalVars, ctx->globalVarCount, varDef->varID);
                arrayMapSet(&ctx->globalArrayMap, resolvedVarID, access.arrayIndex, val);
                hmput(ctx->globalArrayVarTracker, resolvedVarID, 1);
                if (shouldTraceVariable(ctx->varWritesToBeTraced, "global", nullptr, varDef->name)) {
                    char* rvalueAsString = RValue_toStringTyped(val);
                    if (access.hasInstanceType && originalInstanceType != instanceType) {
                        fprintf(stderr, "VM: [%s] WRITE global.%s[%d] = %s (resolved from stack, instruction said: %s)\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString, instanceTypeName(originalInstanceType));
                    } else {
                        fprintf(stderr, "VM: [%s] WRITE global.%s[%d] = %s\n", ctx->currentCodeName, varDef->name, access.arrayIndex, rvalueAsString);
                    }
                    free(rvalueAsString);
                }
                return;
            }
            case INSTANCE_SELF:
            default: {
                Instance* inst = targetInstance;
                if (inst != nullptr) {
                    int32_t resolvedVarID = resolveArrayAliasHm(inst->selfVars, varDef->varID);
                    arrayMapSet(&inst->selfArrayMap, resolvedVarID, access.arrayIndex, val);
                    hmput(inst->selfArrayVarTracker, resolvedVarID, 1);
                    if (shouldTraceVariable(ctx->varWritesToBeTraced, ctx->dataWin->objt.objects[inst->objectIndex].name, "self", varDef->name)) {
                        char* rvalueAsString = RValue_toStringTyped(val);
                        if (access.hasInstanceType && originalInstanceType != instanceType) {
                            fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s (instanceId=%d) (resolved from stack, instruction said: %s)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, access.arrayIndex, rvalueAsString, inst->instanceId, instanceTypeName(originalInstanceType));
                        } else {
                            fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s (instanceId=%d)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, access.arrayIndex, rvalueAsString, inst->instanceId);
                        }
                        free(rvalueAsString);
                    }
                    return;
                }
                uint8_t varType = (varRef >> 24) & 0xF8;
                const char* varTypeName = varType == VARTYPE_ARRAY ? "ARRAY" : varType == VARTYPE_STACKTOP ? "STACKTOP" : varType == VARTYPE_NORMAL ? "NORMAL" : varType == VARTYPE_INSTANCE ? "INSTANCE" : "UNKNOWN";
                char* valAsString = RValue_toString(val);
                fprintf(stderr, "VM: [%s] Array write on self var '%s' but no current instance (instanceType=%d, varType=%s, isArray=%s, originalInstanceType=%d, hasInstanceType=%s, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, instanceType, varTypeName, access.isArray ? "true" : "false", originalInstanceType, access.hasInstanceType ? "true" : "false", varDef->varID, valAsString);
                free(valAsString);
                return;
            }
        }
    }

    bool shouldLogGlobal = false;
    bool shouldLogInstance = false;

    switch (instanceType) {
        case INSTANCE_LOCAL: {
            require(ctx->localVarCount > (uint32_t) varDef->varID);
            RValue* dest = &ctx->localVars[varDef->varID];
            RValue_free(dest);
            if (val.type == RVALUE_STRING && !val.ownsString && val.string != nullptr) {
                *dest = RValue_makeOwnedString(safeStrdup(val.string));
            } else {
                *dest = val;
            }
            return;
        }
        case INSTANCE_GLOBAL: {
            require(ctx->globalVarCount > (uint32_t) varDef->varID);
            shouldLogGlobal = shouldTraceVariable(ctx->varWritesToBeTraced, "global", nullptr, varDef->name);
            RValue* dest = &ctx->globalVars[varDef->varID];
            RValue_free(dest);
            if (val.type == RVALUE_STRING && !val.ownsString && val.string != nullptr) {
                *dest = RValue_makeOwnedString(safeStrdup(val.string));
            } else {
                *dest = val;
            }
            if (shouldLogGlobal) {
                char* rvalueAsString = RValue_toStringTyped(*dest);
                fprintf(stderr, "VM: [%s] WRITE global.%s = %s\n", ctx->currentCodeName, varDef->name, rvalueAsString);
                free(rvalueAsString);
            }
            return;
        }
        case INSTANCE_SELF:
        default: {
            // Self or object/instance reference - use sparse hashmap
            Instance* inst = targetInstance;
            shouldLogInstance = shouldTraceVariable(ctx->varWritesToBeTraced, ctx->dataWin->objt.objects[inst->objectIndex].name, "self", varDef->name);
            Instance_setSelfVar(inst, varDef->varID, val);
            if (shouldLogInstance) {
                RValue written = Instance_getSelfVar(inst, varDef->varID);
                char* rvalueAsString = RValue_toStringTyped(written);
                GameObject* obj = &ctx->dataWin->objt.objects[inst->objectIndex];
                fprintf(stderr, "VM: [%s] WRITE %s.%s = %s (instanceId=%d)\n", ctx->currentCodeName, obj->name, varDef->name, rvalueAsString, inst->instanceId);
                free(rvalueAsString);
            }
            // val ownership transferred to Instance_setSelfVar, don't free here
            return;
        }
    }
}

// ===[ Type Conversion ]===

static RValue convertValue(RValue val, uint8_t targetType) {
    switch (targetType) {
        case GML_TYPE_DOUBLE:
            return RValue_makeReal(RValue_toReal(val));
        case GML_TYPE_FLOAT:
            return RValue_makeReal((double) (float) RValue_toReal(val));
        case GML_TYPE_INT32:
            return RValue_makeInt32(RValue_toInt32(val));
        case GML_TYPE_INT64:
            return RValue_makeInt64(RValue_toInt64(val));
        case GML_TYPE_BOOL:
            return RValue_makeBool(RValue_toBool(val));
        case GML_TYPE_STRING: {
            char* str = RValue_toString(val);
            return RValue_makeOwnedString(str);
        }
        case GML_TYPE_VARIABLE:
            // Variable type on stack is just an RValue passthrough
            return val;
        default:
            fprintf(stderr, "VM: Unknown target type 0x%X for conversion\n", targetType);
            return val;
    }
}

// ===[ Forward Declarations ]===

static RValue executeLoop(VMContext* ctx);
static void handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData);

// ===[ Opcode Handlers ]===

static void handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    uint8_t type1 = instrType1(instr);

    switch (type1) {
        case GML_TYPE_DOUBLE:
            stackPush(ctx,RValue_makeReal(BinaryUtils_readFloat64(extraData)));
            break;
        case GML_TYPE_FLOAT:
            stackPush(ctx,RValue_makeReal((double) BinaryUtils_readFloat32(extraData)));
            break;
        case GML_TYPE_INT32:
            stackPush(ctx,RValue_makeInt32(BinaryUtils_readInt32(extraData)));
            break;
        case GML_TYPE_INT64:
            stackPush(ctx,RValue_makeInt64(BinaryUtils_readInt64(extraData)));
            break;
        case GML_TYPE_BOOL:
            stackPush(ctx,RValue_makeBool(BinaryUtils_readInt32(extraData) != 0));
            break;
        case GML_TYPE_VARIABLE: {
            int32_t instanceType = (int32_t) instrInstanceType(instr);
            uint32_t varRef = resolveVarOperand(extraData);
            RValue val = resolveVariableRead(ctx, instanceType, varRef);
            stackPush(ctx,val);
            break;
        }
        case GML_TYPE_STRING: {
            int32_t stringIndex = BinaryUtils_readInt32(extraData);
            require(stringIndex >= 0 && ctx->dataWin->strg.count > (uint32_t) stringIndex);
            stackPush(ctx,RValue_makeString(ctx->dataWin->strg.strings[stringIndex]));
            break;
        }
        case GML_TYPE_INT16: {
            int16_t value = (int16_t) (instr & 0xFFFF);
            stackPush(ctx,RValue_makeInt32((int32_t) value));
            break;
        }
        default:
            fprintf(stderr, "VM: Push with unknown type 0x%X\n", type1);
            abort();
    }
}

static void handlePushScoped(VMContext* ctx, uint32_t instr, const uint8_t* extraData, ArrayMapEntry* variableMap, uint32_t count, RValue* variables, const char* scopeName, const char* altScopeName, StringBooleanEntry* traceMap) {
    (void) instr;
    uint32_t varRef = resolveVarOperand(extraData);
    Variable* varDef = resolveVarDef(ctx, varRef);

    ArrayAccess access = popArrayAccess(ctx, varRef);
    RValue val;
    if (access.isArray) {
        int32_t resolvedVarID = resolveArrayAlias(variables, count, varDef->varID);
        val = arrayMapGet(variableMap, resolvedVarID, access.arrayIndex);
        if (shouldTraceVariable(traceMap, scopeName, altScopeName, varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(val);
            fprintf(stderr, "VM: [%s] READ %s.%s[%d] -> %s\n", ctx->currentCodeName, scopeName, varDef->name, access.arrayIndex, rvalueAsString);
            free(rvalueAsString);
        }
    } else {
        require(count > (uint32_t) varDef->varID);
        val = variables[varDef->varID];
        val.ownsString = false; // Non-owning copy
        if (shouldTraceVariable(traceMap, scopeName, altScopeName, varDef->name)) {
            char* rvalueAsString = RValue_toStringTyped(val);
            fprintf(stderr, "VM: [%s] READ %s.%s -> %s\n", ctx->currentCodeName, scopeName, varDef->name, rvalueAsString);
            free(rvalueAsString);
        }
    }
    stackPush(ctx,val);
}

static void handlePushLoc(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    handlePushScoped(ctx, instr, extraData, ctx->localArrayMap, ctx->localVarCount, ctx->localVars, "local", nullptr, nullptr);
}

static void handlePushGlb(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    handlePushScoped(ctx, instr, extraData, ctx->globalArrayMap, ctx->globalVarCount, ctx->globalVars, "global", nullptr, ctx->varReadsToBeTraced);
}

static void handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    (void) instr;
    uint32_t varRef = resolveVarOperand(extraData);
    Variable* varDef = resolveVarDef(ctx, varRef);

    ArrayAccess access = popArrayAccess(ctx, varRef);

    RValue val = VMBuiltins_getVariable(ctx, varDef->name, access.arrayIndex);
    stackPush(ctx,val);
}

static void handlePushI(VMContext* ctx, uint32_t instr) {
    int16_t value = (int16_t) (instr & 0xFFFF);
    stackPush(ctx,RValue_makeInt32((int32_t) value));
}

static void handlePop(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    int32_t instanceType = (int32_t) instrInstanceType(instr);
    uint8_t type1 = instrType1(instr);   // destination type
    uint8_t type2 = instrType2(instr);   // source type (what's on stack)
    uint32_t varRef = resolveVarOperand(extraData);
    uint8_t varType = (varRef >> 24) & 0xF8;

    RValue val;
    int32_t arrayIndex = -1;

    int32_t originalInstanceType = instanceType;
    if (varType == VARTYPE_ARRAY) {
        if (type1 == GML_TYPE_VARIABLE) {
            // Simple assignment (Pop.v.v): stack bottom-to-top = [value, instanceType, arrayIndex]
            RValue arrayIdxVal = stackPop(ctx);
            arrayIndex = RValue_toInt32(arrayIdxVal);
            RValue_free(&arrayIdxVal);

            RValue instTypeVal = stackPop(ctx);
            instanceType = RValue_toInt32(instTypeVal);
            RValue_free(&instTypeVal);

            val = stackPop(ctx);
        } else {
            // Compound assignment (Pop.i.v, etc.): stack bottom-to-top = [instanceType, arrayIndex, value]
            val = stackPop(ctx);

            RValue arrayIdxVal = stackPop(ctx);
            arrayIndex = RValue_toInt32(arrayIdxVal);
            RValue_free(&arrayIdxVal);

            RValue instTypeVal = stackPop(ctx);
            instanceType = RValue_toInt32(instTypeVal);
            RValue_free(&instTypeVal);
        }
    } else if (varType == VARTYPE_STACKTOP && type1 == GML_TYPE_VARIABLE) {
        // Simple assignment (Pop.v.v) with STACKTOP: stack bottom-to-top = [value, instanceType]
        // Pop instanceType first (top), then value (bottom)
        RValue instTypeVal = stackPop(ctx);
        instanceType = RValue_toInt32(instTypeVal);
        RValue_free(&instTypeVal);

        val = stackPop(ctx);

        // Clear STACKTOP type bits so resolveVariableWrite's popArrayAccess won't double-pop
        varRef = (varRef & 0x07FFFFFF) | ((uint32_t) VARTYPE_NORMAL << 24);
    } else {
        val = stackPop(ctx);
    }

    // Convert if source type differs from destination type.
    // For VARTYPE_ARRAY compound assignments (type1 != GML_TYPE_VARIABLE), the type1 field
    // indicates the stack layout (compound vs simple), NOT a type conversion target.
    // Skip conversion in that case to preserve string values through += operations.
    // For compound assignments (type1 != GML_TYPE_VARIABLE) with VARTYPE_ARRAY or VARTYPE_STACKTOP,
    // the type1 field indicates the stack layout (compound vs simple), NOT a type conversion target.
    // Skip conversion to preserve the actual computed value (e.g. g.image_angle -= 4.5 must not truncate to int).
    bool isCompoundAssignment = ((varType == VARTYPE_ARRAY || varType == VARTYPE_STACKTOP) && type1 != GML_TYPE_VARIABLE);
    if (type2 != type1 && type1 != GML_TYPE_VARIABLE && !isCompoundAssignment) {
        RValue converted = convertValue(val, type1);
        RValue_free(&val);
        val = converted;
    }

    if (varType == VARTYPE_ARRAY) {
        Variable* varDef = resolveVarDef(ctx, varRef);
        if (varDef->varID == -6) {
            // Resolve target instance for built-in array variable writes (e.g. obj_foo.alarm[0] = 2)
            if (instanceType >= 0 && 100000 > instanceType) {
                // Object reference: write to ALL instances of that object
                Runner* runner = (Runner*) ctx->runner;
                int32_t instanceCount = (int32_t) arrlen(runner->instances);
                Instance* savedInstance = (Instance*) ctx->currentInstance;
                repeat(instanceCount, i) {
                    Instance* inst = runner->instances[i];
                    if (!inst->active || !VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, instanceType)) continue;
                    ctx->currentInstance = inst;
                    VMBuiltins_setVariable(ctx, varDef->name, val, arrayIndex);
                }
                ctx->currentInstance = savedInstance;
            } else if (instanceType >= 0) {
                // Instance ID reference
                Instance* target = findInstanceByTarget(ctx, instanceType);
                if (target != nullptr) {
                    Instance* savedInstance = (Instance*) ctx->currentInstance;
                    ctx->currentInstance = target;
                    VMBuiltins_setVariable(ctx, varDef->name, val, arrayIndex);
                    ctx->currentInstance = savedInstance;
                }
            } else if (instanceType == INSTANCE_OTHER && ctx->otherInstance != nullptr) {
                Instance* savedInstance = (Instance*) ctx->currentInstance;
                ctx->currentInstance = (Instance*) ctx->otherInstance;
                VMBuiltins_setVariable(ctx, varDef->name, val, arrayIndex);
                ctx->currentInstance = savedInstance;
            } else {
                // INSTANCE_SELF or other special types: use current instance
                VMBuiltins_setVariable(ctx, varDef->name, val, arrayIndex);
            }
        } else {
            switch (instanceType) {
                case INSTANCE_LOCAL:
                    arrayMapSet(&ctx->localArrayMap, varDef->varID, arrayIndex, val);
                    break;
                case INSTANCE_GLOBAL: {
                    int32_t resolvedVarID = resolveArrayAlias(ctx->globalVars, ctx->globalVarCount, varDef->varID);
                    arrayMapSet(&ctx->globalArrayMap, resolvedVarID, arrayIndex, val);
                    hmput(ctx->globalArrayVarTracker, resolvedVarID, 1);
                    if (shouldTraceVariable(ctx->varWritesToBeTraced, "global", nullptr, varDef->name)) {
                        char* rvalueAsString = RValue_toString(val);
                        if (originalInstanceType != instanceType) {
                            fprintf(stderr, "VM: [%s] WRITE global.%s[%d] = %s (resolved from stack, instruction said: %s)\n", ctx->currentCodeName, varDef->name, arrayIndex, rvalueAsString, instanceTypeName(originalInstanceType));
                        } else {
                            fprintf(stderr, "VM: [%s] WRITE global.%s[%d] = %s\n", ctx->currentCodeName, varDef->name, arrayIndex, rvalueAsString);
                        }
                        free(rvalueAsString);
                    }
                    break;
                }
                case INSTANCE_SELF:
                default: {
                    struct Instance* inst = (struct Instance*) ctx->currentInstance;
                    if (instanceType >= 0) {
                        inst = findInstanceByTarget(ctx, instanceType);
                        if (inst == nullptr) {
                            const char* varTypeName = varType == VARTYPE_ARRAY ? "ARRAY" : varType == VARTYPE_STACKTOP ? "STACKTOP" : varType == VARTYPE_NORMAL ? "NORMAL" : varType == VARTYPE_INSTANCE ? "INSTANCE" : "UNKNOWN";
                            char* valAsString = RValue_toString(val);
                            if (instanceType < 100000 && (uint32_t) instanceType < ctx->dataWin->objt.count) {
                                fprintf(stderr, "VM: [%s] WRITE array var '%s[%d]' on object index %d (%s) but no instance found (varType=%s, originalInstanceType=%d, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, arrayIndex, instanceType, ctx->dataWin->objt.objects[instanceType].name, varTypeName, originalInstanceType, varDef->varID, valAsString);
                            } else {
                                fprintf(stderr, "VM: [%s] WRITE array var '%s[%d]' on instance %d but no instance found (varType=%s, originalInstanceType=%d, varID=%d, value=%s)\n", ctx->currentCodeName, varDef->name, arrayIndex, instanceType, varTypeName, originalInstanceType, varDef->varID, valAsString);
                            }
                            free(valAsString);
                            break;
                        }
                    }
                    if (inst != nullptr) {
                        int32_t resolvedVarID = resolveArrayAliasHm(inst->selfVars, varDef->varID);
                        arrayMapSet(&inst->selfArrayMap, resolvedVarID, arrayIndex, val);
                        hmput(inst->selfArrayVarTracker, resolvedVarID, 1);
                        if (shouldTraceVariable(ctx->varWritesToBeTraced, ctx->dataWin->objt.objects[inst->objectIndex].name, "self", varDef->name)) {
                            char* rvalueAsString = RValue_toString(val);
                            if (originalInstanceType != instanceType) {
                                fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s (instanceId=%d) (resolved from stack, instruction said: %s)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, arrayIndex, rvalueAsString, inst->instanceId, instanceTypeName(originalInstanceType));
                            } else {
                                fprintf(stderr, "VM: [%s] WRITE %s.%s[%d] = %s (instanceId=%d)\n", ctx->currentCodeName, ctx->dataWin->objt.objects[inst->objectIndex].name, varDef->name, arrayIndex, rvalueAsString, inst->instanceId);
                            }
                            free(rvalueAsString);
                        }
                    }
                    break;
                }
            }
        }
    } else {
        resolveVariableWrite(ctx, instanceType, varRef, val);
    }
}

static void handlePopz(VMContext* ctx) {
    RValue val = stackPop(ctx);
    RValue_free(&val);
}

static void handleAdd(VMContext* ctx) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);

    if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
        // String concatenation
        const char* sa = a.string != nullptr ? a.string : "";
        const char* sb = b.string != nullptr ? b.string : "";
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = safeMalloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(ctx,RValue_makeOwnedString(result));
    } else if (a.type == RVALUE_STRING || b.type == RVALUE_STRING) {
        // String + Number: convert both to strings and concatenate (GMS behavior)
        char* sa = RValue_toString(a);
        char* sb = RValue_toString(b);
        size_t lenA = strlen(sa);
        size_t lenB = strlen(sb);
        char* result = safeMalloc(lenA + lenB + 1);
        memcpy(result, sa, lenA);
        memcpy(result + lenA, sb, lenB + 1);
        free(sa);
        free(sb);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(ctx,RValue_makeOwnedString(result));
    } else if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) {
        stackPush(ctx, RValue_makeInt32(a.int32 + b.int32));
    } else if (a.type == RVALUE_INT64 && b.type == RVALUE_INT64) {
        stackPush(ctx, RValue_makeInt64(a.int64 + b.int64));
    } else {
        double result = RValue_toReal(a) + RValue_toReal(b);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(ctx, RValue_makeReal(result));
    }
}

static void handleSub(VMContext* ctx) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) {
        stackPush(ctx, RValue_makeInt32(a.int32 - b.int32));
    } else if (a.type == RVALUE_INT64 && b.type == RVALUE_INT64) {
        stackPush(ctx, RValue_makeInt64(a.int64 - b.int64));
    } else {
        double result = RValue_toReal(a) - RValue_toReal(b);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(ctx, RValue_makeReal(result));
    }
}

static void handleMul(VMContext* ctx) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);

    if (a.type == RVALUE_STRING) {
        // String * Number = string repetition
        int count = RValue_toInt32(b);
        const char* str = a.string != nullptr ? a.string : "";
        size_t len = strlen(str);
        if (count <= 0 || len == 0) {
            RValue_free(&a);
            RValue_free(&b);
            stackPush(ctx,RValue_makeOwnedString(safeStrdup("")));
        } else {
            char* result = safeMalloc(len * count + 1);
            repeat(count, i) {
                memcpy(result + i * len, str, len);
            }
            result[len * count] = '\0';
            RValue_free(&a);
            RValue_free(&b);
            stackPush(ctx,RValue_makeOwnedString(result));
        }
    } else if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) {
        stackPush(ctx, RValue_makeInt32(a.int32 * b.int32));
    } else if (a.type == RVALUE_INT64 && b.type == RVALUE_INT64) {
        stackPush(ctx, RValue_makeInt64(a.int64 * b.int64));
    } else {
        double result = RValue_toReal(a) * RValue_toReal(b);
        RValue_free(&a);
        RValue_free(&b);
        stackPush(ctx, RValue_makeReal(result));
    }
}

static void handleDiv(VMContext* ctx) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    double divisor = RValue_toReal(b);
    if (divisor == 0.0) {
        fprintf(stderr, "VM: DoDiv :: Divide by zero\n");
        abort();
    }
    double result = RValue_toReal(a) / divisor;
    RValue_free(&a);
    RValue_free(&b);
    stackPush(ctx,RValue_makeReal(result));
}

static void handleRem(VMContext* ctx) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    int32_t ib = RValue_toInt32(b);
    if (ib == 0) {
        fprintf(stderr, "VM: DoRem :: Divide by zero\n");
        abort();
    }
    int32_t result = RValue_toInt32(a) % ib;
    RValue_free(&a);
    RValue_free(&b);
    stackPush(ctx,RValue_makeInt32(result));
}

static void handleMod(VMContext* ctx) {
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);
    double divisor = RValue_toReal(b);
    if (divisor == 0.0) {
        fprintf(stderr, "VM: DoMod :: Divide by zero\n");
        abort();
    }
    double result = fmod(RValue_toReal(a), divisor);
    RValue_free(&a);
    RValue_free(&b);
    stackPush(ctx,RValue_makeReal(result));
}

#define SIMPLE_BYTECODE_BITWISE_OPERATION(op) \
    RValue b = stackPop(ctx); \
    RValue a = stackPop(ctx); \
    int32_t result = RValue_toInt32(a) op RValue_toInt32(b); \
    RValue_free(&a); \
    RValue_free(&b); \
    stackPush(ctx,RValue_makeInt32(result))

static void handleAnd(VMContext* ctx) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(&);
}

static void handleOr(VMContext* ctx) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(|);
}

static void handleXor(VMContext* ctx) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(^);
}

static void handleNeg(VMContext* ctx) {
    RValue a = stackPop(ctx);
    double result = -RValue_toReal(a);
    RValue_free(&a);
    stackPush(ctx,RValue_makeReal(result));
}

static void handleNot(VMContext* ctx, uint32_t instr) {
    RValue a = stackPop(ctx);
    uint8_t type1 = instrType1(instr);
    if (GML_TYPE_BOOL == type1) {
        // Logical NOT: compiler emits this for the ! operator on boolean expressions
        int32_t result = (RValue_toInt32(a) == 0) ? 1 : 0;
        RValue_free(&a);
        stackPush(ctx,RValue_makeBool(result != 0));
    } else {
        // Bitwise NOT: used for ~ operator on integer types
        int32_t result = ~RValue_toInt32(a);
        RValue_free(&a);
        stackPush(ctx,RValue_makeInt32(result));
    }
}

static void handleShl(VMContext* ctx) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(<<);
}

static void handleShr(VMContext* ctx) {
    SIMPLE_BYTECODE_BITWISE_OPERATION(>>);
}

static void handleConv(VMContext* ctx, uint32_t instr) {
    uint8_t srcType = instrType1(instr);
    uint8_t dstType = instrType2(instr);

    RValue val = stackPop(ctx);

    uint8_t convKey = (dstType << 4) | srcType;
    RValue result;

    switch (convKey) {
        // Identity conversions (no-op)
        case 0x00: case 0x22: case 0x33: case 0x44: case 0x66:
            result = val;
            break;

        // Double (0) -> other
        case 0x20: result = RValue_makeInt32((int32_t) val.real); break;
        case 0x30: result = RValue_makeInt64((int64_t) val.real); break;
        case 0x40: result = RValue_makeBool(val.real > 0.5); break;
        case 0x50: result = val; break; // Double -> Variable (passthrough)
        case 0x60: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF0: result = RValue_makeInt32((int32_t) val.real); break;

        // Float (1) -> other (float stored as double in our RValue)
        case 0x01: result = RValue_makeReal(val.real); break;
        case 0x21: result = RValue_makeInt32((int32_t) val.real); break;
        case 0x31: result = RValue_makeInt64((int64_t) val.real); break;
        case 0x41: result = RValue_makeBool(val.real > 0.5); break;
        case 0x51: result = val; break; // Float -> Variable (passthrough)

        // Int32 (2) -> other
        case 0x02: result = RValue_makeReal((double) val.int32); break;
        case 0x12: result = RValue_makeReal((double) val.int32); break;
        case 0x32: result = RValue_makeInt64((int64_t) val.int32); break;
        case 0x42: result = RValue_makeBool(val.int32 > 0); break;
        case 0x52: result = val; break; // Int32 -> Variable (passthrough)
        case 0x62: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF2: result = val; break;

        // Int64 (3) -> other
        case 0x03: result = RValue_makeReal((double) val.int64); break;
        case 0x23: result = RValue_makeInt32((int32_t) val.int64); break;
        case 0x43: result = RValue_makeBool(val.int64 > 0); break;
        case 0x53: result = val; break; // Int64 -> Variable (passthrough)

        // Bool (4) -> other
        case 0x04: result = RValue_makeReal((double) val.int32); break;
        case 0x24: result = RValue_makeInt32(val.int32); break;
        case 0x34: result = RValue_makeInt64((int64_t) val.int32); break;
        case 0x54: result = val; break; // Bool -> Variable (passthrough)
        case 0x64: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }

        // Variable (5) -> other
        case 0x05: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x15: result = RValue_makeReal(RValue_toReal(val)); break;
        case 0x25: result = RValue_makeInt32(RValue_toInt32(val)); break;
        case 0x35: result = RValue_makeInt64(RValue_toInt64(val)); break;
        case 0x45: result = RValue_makeBool(RValue_toBool(val)); break;
        case 0x55: result = val; break; // Variable -> Variable (identity)
        case 0x65: { char* s = RValue_toString(val); result = RValue_makeOwnedString(s); break; }
        case 0xF5: result = RValue_makeInt32(RValue_toInt32(val)); break;

        // String (6) -> other
        case 0x06: result = RValue_makeReal(strtod(val.string, nullptr)); break;
        case 0x26: result = RValue_makeInt32((int32_t) strtod(val.string, nullptr)); break;
        case 0x36: result = RValue_makeInt64((int64_t) strtod(val.string, nullptr)); break;
        case 0x46: result = RValue_makeBool(val.string != nullptr && val.string[0] != '\0'); break;
        case 0x56: {
            // String -> Variable: keep as-is since our RValue handles strings natively
            result = val;
            break;
        }

        // Int16 (F) -> other
        case 0x0F: result = RValue_makeReal((double) val.int32); break;
        case 0x2F: result = val; break;
        case 0x5F: result = val; break;

        default:
            fprintf(stderr, "VM: Conv unhandled conversion 0x%02X (src=0x%X dst=0x%X)\n", convKey, srcType, dstType);
            result = val;
            break;
    }

    // Don't free the old value if we're returning the same value (identity conversion or passthrough)
    if (result.string != val.string || result.type != val.type) {
        RValue_free(&val);
    }

    stackPush(ctx,result);
}

static void handleCmp(VMContext* ctx, uint32_t instr) {
    uint8_t cmpKind = instrCmpKind(instr);
    RValue b = stackPop(ctx);
    RValue a = stackPop(ctx);

    bool result;
    if (a.type == RVALUE_STRING && b.type == RVALUE_STRING) {
        int cmp = strcmp(a.string != nullptr ? a.string : "", b.string != nullptr ? b.string : "");
        switch (cmpKind) {
            case CMP_LT:  result = 0 > cmp; break;
            case CMP_LTE: result = 0 >= cmp; break;
            case CMP_EQ:  result = cmp == 0; break;
            case CMP_NEQ: result = cmp != 0; break;
            case CMP_GTE: result = cmp >= 0; break;
            case CMP_GT:  result = cmp > 0; break;
            default: result = false; break;
        }
    } else {
        double da = RValue_toReal(a);
        double db = RValue_toReal(b);
        double diff = da - db;
        // GML uses epsilon-based comparison for all numeric CMP operations
        int cmp = fabs(diff) <= GML_MATH_EPSILON ? 0 : (diff < 0 ? -1 : 1);
        switch (cmpKind) {
            case CMP_LT:  result = cmp < 0; break;
            case CMP_LTE: result = cmp <= 0; break;
            case CMP_EQ:  result = cmp == 0; break;
            case CMP_NEQ: result = cmp != 0; break;
            case CMP_GTE: result = cmp >= 0; break;
            case CMP_GT:  result = cmp > 0; break;
            default: result = false; break;
        }
    }

    RValue_free(&a);
    RValue_free(&b);
    stackPush(ctx,RValue_makeBool(result));
}

static void handleDup(VMContext* ctx, uint32_t instr) {
    // The Extra field (lower 8 bits) encodes how many additional items beyond 1 to duplicate.
    // dup.i 0 = duplicate 1 item, dup.i 1 = duplicate 2 items (used for array access: instanceType + arrayIndex), etc.
    uint8_t extra = (uint8_t)(instr & 0xFF);
    int32_t count = (int32_t) extra + 1;

    require(ctx->stack.top >= count);

    // Copy 'count' items from the top of the stack (preserving order)
    int32_t startIdx = ctx->stack.top - count;
    for (int32_t i = 0; count > i; i++) {
        RValue copy = ctx->stack.slots[startIdx + i];

        // If the value owns a string, duplicate it to avoid double-free
        if (copy.type == RVALUE_STRING && copy.ownsString && copy.string != nullptr) {
            copy.string = safeStrdup(copy.string);
        }

        stackPush(ctx, copy);
    }
}

static void handleBranch(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    int32_t offset = instrJumpOffset(instr);
    ctx->ip = instrAddr + offset;
}

static void handleBranchTrue(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    RValue val = stackPop(ctx);
    bool condition = RValue_toInt32(val) != 0;
    RValue_free(&val);
    if (condition) {
        int32_t offset = instrJumpOffset(instr);
        ctx->ip = instrAddr + offset;
    }
}

static void handleBranchFalse(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    RValue val = stackPop(ctx);
    bool condition = RValue_toInt32(val) != 0;
    RValue_free(&val);
    if (!condition) {
        int32_t offset = instrJumpOffset(instr);
        ctx->ip = instrAddr + offset;
    }
}

// ===[ Function Call Handler ]===

static void handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData) {
    int32_t argCount = instr & 0xFFFF;
    uint32_t funcIndex = resolveFuncOperand(extraData);
    require(ctx->dataWin->func.functionCount > funcIndex);

    const char* funcName = ctx->dataWin->func.functions[funcIndex].name;

    // Pop arguments from stack (args pushed right-to-left, so first arg is on top)
    // Use stack-allocated buffer for small arg counts (GMS 1.4 supports up to 16 arguments)
    RValue stackArgs[GML_MAX_ARGUMENTS];
    RValue* args = nullptr;
    if (argCount > 0) {
        args = (GML_MAX_ARGUMENTS >= argCount) ? stackArgs : safeMalloc(argCount * sizeof(RValue));
        repeat(argCount, i) {
            args[i] = stackPop(ctx);
        }
    }

    bool functionIsBeingTraced = shgeti(ctx->functionCallsToBeTraced, "*") != -1 || shgeti(ctx->functionCallsToBeTraced, funcName) != -1 || shgeti(ctx->functionCallsToBeTraced, ctx->currentCodeName) != -1;
    char* functionArgumentList = nullptr;
    if (functionIsBeingTraced) {
        functionArgumentList = safeStrdup("");
        for (int32_t i = 0; i < argCount; i++) {
            char* display = RValue_toStringFancy(args[i]);

            if (i > 0) {
                char* tmp = safeMalloc(strlen(functionArgumentList) + 2 + strlen(display) + 1);
                sprintf(tmp, "%s, %s", functionArgumentList, display);
                free(functionArgumentList);
                functionArgumentList = tmp;
            } else {
                free(functionArgumentList);
                functionArgumentList = safeStrdup(display);
            }
            free(display);
        }

        fprintf(stderr, "VM: [%s] Calling function \"%s(%s)\"\n", ctx->currentCodeName, funcName, functionArgumentList);
    }

    // Check built-in functions first
    BuiltinFunc builtin = VMBuiltins_find(funcName);
    if (builtin != nullptr) {
        static int32_t blackScreenBuiltinLogCount = 0;
        if (VM_shouldProfileBlackScreenBuiltins(ctx) && blackScreenBuiltinLogCount < 256) {
            char logBuffer[256];
            snprintf(logBuffer, sizeof(logBuffer), "vm_scene: code=%s builtin=%s argc=%d", ctx->currentCodeName, funcName, argCount);
            VM_bootLog(logBuffer);
            blackScreenBuiltinLogCount++;
        }
        RValue result = builtin(ctx, args, argCount);
        // Free arguments
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            if (args != stackArgs) free(args);
        }

        if (functionIsBeingTraced) {
            char* returnValueAsString = RValue_toStringFancy(result);
            fprintf(stderr, "VM: [%s] Built-in function \"%s(%s)\" returned %s\n", ctx->currentCodeName, funcName, functionArgumentList, returnValueAsString);
            free(returnValueAsString);
            free(functionArgumentList);
        }

        stackPush(ctx,result);
        return;
    }

    // Look up script/user function via funcMap
    ptrdiff_t mapIdx = shgeti(ctx->funcMap, (char*) funcName);
    if (0 > mapIdx) {
        // Log once per (callingCode, funcName) pair
        const char* callerName = VM_getCallerName(ctx);
        char* dedupKey = VM_createDedupKey(callerName, funcName);

        if (0 > shgeti(ctx->loggedUnknownFuncs, dedupKey)) {
            shput(ctx->loggedUnknownFuncs, dedupKey, true);
            fprintf(stderr, "VM: [%s] Unknown function \"%s\"!\n", callerName, funcName);
        } else {
            free(dedupKey);
        }

        // Free arguments and push undefined
        if (args != nullptr) {
            repeat(argCount, i) {
                RValue_free(&args[i]);
            }
            if (args != stackArgs) free(args);
        }
        stackPush(ctx,RValue_makeUndefined());
        return;
    }

    int32_t codeIndex = ctx->funcMap[mapIdx].value;
    RValue result = VM_callCodeIndex(ctx, codeIndex, args, argCount);

    if (functionIsBeingTraced) {
        char* returnValueAsString = RValue_toStringFancy(result);
        fprintf(stderr, "VM: [%s] Script function \"%s(%s)\" returned %s\n", ctx->currentCodeName, funcName, functionArgumentList, returnValueAsString);
        free(returnValueAsString);
        free(functionArgumentList);
    }

    // Free arguments (VM_callCodeIndex copies what it needs)
    if (args != nullptr) {
        repeat(argCount, i) {
            RValue_free(&args[i]);
        }
        if (args != stackArgs) free(args);
    }

    // Push return value
    stackPush(ctx,result);
}

// ===[ With-Statement Helpers (PushEnv/PopEnv) ]===

// Checks if objectIndex is or inherits from targetObjectIndex by walking the parent chain.
bool VM_isObjectOrDescendant(DataWin* dataWin, int32_t objectIndex, int32_t targetObjectIndex) {
    int32_t currentObj = objectIndex;
    int depth = 0;
    while (currentObj >= 0 && (uint32_t) currentObj < dataWin->objt.count && 32 > depth) {
        if (currentObj == targetObjectIndex) return true;
        currentObj = dataWin->objt.objects[currentObj].parentId;
        depth++;
    }
    return false;
}


// Sets the VM instance context from an Instance.
static void switchToInstance(VMContext* ctx, Instance* inst) {
    ctx->currentInstance = inst;
}

// Restores VM context from an EnvFrame's saved fields.
static void restoreEnvContext(VMContext* ctx, EnvFrame* frame) {
    ctx->currentInstance = frame->savedInstance;
    ctx->otherInstance = frame->savedOtherInstance;
}

static void handlePushEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    int32_t jumpOffset = instrJumpOffset(instr);

    // Pop target from stack
    RValue targetVal = stackPop(ctx);
    int32_t target = RValue_toInt32(targetVal);
    RValue_free(&targetVal);

    // Create env frame, save current context
    EnvFrame* frame = safeMalloc(sizeof(EnvFrame));
    frame->savedInstance = (Instance*) ctx->currentInstance;
    frame->savedOtherInstance = (Instance*) ctx->otherInstance;
    frame->instanceList = nullptr;
    frame->currentIndex = 0;
    frame->parent = ctx->envStack;
    ctx->envStack = frame;

    // Inside a with-block, "other" refers to the instance that executed the with-statement
    ctx->otherInstance = (Instance*) ctx->currentInstance;

    Runner* runner = (Runner*) ctx->runner;

    if (target == INSTANCE_SELF) {
        // with(self) - no-op, keep current instance
        return;
    }

    if (target == INSTANCE_OTHER) {
        // with(other) - switch to the instance that was "self" before the nearest enclosing with-block
        // For nested with-blocks, other refers to the saved instance from the parent env frame
        if (frame->parent != nullptr) {
            switchToInstance(ctx, frame->parent->savedInstance);
        } else if (ctx->otherInstance != nullptr) {
            // No parent env frame, but we have an otherInstance (e.g., from collision events)
            switchToInstance(ctx, (Instance*) ctx->otherInstance);
        }
        // If no parent frame and no otherInstance, keep the saved instance (no-op)
        return;
    }

    if (target == INSTANCE_NOONE) {
        // with(noone) - skip the block entirely
        ctx->ip = instrAddr + jumpOffset;
        return;
    }

    if (target == INSTANCE_ALL) {
        // with(all) - iterate over all active instances
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        for (int32_t i = 0; instanceCount > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active) {
                arrput(frame->instanceList, inst);
            }
        }

        if (arrlen(frame->instanceList) == 0) {
            // No active instances, skip the block
            ctx->ip = instrAddr + jumpOffset;
            return;
        }

        frame->currentIndex = 0;
        switchToInstance(ctx, frame->instanceList[0]);
        return;
    }

    if (target >= 0 && 100000 > target) {
        // Object index - iterate over active instances of this object (or its descendants)
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        for (int32_t i = 0; instanceCount > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active && VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, target)) {
                arrput(frame->instanceList, inst);
            }
        }

        if (arrlen(frame->instanceList) == 0) {
            // No matching instances, skip the block
            ctx->ip = instrAddr + jumpOffset;
            return;
        }

        frame->currentIndex = 0;
        switchToInstance(ctx, frame->instanceList[0]);
        return;
    }

    if (target >= 100000) {
        // Instance ID - find the specific instance
        int32_t instanceCount = (int32_t) arrlen(runner->instances);
        for (int32_t i = 0; instanceCount > i; i++) {
            Instance* inst = runner->instances[i];
            if (inst->active && (int32_t) inst->instanceId == target) {
                switchToInstance(ctx, inst);
                return;
            }
        }

        // Instance not found, skip the block
        ctx->ip = instrAddr + jumpOffset;
        return;
    }

    fprintf(stderr, "VM: PushEnv with unhandled target %d\n", target);
    ctx->ip = instrAddr + jumpOffset;
}

static void handlePopEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr) {
    EnvFrame* frame = ctx->envStack;
    require(frame != nullptr);

    // Check for exit magic: PopEnv with 0xF00000 operand means "unwind env stack and exit/return"
    if ((instr & 0x00FFFFFF) == 0xF00000) {
        // Restore context and pop frame
        restoreEnvContext(ctx, frame);
        ctx->envStack = frame->parent;
        arrfree(frame->instanceList);
        free(frame);
        return;
    }

    // Check if there are more instances to iterate
    if (frame->instanceList != nullptr && arrlen(frame->instanceList) > frame->currentIndex + 1) {
        frame->currentIndex++;
        Instance* nextInst = frame->instanceList[frame->currentIndex];
        // Skip destroyed instances
        while (!nextInst->active && arrlen(frame->instanceList) > frame->currentIndex + 1) {
            frame->currentIndex++;
            nextInst = frame->instanceList[frame->currentIndex];
        }
        if (nextInst->active) {
            switchToInstance(ctx, nextInst);
            // Jump back to the start of the with-block body
            int32_t jumpOffset = instrJumpOffset(instr);
            ctx->ip = instrAddr + jumpOffset;
            return;
        }
    }

    // Done iterating - restore context and pop frame
    restoreEnvContext(ctx, frame);
    ctx->envStack = frame->parent;
    arrfree(frame->instanceList);
    free(frame);
}

// ===[ Execution Loop ]===

static const char* opcodeName(uint8_t opcode) {
    switch (opcode) {
        case OP_CONV:    return "Conv";
        case OP_MUL:     return "Mul";
        case OP_DIV:     return "Div";
        case OP_REM:     return "Rem";
        case OP_MOD:     return "Mod";
        case OP_ADD:     return "Add";
        case OP_SUB:     return "Sub";
        case OP_AND:     return "And";
        case OP_OR:      return "Or";
        case OP_XOR:     return "Xor";
        case OP_NEG:     return "Neg";
        case OP_NOT:     return "Not";
        case OP_SHL:     return "Shl";
        case OP_SHR:     return "Shr";
        case OP_CMP:     return "Cmp";
        case OP_POP:     return "Pop";
        case OP_PUSHI:   return "PushI";
        case OP_DUP:     return "Dup";
        case OP_RET:     return "Ret";
        case OP_EXIT:    return "Exit";
        case OP_POPZ:    return "Popz";
        case OP_B:       return "B";
        case OP_BT:      return "BT";
        case OP_BF:      return "BF";
        case OP_PUSHENV: return "PushEnv";
        case OP_POPENV:  return "PopEnv";
        case OP_PUSH:    return "Push";
        case OP_PUSHLOC: return "PushLoc";
        case OP_PUSHGLB: return "PushGlb";
        case OP_PUSHBLTN:return "PushBltn";
        case OP_CALL:    return "Call";
        case OP_BREAK:   return "Break";
        default:         return "???";
    }
}

// Forward declaration for formatInstruction (defined in disassembler section, used by trace-opcodes)
static void formatInstruction(VMContext* ctx, const uint8_t* bytecodeBase, uint32_t instrAddr, uint32_t instr, const uint8_t* extraData, char* opcodeStr, size_t opcodeSize, char* operandStr, size_t operandSize, char* commentStr, size_t commentSize);

static RValue executeLoop(VMContext* ctx) {
    while (ctx->codeEnd > ctx->ip) {
        uint32_t instrAddr = ctx->ip;
        uint32_t instr = BinaryUtils_readUint32(ctx->bytecodeBase + ctx->ip);
        ctx->ip += 4;

        // extraData pointer (may not be used depending on opcode)
        const uint8_t* extraData = ctx->bytecodeBase + ctx->ip;

        // If instruction has extra data (bit 30 set), advance IP past it
        if (instrHasExtraData(instr)) {
            ctx->ip += extraDataSize(instrType1(instr));
        }

        uint8_t opcode = instrOpcode(instr);

        if (shlen(ctx->opcodesToBeTraced) > 0) {
            if (shgeti(ctx->opcodesToBeTraced, "*") != -1 || shgeti(ctx->opcodesToBeTraced, ctx->currentCodeName) != -1) {
                char opcodeStr[32], operandStr[256] = "", commentStr[128] = "";
                formatInstruction(ctx, ctx->bytecodeBase, instrAddr, instr, extraData, opcodeStr, sizeof(opcodeStr), operandStr, sizeof(operandStr), commentStr, sizeof(commentStr));
                if (operandStr[0] != '\0') {
                    fprintf(stderr, "VM: [%s] @%04X [0x%08X] %s %s [stack=%d]\n", ctx->currentCodeName, instrAddr, instr, opcodeStr, operandStr, ctx->stack.top);
                } else {
                    fprintf(stderr, "VM: [%s] @%04X [0x%08X] %s [stack=%d]\n", ctx->currentCodeName, instrAddr, instr, opcodeStr, ctx->stack.top);
                }
            }
        }

        switch (opcode) {
            // Push instructions
            case OP_PUSH:
                handlePush(ctx, instr, extraData);
                break;
            case OP_PUSHLOC:
                handlePushLoc(ctx, instr, extraData);
                break;
            case OP_PUSHGLB:
                handlePushGlb(ctx, instr, extraData);
                break;
            case OP_PUSHBLTN:
                handlePushBltn(ctx, instr, extraData);
                break;
            case OP_PUSHI:
                handlePushI(ctx, instr);
                break;

            // Pop instructions
            case OP_POP:
                handlePop(ctx, instr, extraData);
                break;
            case OP_POPZ:
                handlePopz(ctx);
                break;

            // Arithmetic
            case OP_ADD: handleAdd(ctx); break;
            case OP_SUB: handleSub(ctx); break;
            case OP_MUL: handleMul(ctx); break;
            case OP_DIV: handleDiv(ctx); break;
            case OP_REM: handleRem(ctx); break;
            case OP_MOD: handleMod(ctx); break;

            // Bitwise / Logical
            case OP_AND: handleAnd(ctx); break;
            case OP_OR:  handleOr(ctx); break;
            case OP_XOR: handleXor(ctx); break;
            case OP_SHL: handleShl(ctx); break;
            case OP_SHR: handleShr(ctx); break;

            // Unary
            case OP_NEG: handleNeg(ctx); break;
            case OP_NOT: handleNot(ctx, instr); break;

            // Type conversion
            case OP_CONV:
                handleConv(ctx, instr);
                break;

            // Comparison
            case OP_CMP:
                handleCmp(ctx, instr);
                break;

            // Duplicate
            case OP_DUP:
                handleDup(ctx, instr);
                break;

            // Branches
            case OP_B:
                handleBranch(ctx, instr, instrAddr);
                break;
            case OP_BT:
                handleBranchTrue(ctx, instr, instrAddr);
                break;
            case OP_BF:
                handleBranchFalse(ctx, instr, instrAddr);
                break;

            // Function call
            case OP_CALL:
                handleCall(ctx, instr, extraData);
                break;

            // Return
            case OP_RET: {
                RValue retVal = stackPop(ctx);
                return retVal;
            }

            // Exit (no return value)
            case OP_EXIT:
                return RValue_makeUndefined();

            // Environment (with-statements)
            case OP_PUSHENV:
                handlePushEnv(ctx, instr, instrAddr);
                break;
            case OP_POPENV:
                handlePopEnv(ctx, instr, instrAddr);
                break;

            // Break (no-op / debug)
            case OP_BREAK:
                break;

            default:
                fprintf(stderr, "VM: Unknown opcode 0x%02X at offset %u\n", opcode, instrAddr);
                abort();
        }
    }

    return RValue_makeUndefined();
}

// ===[ Public API ]===

VMContext* VM_create(DataWin* dataWin) {
    VMContext* ctx = safeCalloc(1, sizeof(VMContext));
    ctx->dataWin = dataWin;
    ctx->stack.top = 0;
    ctx->selfId = -1;
    ctx->otherId = -1;
    ctx->callDepth = 0;
    ctx->currentEventType = -1;
    ctx->currentEventSubtype = -1;
    ctx->currentEventObjectIndex = -1;

    // Build reference lookup maps (file buffer stays read-only)
    patchReferenceOperands(ctx);

    // Scan VARI entries to find max varID for global scope
    // Built-in variables have varID == -6 (sentinel), skip those
    uint32_t maxGlobalVarID = 0;
    forEach(Variable, v, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (0 > v->varID) continue;
        if (v->instanceType == INSTANCE_GLOBAL) {
            if ((uint32_t) v->varID + 1 > maxGlobalVarID) maxGlobalVarID = (uint32_t) v->varID + 1;
        }
    }

    ctx->globalVarCount = maxGlobalVarID;
    ctx->globalVars = safeCalloc(maxGlobalVarID, sizeof(RValue));
    repeat(maxGlobalVarID, i) {
        ctx->globalVars[i].type = RVALUE_UNDEFINED;
    }

    ctx->globalArrayMap = nullptr;
    ctx->localArrayMap = nullptr;
    ctx->globalArrayVarTracker = nullptr;

    // Find the varID for "creator" self variable (used by instance_create)
    ctx->creatorVarID = -1;
    forEach(Variable, cv, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (cv->instanceType == INSTANCE_SELF && cv->varID >= 0 && strcmp(cv->name, "creator") == 0) {
            ctx->creatorVarID = cv->varID;
            break;
        }
    }

    // Build globalVarNameMap: varName -> varID for global variables
    ctx->globalVarNameMap = nullptr;
    forEach(Variable, v2, dataWin->vari.variables, dataWin->vari.variableCount) {
        if (v2->instanceType == INSTANCE_GLOBAL && v2->varID >= 0) {
            ptrdiff_t existing = shgeti(ctx->globalVarNameMap, (char*) v2->name);
            if (0 > existing) {
                shput(ctx->globalVarNameMap, (char*) v2->name, v2->varID);
            }
        }
    }

    // Build funcName -> codeIndex hash map from SCPT chunk
    ctx->funcMap = nullptr;
    forEach(Script, s, dataWin->scpt.scripts, dataWin->scpt.count) {
        if (s->name != nullptr && s->codeId >= 0) {
            if (dataWin->code.count > (uint32_t) s->codeId) {
                const char* codeName = dataWin->code.entries[s->codeId].name;
                // Map the full code entry name (e.g. "gml_Script_SCR_GAMESTART")
                shput(ctx->funcMap, (char*) codeName, s->codeId);
                // Also map the bare script name (e.g. "SCR_GAMESTART")
                // since the FUNC chunk references use bare names in CALL instructions
                shput(ctx->funcMap, (char*) s->name, s->codeId);
            }
        }
    }

    // Also map code entry names directly for non-script code (object events, room creation codes, etc.)
    repeat(dataWin->code.count, i) {
        const char* codeName = dataWin->code.entries[i].name;
        ptrdiff_t existing = shgeti(ctx->funcMap, (char*) codeName);
        if (0 > existing) {
            shput(ctx->funcMap, (char*) codeName, (int32_t) i);
        }
    }

    // Register built-in functions
    VMBuiltins_registerAll();

    fprintf(stderr, "VM: Initialized with %u global vars, sparse self vars (hashmap), %u functions mapped\n", ctx->globalVarCount, (uint32_t) shlen(ctx->funcMap));

    return ctx;
}

RValue VM_executeCode(VMContext* ctx, int32_t codeIndex) {
    require(codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    ctx->bytecodeBase = ctx->dataWin->bytecodeBuffer + (code->bytecodeAbsoluteOffset - ctx->dataWin->bytecodeBufferBase);
    ctx->ip = 0;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;

    // Allocate locals
    uint32_t localsCount = code->localsCount;
    if (localsCount == 0) localsCount = 1; // at least 1 slot to avoid nullptr
    ctx->localVars = safeCalloc(localsCount, sizeof(RValue));
    ctx->localVarCount = localsCount;
    ctx->localArrayMap = nullptr;
    repeat(localsCount, i) {
        ctx->localVars[i].type = RVALUE_UNDEFINED;
    }

    // Reset stack for top-level execution
    ctx->stack.top = 0;

    RValue result = executeLoop(ctx);

    // Free locals
    repeat(ctx->localVarCount, i) {
        RValue_free(&ctx->localVars[i]);
    }
    free(ctx->localVars);
    ctx->localVars = nullptr;
    ctx->localVarCount = 0;

    // Free local array map
    RValue_freeAllRValuesInMap(ctx->localArrayMap);
    hmfree(ctx->localArrayMap);
    ctx->localArrayMap = nullptr;

    return result;
}

CodeLocals* VM_resolveCodeLocals(VMContext* ctx, const char* codeName) {
    CodeLocals* codeLocals = nullptr;
    forEach(CodeLocals, cl, ctx->dataWin->func.codeLocals, ctx->dataWin->func.codeLocalsCount) {
        if (strcmp(cl->name, codeName) == 0) {
            codeLocals = cl;
            break;
        }
    }
    return codeLocals;
}

RValue VM_callCodeIndex(VMContext* ctx, int32_t codeIndex, RValue* args, int32_t argCount) {
    require(codeIndex >= 0 && ctx->dataWin->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];

    // Save current frame
    CallFrame* frame = safeMalloc(sizeof(CallFrame));
    frame->savedIP = ctx->ip;
    frame->savedCodeEnd = ctx->codeEnd;
    frame->savedBytecodeBase = ctx->bytecodeBase;
    frame->savedLocals = ctx->localVars;
    frame->savedLocalsCount = ctx->localVarCount;
    frame->savedCodeName = ctx->currentCodeName;
    frame->savedLocalArrayMap = ctx->localArrayMap;
    frame->savedScriptArgs = ctx->scriptArgs;
    frame->savedScriptArgCount = ctx->scriptArgCount;
    frame->parent = ctx->callStack;
    ctx->callStack = frame;
    ctx->callDepth++;

    // Set up callee
    ctx->bytecodeBase = ctx->dataWin->bytecodeBuffer + (code->bytecodeAbsoluteOffset - ctx->dataWin->bytecodeBufferBase);
    ctx->ip = 0;
    ctx->codeEnd = code->length;
    ctx->currentCodeName = code->name;
    ctx->localArrayMap = nullptr;

    uint32_t localsCount = code->localsCount;
    if (localsCount == 0) localsCount = 1;
    ctx->localVars = safeCalloc(localsCount, sizeof(RValue));
    ctx->localVarCount = localsCount;
    repeat(localsCount, i) {
        ctx->localVars[i].type = RVALUE_UNDEFINED;
    }

    // Store arguments in scriptArgs (mirrors GMS 1.4's global argument stack)
    ctx->scriptArgCount = argCount;
    if (argCount > 0 && args != nullptr) {
        ctx->scriptArgs = safeMalloc((uint32_t) argCount * sizeof(RValue));
        repeat(argCount, argIdx) {
            RValue argCopy = args[argIdx];
            if (argCopy.type == RVALUE_STRING && argCopy.ownsString && argCopy.string != nullptr) {
                argCopy.string = safeStrdup(argCopy.string);
            }
            ctx->scriptArgs[argIdx] = argCopy;
        }
    } else {
        ctx->scriptArgs = nullptr;
    }

    // Execute the callee
    RValue result = executeLoop(ctx);

    // Make result string owning BEFORE freeing callee locals/arrays to prevent
    // dangling pointer if the returned string points into a callee local var or array map.
    if (result.type == RVALUE_STRING && !result.ownsString && result.string != nullptr) {
        result = RValue_makeOwnedString(safeStrdup(result.string));
    }

    // Restore caller frame
    CallFrame* saved = ctx->callStack;
    ctx->ip = saved->savedIP;
    ctx->codeEnd = saved->savedCodeEnd;
    ctx->bytecodeBase = saved->savedBytecodeBase;

    // Free callee locals
    repeat(ctx->localVarCount, i) {
        RValue_free(&ctx->localVars[i]);
    }
    free(ctx->localVars);

    // Free callee local array map
    RValue_freeAllRValuesInMap(ctx->localArrayMap);
    hmfree(ctx->localArrayMap);

    // Free callee script args
    repeat(ctx->scriptArgCount, i) {
        RValue_free(&ctx->scriptArgs[i]);
    }
    free(ctx->scriptArgs);

    ctx->localVars = saved->savedLocals;
    ctx->localVarCount = saved->savedLocalsCount;
    ctx->localArrayMap = saved->savedLocalArrayMap;
    ctx->scriptArgs = saved->savedScriptArgs;
    ctx->scriptArgCount = saved->savedScriptArgCount;
    ctx->currentCodeName = saved->savedCodeName;
    ctx->callStack = saved->parent;
    ctx->callDepth--;
    free(saved);

    return result;
}

// ===[ Disassembler ]===

static char gmlTypeChar(uint8_t type) {
    switch (type) {
        case GML_TYPE_DOUBLE:   return 'd';
        case GML_TYPE_FLOAT:    return 'f';
        case GML_TYPE_INT32:    return 'i';
        case GML_TYPE_INT64:    return 'l';
        case GML_TYPE_BOOL:     return 'b';
        case GML_TYPE_VARIABLE: return 'v';
        case GML_TYPE_STRING:   return 's';
        case GML_TYPE_INT16:    return 'e';
        default:                return '?';
    }
}

static const char* cmpKindName(uint8_t kind) {
    switch (kind) {
        case CMP_LT:  return "LT";
        case CMP_LTE: return "LTE";
        case CMP_EQ:  return "EQ";
        case CMP_NEQ: return "NEQ";
        case CMP_GTE: return "GTE";
        case CMP_GT:  return "GT";
        default:      return "???";
    }
}

static const char* varTypeName(uint32_t varRef) {
    uint8_t varType = (varRef >> 24) & 0xF8;
    switch (varType) {
        case VARTYPE_ARRAY:    return "Array";
        case VARTYPE_STACKTOP: return "StackTop";
        case VARTYPE_NORMAL:   return "Normal";
        case VARTYPE_INSTANCE: return "Instance";
        default:               return "Unknown";
    }
}

static const char* disasmScopeName(VMContext* ctx, int32_t instanceType) {
    switch (instanceType) {
        case INSTANCE_SELF:      return "self";
        case INSTANCE_OTHER:     return "other";
        case INSTANCE_ALL:       return "all";
        case INSTANCE_NOONE:     return "noone";
        case INSTANCE_GLOBAL:    return "global";
        case INSTANCE_LOCAL:     return "local";
        case INSTANCE_STACKTOP:  return "stacktop";
        default:
            if (instanceType >= 0 && ctx->dataWin->objt.count > (uint32_t) instanceType) {
                return ctx->dataWin->objt.objects[instanceType].name;
            }
            return "unknown";
    }
}

// Formats a variable operand for disassembly: "scope.varName [varType]"
// If scopeOverride is set (e.g. "local", "global"), uses that instead of resolving instrInstType.
// Shows VARI instanceType mismatch annotation when scopeOverride is nullptr and types differ.
static void disasmFormatVar(VMContext* ctx, const uint8_t* extraData, const char* scopeOverride, int32_t instrInstType, char* buf, size_t bufSize) {
    uint32_t varRef = resolveVarOperand(extraData);
    Variable* varDef = resolveVarDef(ctx, varRef);
    const char* vType = varTypeName(varRef);
    const char* scope = scopeOverride != nullptr ? scopeOverride : disasmScopeName(ctx, instrInstType);

    if (scopeOverride == nullptr && varDef->instanceType != instrInstType) {
        const char* variScope = disasmScopeName(ctx, varDef->instanceType);
        snprintf(buf, bufSize, "%s.%s [%s] (VARI: %s, instr: %s)", scope, varDef->name, vType, variScope, scope);
    } else {
        snprintf(buf, bufSize, "%s.%s [%s]", scope, varDef->name, vType);
    }
}

// Returns stack effect comment for a variable access instruction
static void disasmFormatVarComment(VMContext* ctx, const uint8_t* extraData, bool isPop, char* buf, size_t bufSize) {
    uint32_t varRef = resolveVarOperand(extraData);
    uint8_t varType = (varRef >> 24) & 0xF8;
    if (isPop) {
        switch (varType) {
            case VARTYPE_ARRAY:    snprintf(buf, bufSize, "// pops: [arrayIndex, instanceType, value]"); break;
            case VARTYPE_STACKTOP: snprintf(buf, bufSize, "// pops: [instanceType, value]"); break;
            default:               snprintf(buf, bufSize, "// pops: [value]"); break;
        }
    } else {
        switch (varType) {
            case VARTYPE_ARRAY:    snprintf(buf, bufSize, "// pops: [arrayIndex, instanceType] -> pushes: [value]"); break;
            case VARTYPE_STACKTOP: snprintf(buf, bufSize, "// pops: [instanceType] -> pushes: [value]"); break;
            default:               snprintf(buf, bufSize, "// pushes: [value]"); break;
        }
    }
}

// Formats a single instruction into opcodeStr, operandStr, and commentStr buffers.
// Used by both VM_disassemble and --trace-opcodes.
// bytecodeBase is needed because the disassembler and trace have it from different sources.
static void formatInstruction(VMContext* ctx, const uint8_t* bytecodeBase, uint32_t instrAddr, uint32_t instr, const uint8_t* extraData,
                              char* opcodeStr, size_t opcodeSize, char* operandStr, size_t operandSize, char* commentStr, size_t commentSize) {
    DataWin* dw = ctx->dataWin;
    uint8_t opcode = instrOpcode(instr);
    uint8_t type1 = instrType1(instr);
    uint8_t type2 = instrType2(instr);
    int16_t instType = instrInstanceType(instr);

    switch (opcode) {
        // Binary arithmetic/logic
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
        case OP_REM: case OP_MOD: case OP_AND: case OP_OR:
        case OP_XOR: case OP_SHL: case OP_SHR:
            snprintf(opcodeStr, opcodeSize, "%s.%c.%c", opcodeName(opcode), gmlTypeChar(type1), gmlTypeChar(type2));
            snprintf(commentStr, commentSize, "// pops: [a, b] -> pushes: [result]");
            break;

        // Unary
        case OP_NEG:
            snprintf(opcodeStr, opcodeSize, "Neg.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// pops: [a] -> pushes: [result]");
            break;
        case OP_NOT:
            snprintf(opcodeStr, opcodeSize, "Not.%c", gmlTypeChar(type1));
            if (type1 == GML_TYPE_BOOL) {
                snprintf(commentStr, commentSize, "// pops: [a] -> pushes: [bool] (logical NOT)");
            } else {
                snprintf(commentStr, commentSize, "// pops: [a] -> pushes: [int] (bitwise NOT)");
            }
            break;

        // Type conversion
        case OP_CONV:
            snprintf(opcodeStr, opcodeSize, "Conv.%c.%c", gmlTypeChar(type1), gmlTypeChar(type2));
            snprintf(commentStr, commentSize, "// pops: [%c] -> pushes: [%c]", gmlTypeChar(type2), gmlTypeChar(type1));
            break;

        // Comparison
        case OP_CMP:
            snprintf(opcodeStr, opcodeSize, "Cmp.%c.%c", gmlTypeChar(type1), gmlTypeChar(type2));
            snprintf(operandStr, operandSize, "%s", cmpKindName(instrCmpKind(instr)));
            snprintf(commentStr, commentSize, "// pops: [a, b] -> pushes: [bool]");
            break;

        // Push
        case OP_PUSH: {
            switch (type1) {
                case GML_TYPE_DOUBLE:
                    snprintf(opcodeStr, opcodeSize, "Push.d");
                    snprintf(operandStr, operandSize, "%g", BinaryUtils_readFloat64(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [double]");
                    break;
                case GML_TYPE_FLOAT:
                    snprintf(opcodeStr, opcodeSize, "Push.f");
                    snprintf(operandStr, operandSize, "%g", (double) BinaryUtils_readFloat32(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [float]");
                    break;
                case GML_TYPE_INT32:
                    snprintf(opcodeStr, opcodeSize, "Push.i");
                    snprintf(operandStr, operandSize, "%d", BinaryUtils_readInt32(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [int32]");
                    break;
                case GML_TYPE_INT64:
                    snprintf(opcodeStr, opcodeSize, "Push.l");
                    snprintf(operandStr, operandSize, "%lld", (long long) BinaryUtils_readInt64(extraData));
                    snprintf(commentStr, commentSize, "// pushes: [int64]");
                    break;
                case GML_TYPE_BOOL:
                    snprintf(opcodeStr, opcodeSize, "Push.b");
                    snprintf(operandStr, operandSize, "%s", BinaryUtils_readInt32(extraData) != 0 ? "true" : "false");
                    snprintf(commentStr, commentSize, "// pushes: [bool]");
                    break;
                case GML_TYPE_STRING: {
                    snprintf(opcodeStr, opcodeSize, "Push.s");
                    int32_t strIdx = BinaryUtils_readInt32(extraData);
                    if (strIdx >= 0 && dw->strg.count > (uint32_t) strIdx) {
                        const char* str = dw->strg.strings[strIdx];
                        if (strlen(str) > 60) {
                            snprintf(operandStr, operandSize, "\"%.57s...\"", str);
                        } else {
                            snprintf(operandStr, operandSize, "\"%s\"", str);
                        }
                    } else {
                        snprintf(operandStr, operandSize, "[string:%d]", strIdx);
                    }
                    snprintf(commentStr, commentSize, "// pushes: [string]");
                    break;
                }
                case GML_TYPE_VARIABLE:
                    snprintf(opcodeStr, opcodeSize, "Push.v");
                    disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
                    disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
                    break;
                case GML_TYPE_INT16:
                    snprintf(opcodeStr, opcodeSize, "Push.e");
                    snprintf(operandStr, operandSize, "%d", (int32_t) instType);
                    snprintf(commentStr, commentSize, "// pushes: [int16]");
                    break;
                default:
                    snprintf(opcodeStr, opcodeSize, "Push.?");
                    snprintf(operandStr, operandSize, "(unknown type 0x%X)", type1);
                    break;
            }
            break;
        }

        // Scoped pushes
        case OP_PUSHLOC:
            snprintf(opcodeStr, opcodeSize, "PushLoc.v");
            disasmFormatVar(ctx, extraData, "local", (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
            break;
        case OP_PUSHGLB:
            snprintf(opcodeStr, opcodeSize, "PushGlb.v");
            disasmFormatVar(ctx, extraData, "global", (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
            break;
        case OP_PUSHBLTN:
            snprintf(opcodeStr, opcodeSize, "PushBltn.v");
            disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, false, commentStr, commentSize);
            break;

        // PushI (int16 immediate)
        case OP_PUSHI:
            snprintf(opcodeStr, opcodeSize, "PushI.e");
            snprintf(operandStr, operandSize, "%d", (int32_t) instType);
            snprintf(commentStr, commentSize, "// pushes: [int16]");
            break;

        // Pop (store to variable)
        case OP_POP:
            snprintf(opcodeStr, opcodeSize, "Pop.%c.%c", gmlTypeChar(type1), gmlTypeChar(type2));
            disasmFormatVar(ctx, extraData, nullptr, (int32_t) instType, operandStr, operandSize);
            disasmFormatVarComment(ctx, extraData, true, commentStr, commentSize);
            break;

        // Unconditional branch
        case OP_B: {
            snprintf(opcodeStr, opcodeSize, "B");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            snprintf(operandStr, operandSize, "L_%04X (offset: %+d)", target, offset);
            break;
        }

        // Conditional branches
        case OP_BT: {
            snprintf(opcodeStr, opcodeSize, "BT");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            snprintf(operandStr, operandSize, "L_%04X (offset: %+d)", target, offset);
            snprintf(commentStr, commentSize, "// pops: [bool]");
            break;
        }
        case OP_BF: {
            snprintf(opcodeStr, opcodeSize, "BF");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            snprintf(operandStr, operandSize, "L_%04X (offset: %+d)", target, offset);
            snprintf(commentStr, commentSize, "// pops: [bool]");
            break;
        }

        // With-statement: PushEnv
        case OP_PUSHENV: {
            snprintf(opcodeStr, opcodeSize, "PushEnv");
            int32_t offset = instrJumpOffset(instr);
            uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
            // Peek at previous instruction to identify the target object
            const char* targetName = nullptr;
            if (instrAddr >= 4) {
                uint32_t prevInstr = BinaryUtils_readUint32(bytecodeBase + instrAddr - 4);
                if (instrOpcode(prevInstr) == OP_PUSHI) {
                    int16_t objIdx = (int16_t) (prevInstr & 0xFFFF);
                    targetName = disasmScopeName(ctx, (int32_t) objIdx);
                }
            }
            if (targetName != nullptr) {
                snprintf(operandStr, operandSize, "%s (target: L_%04X, offset: %+d)", targetName, target, offset);
            } else {
                snprintf(operandStr, operandSize, "(target: L_%04X, offset: %+d)", target, offset);
            }
            snprintf(commentStr, commentSize, "// pops: [target]");
            break;
        }

        // With-statement: PopEnv
        case OP_POPENV: {
            snprintf(opcodeStr, opcodeSize, "PopEnv");
            if ((instr & 0x00FFFFFF) == 0xF00000) {
                snprintf(operandStr, operandSize, "[exit]");
            } else {
                int32_t offset = instrJumpOffset(instr);
                uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
                snprintf(operandStr, operandSize, "(target: L_%04X, offset: %+d)", target, offset);
            }
            break;
        }

        // Function call
        case OP_CALL: {
            snprintf(opcodeStr, opcodeSize, "Call.i");
            int32_t argCount = instr & 0xFFFF;
            uint32_t funcIdx = resolveFuncOperand(extraData);
            const char* funcName = (dw->func.functionCount > funcIdx) ? dw->func.functions[funcIdx].name : "???";
            snprintf(operandStr, operandSize, "%s(%d)", funcName, argCount);
            if (argCount > 0) {
                char argList[128] = "";
                int32_t pos = 0;
                for (int32_t i = 0; 8 > i && argCount > i; i++) {
                    if (i > 0) pos += snprintf(argList + pos, sizeof(argList) - pos, ", ");
                    pos += snprintf(argList + pos, sizeof(argList) - pos, "arg%d", i);
                }
                if (argCount > 8) snprintf(argList + pos, sizeof(argList) - pos, ", ...");
                snprintf(commentStr, commentSize, "// pops: [%s] -> pushes: [result]", argList);
            } else {
                snprintf(commentStr, commentSize, "// pushes: [result]");
            }
            break;
        }

        // Duplicate stack items
        case OP_DUP: {
            uint8_t extra = (uint8_t) (instr & 0xFF);
            int32_t count = (int32_t) extra + 1;
            snprintf(opcodeStr, opcodeSize, "Dup.%c", gmlTypeChar(type1));
            if (count > 1) {
                snprintf(operandStr, operandSize, "%d", count);
                snprintf(commentStr, commentSize, "// duplicates %d items", count);
            } else {
                snprintf(commentStr, commentSize, "// duplicates top item");
            }
            break;
        }

        // Control flow
        case OP_RET:
            snprintf(opcodeStr, opcodeSize, "Ret.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// pops: [value] (return)");
            break;
        case OP_EXIT:
            snprintf(opcodeStr, opcodeSize, "Exit.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// (end of code)");
            break;
        case OP_POPZ:
            snprintf(opcodeStr, opcodeSize, "Popz.%c", gmlTypeChar(type1));
            snprintf(commentStr, commentSize, "// pops: [value]");
            break;

        // Debug break
        case OP_BREAK:
            snprintf(opcodeStr, opcodeSize, "Break.%c", gmlTypeChar(type1));
            snprintf(operandStr, operandSize, "%d", (int32_t) instType);
            break;

        default:
            snprintf(opcodeStr, opcodeSize, "??? (0x%02X)", opcode);
            break;
    }
}

void VM_buildCrossReferences(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    ctx->crossRefMap = nullptr;

    repeat(dw->code.count, callerIdx) {
        CodeEntry* code = &dw->code.entries[callerIdx];
        const uint8_t* base = dw->bytecodeBuffer + (code->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
        uint32_t ip = 0;

        while (code->length > ip) {
            uint32_t instr = BinaryUtils_readUint32(base + ip);
            ip += 4;
            const uint8_t* ed = base + ip;
            if (instrHasExtraData(instr)) {
                ip += extraDataSize(instrType1(instr));
            }

            if (instrOpcode(instr) == OP_CALL) {
                uint32_t funcIdx = resolveFuncOperand(ed);
                if (dw->func.functionCount > funcIdx) {
                    const char* funcName = dw->func.functions[funcIdx].name;
                    ptrdiff_t codeMapIdx = shgeti(ctx->funcMap, (char*) funcName);
                    if (codeMapIdx >= 0) {
                        int32_t targetIdx = ctx->funcMap[codeMapIdx].value;
                        ptrdiff_t mapIdx = hmgeti(ctx->crossRefMap, targetIdx);
                        if (0 > mapIdx) {
                            int32_t* callers = nullptr;
                            arrput(callers, (int32_t) callerIdx);
                            hmput(ctx->crossRefMap, targetIdx, callers);
                        } else {
                            // Deduplicate: don't add the same caller twice
                            int32_t* callers = ctx->crossRefMap[mapIdx].value;
                            bool found = false;
                            for (ptrdiff_t k = 0; arrlen(callers) > k; k++) {
                                if (callers[k] == (int32_t) callerIdx) { found = true; break; }
                            }
                            if (!found) {
                                arrput(ctx->crossRefMap[mapIdx].value, (int32_t) callerIdx);
                            }
                        }
                    }
                }
            }
        }
    }
}

void VM_disassemble(VMContext* ctx, int32_t codeIndex) {
    DataWin* dw = ctx->dataWin;
    require(dw->code.count > (uint32_t) codeIndex);
    CodeEntry* code = &dw->code.entries[codeIndex];

    // Header
    printf("=== %s (length=%u, locals=%u, args=%u) ===\n", code->name, code->length, code->localsCount, code->argumentsCount);

    // CodeLocals
    CodeLocals* locals = VM_resolveCodeLocals(ctx, code->name);
    if (locals != nullptr && locals->localVarCount > 0) {
        printf("Locals:");
        repeat(locals->localVarCount, i) {
            if (i > 0) printf(",");
            printf(" [%u] %s", locals->locals[i].index, locals->locals[i].name);
        }
        printf("\n");
    }

    // Cross-references
    if (ctx->crossRefMap != nullptr) {
        ptrdiff_t mapIdx = hmgeti(ctx->crossRefMap, codeIndex);
        if (mapIdx >= 0) {
            int32_t* callers = ctx->crossRefMap[mapIdx].value;
            printf("Called by:");
            for (ptrdiff_t i = 0; arrlen(callers) > i; i++) {
                if (i > 0) printf(",");
                printf(" %s", dw->code.entries[callers[i]].name);
            }
            printf("\n");
        }
    }

    printf("\n");

    const uint8_t* bytecodeBase = dw->bytecodeBuffer + (code->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
    uint32_t codeLength = code->length;

    // Pass 1: collect branch targets for labels
    struct { uint32_t key; bool value; }* branchTargets = nullptr;
    {
        uint32_t ip = 0;
        while (codeLength > ip) {
            uint32_t instrAddr = ip;
            uint32_t instr = BinaryUtils_readUint32(bytecodeBase + ip);
            ip += 4;
            if (instrHasExtraData(instr)) {
                ip += extraDataSize(instrType1(instr));
            }
            uint8_t opcode = instrOpcode(instr);
            if (opcode == OP_B || opcode == OP_BT || opcode == OP_BF || opcode == OP_PUSHENV) {
                int32_t offset = instrJumpOffset(instr);
                uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
                hmput(branchTargets, target, true);
            }
            if (opcode == OP_POPENV) {
                if ((instr & 0x00FFFFFF) != 0xF00000) {
                    int32_t offset = instrJumpOffset(instr);
                    uint32_t target = (uint32_t) ((int32_t) instrAddr + offset);
                    hmput(branchTargets, target, true);
                }
            }
        }
    }

    // Pass 2: print instructions
    uint32_t ip = 0;
    int32_t envDepth = 0;

    while (codeLength > ip) {
        uint32_t instrAddr = ip;
        uint32_t instr = BinaryUtils_readUint32(bytecodeBase + ip);
        ip += 4;
        const uint8_t* extraData = bytecodeBase + ip;
        if (instrHasExtraData(instr)) {
            ip += extraDataSize(instrType1(instr));
        }

        uint8_t opcode = instrOpcode(instr);

        // PopEnv decreases depth before printing
        if (opcode == OP_POPENV && envDepth > 0) envDepth--;

        // Print label if this address is a branch target
        if (hmgeti(branchTargets, instrAddr) >= 0) {
            printf("  %04X: L_%04X:\n", instrAddr, instrAddr);
        }

        int32_t indent = 2 + envDepth * 4;
        char opcodeStr[32];
        char operandStr[256] = "";
        char commentStr[128] = "";

        formatInstruction(ctx, bytecodeBase, instrAddr, instr, extraData, opcodeStr, sizeof(opcodeStr), operandStr, sizeof(operandStr), commentStr, sizeof(commentStr));

        // Print the formatted line
        if (commentStr[0] != '\0') {
            printf("%*s%04X: [0x%08X] %-16s %-45s %s\n", indent, "", instrAddr, instr, opcodeStr, operandStr, commentStr);
        } else {
            printf("%*s%04X: [0x%08X] %-16s %s\n", indent, "", instrAddr, instr, opcodeStr, operandStr);
        }

        // PushEnv increases depth after printing
        if (opcode == OP_PUSHENV) envDepth++;
    }

    hmfree(branchTargets);
    printf("\n");
}

void VM_free(VMContext* ctx) {
    if (ctx == nullptr) return;

    // Free global vars
    if (ctx->globalVars != nullptr) {
        repeat(ctx->globalVarCount, i) {
            RValue_free(&ctx->globalVars[i]);
        }
        free(ctx->globalVars);
    }

    // Free array maps
    RValue_freeAllRValuesInMap(ctx->globalArrayMap);
    hmfree(ctx->globalArrayMap);

    RValue_freeAllRValuesInMap(ctx->localArrayMap);
    hmfree(ctx->localArrayMap);

    // Free array var trackers
    hmfree(ctx->globalArrayVarTracker);

    // Free hash maps
    shfree(ctx->funcMap);
    shfree(ctx->globalVarNameMap);
    shfree(ctx->loggedUnknownFuncs);
    shfree(ctx->loggedStubbedFuncs);
    shfree(ctx->varReadsToBeTraced);
    shfree(ctx->varWritesToBeTraced);
    shfree(ctx->functionCallsToBeTraced);
    shfree(ctx->alarmsToBeTraced);
    shfree(ctx->instanceLifecyclesToBeTraced);
    shfree(ctx->eventsToBeTraced);
    shfree(ctx->opcodesToBeTraced);
    shfree(ctx->stackToBeTraced);

    // Free cross-reference map
    if (ctx->crossRefMap != nullptr) {
        for (ptrdiff_t i = 0; hmlen(ctx->crossRefMap) > i; i++) {
            arrfree(ctx->crossRefMap[i].value);
        }
        hmfree(ctx->crossRefMap);
    }

    // Free any remaining env frames
    EnvFrame* envFrame = ctx->envStack;
    while (envFrame != nullptr) {
        EnvFrame* parent = envFrame->parent;
        arrfree(envFrame->instanceList);
        free(envFrame);
        envFrame = parent;
    }

    // Free any remaining call frames
    CallFrame* frame = ctx->callStack;
    while (frame != nullptr) {
        CallFrame* parent = frame->parent;
        free(frame);
        frame = parent;
    }

    free(ctx);
}
