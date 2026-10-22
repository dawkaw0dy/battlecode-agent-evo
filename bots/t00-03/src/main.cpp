// fake agent t00-03: faulter
#include <cambc.h>
#include "draft.h"

int main() {
    while (true) {
        if (cambc::is_draft())
            play_draft();
        else {
            __builtin_trap();
        }
        cambc::end_turn();
    }
}
