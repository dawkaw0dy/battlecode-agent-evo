#pragma once

void play_draft();

// Opening plan from the draft memory: score multiplier for a camp group in the first rounds.
float opening_bias(int group, int round);
