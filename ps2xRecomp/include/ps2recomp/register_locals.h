#ifndef PS2RECOMP_REGISTER_LOCALS_H
#define PS2RECOMP_REGISTER_LOCALS_H

#include <string>

namespace ps2recomp
{
    // Rewrites one generated function so guest registers live in local
    // variables and reach the CPU context only where it can be observed.
    // Returns the input unchanged when the text has a shape it does not know.
    std::string applyRegisterLocals(const std::string &functionText);
}

#endif
