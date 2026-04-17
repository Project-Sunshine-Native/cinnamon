#pragma once

#include "vm.h"

typedef RValue (*BuiltinFunc)(VMContext* ctx, RValue* args, int32_t argCount);

void VMBuiltins_registerAll(void);
BuiltinFunc VMBuiltins_find(const char* name);
RValue VMBuiltins_getVariable(VMContext* ctx, const char* name, int32_t arrayIndex);
void VMBuiltins_setVariable(VMContext* ctx, const char* name, RValue val, int32_t arrayIndex);