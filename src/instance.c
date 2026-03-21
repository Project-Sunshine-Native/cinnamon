#include "instance.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds.h"
#include "utils.h"

Instance* Instance_create(uint32_t instanceId, int32_t objectIndex, double x, double y) {
    Instance* inst = safeCalloc(1, sizeof(Instance));
    inst->instanceId = instanceId;
    inst->objectIndex = objectIndex;
    inst->x = x;
    inst->y = y;
    inst->xprevious = x;
    inst->yprevious = y;
    inst->xstart = x;
    inst->ystart = y;
    inst->maskIndex = -1;
    inst->persistent = false;
    inst->solid = false;
    inst->active = true;
    inst->visible = true;
    inst->outsideRoom = false;
    inst->spriteIndex = -1;
    inst->imageSpeed = 1.0;
    inst->imageIndex = 0.0;
    inst->imageXscale = 1.0;
    inst->imageYscale = 1.0;
    inst->imageAngle = 0.0;
    inst->imageAlpha = 1.0;
    inst->imageBlend = 0xFFFFFF;
    inst->depth = 0;
    inst->speed = 0.0;
    inst->direction = 0.0;
    inst->hspeed = 0.0;
    inst->vspeed = 0.0;
    inst->friction = 0.0;
    inst->gravity = 0.0;
    inst->gravityDirection = 270.0;
    inst->pathIndex = -1;
    inst->pathScale = 1.0;
    inst->selfVars = nullptr;
    inst->selfArrayMap = nullptr;
    inst->selfArrayVarTracker = nullptr;

    // Initialize alarms to -1 (inactive)
    repeat(GML_ALARM_COUNT, i) {
        inst->alarm[i] = -1;
    }

    return inst;
}

void Instance_free(Instance* instance) {
    if (instance == nullptr) return;

    // Free owned strings in selfVars hashmap
    repeat(hmlen(instance->selfVars), i) {
        RValue_free(&instance->selfVars[i].value);
    }
    hmfree(instance->selfVars);

    // Free selfArrayMap
    repeat(hmlen(instance->selfArrayMap), i) {
        RValue_free(&instance->selfArrayMap[i].value);
    }
    hmfree(instance->selfArrayMap);

    // Free selfArrayVarTracker
    hmfree(instance->selfArrayVarTracker);

    free(instance);
}

// Compute speed and direction from hspeed/vspeed (HTML5: Compute_Speed1)
void Instance_computeSpeedFromComponents(Instance* inst) {
    // Direction
    if (inst->hspeed == 0.0) {
        if (inst->vspeed > 0.0) {
            inst->direction = 270.0;
        } else if (inst->vspeed < 0.0) {
            inst->direction = 90.0;
        }
        // If both are 0, direction stays unchanged
    } else {
        double dd = clampFloat(180.0 * atan2(inst->vspeed, inst->hspeed) / M_PI);
        if (dd <= 0.0) {
            inst->direction = -dd;
        } else {
            inst->direction = 360.0 - dd;
        }
    }

    // Round direction if very close to integer
    if (fabs(inst->direction - round(inst->direction)) < 0.0001) {
        inst->direction = round(inst->direction);
    }
    inst->direction = fmod(inst->direction, 360.0);

    // Speed
    inst->speed = sqrt(inst->hspeed * inst->hspeed + inst->vspeed * inst->vspeed);
    if (fabs(inst->speed - round(inst->speed)) < 0.0001) {
        inst->speed = round(inst->speed);
    }
}

// Compute hspeed/vspeed from speed and direction (HTML5: Compute_Speed2)
void Instance_computeComponentsFromSpeed(Instance* inst) {
    inst->hspeed = inst->speed * clampFloat(cos(inst->direction * (M_PI / 180.0)));
    inst->vspeed = -inst->speed * clampFloat(sin(inst->direction * (M_PI / 180.0)));

    // Round if very close to integer
    if (fabs(inst->hspeed - round(inst->hspeed)) < 0.0001) {
        inst->hspeed = round(inst->hspeed);
    }
    if (fabs(inst->vspeed - round(inst->vspeed)) < 0.0001) {
        inst->vspeed = round(inst->vspeed);
    }
}
