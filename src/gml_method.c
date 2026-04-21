#include "gml_method.h"
#include "common.h"
#include "utils.h"
#include <stdlib.h>

GMLMethod* GMLMethod_create(int32_t codeIndex, int32_t boundInstanceId) {
    GMLMethod* m = safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = codeIndex;
    m->boundInstanceId = boundInstanceId;
    return m;
}

void GMLMethod_incRef(GMLMethod* m) {
    if (m == nullptr) return;
    m->refCount++;
}

void GMLMethod_decRef(GMLMethod* m) {
    if (m == nullptr) return;
    require(m->refCount > 0);
    m->refCount--;
    if (m->refCount > 0) return;
    free(m);
}
