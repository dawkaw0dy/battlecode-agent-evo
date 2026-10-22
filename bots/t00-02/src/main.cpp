// fake agent t00-02: standstill
#include <cambc.h>
#include "draft.h"

int main() {
    while (true) {
        if (cambc::is_draft())
            play_draft();
        else {
            // stand still
        }
        cambc::end_turn();
    }
}
