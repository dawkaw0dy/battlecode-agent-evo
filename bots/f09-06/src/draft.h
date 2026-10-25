#pragma once
#include <cambc.h>
#include <vector>

void play_draft();
// Our picks in order; the first team_size() of them are taken.
std::vector<cambc::champion_draft> draft_order();
bool draft_type_matches(cambc::champion_draft pick, cambc::champion_type type);
