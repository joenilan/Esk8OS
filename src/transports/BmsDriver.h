#pragma once
#include "transports/BmsTarget.h"

namespace Esk8OS { namespace Transports {

struct BmsData;

// Register a verified implementation here instead of teaching app/UI code a
// vendor protocol. Driver code owns parsing, connection lifecycle and locking.
struct BmsDriver {
    const char* id;
    const char* label;
    void (*begin)();
    bool (*read)(BmsData*);
    void (*loadTarget)();
    BmsTarget (*target)();
    BmsTargetResult (*setTarget)(const char*);
};

const BmsDriver* activeBmsDriver(); // nullptr means no BMS in this build
void beginBms();
bool getBmsData(BmsData* out);
void loadBmsTarget();
BmsTarget getBmsTarget();
BmsTargetResult setBmsTarget(const char* address);

} }
