#pragma once
#include "common.h"
#include "mapinfo.h"
#include "plan.h"
#include <memory>

struct EnemyView {
    vec2 pos{};
    float hp = 0, maxhp = 1, str = 0;
    int range = 1;
    int gems = 0;
    bool channel = false;   // tributing or recalling: cannot fight back this turn
    cambc::champion_type type{};
};

struct AllyView {
    vec2 pos{};
    float hp = 0, maxhp = 1, str = 0;
    int range = 1;
};

// One ability slot per role (-1 = none).
struct Kit {
    int reset = -1, nuke = -1, aoe = -1, cc = -1, heal = -1, dash = -1, shield = -1;
    bool nuke_excl = false, aoe_excl = false, cc_excl = false;
    float nuke_range = 0, aoe_range = 0, cc_range = 0, heal_range = 0, dash_range = 0;
    float nuke_mod = 0, aoe_mod = 0, cc_mod = 0, aoe_radius = 0;
    bool cc_targets_camps = false, nuke_targets_camps = false, aoe_hits_camps = true;
};

// What a champion remembers about one camp group.
struct GroupKnow {
    int ready = 0;          // round the group is expected to be alive again (0 = alive now)
    int seen = -1;          // last round any of its tiles was in vision
    bool reward = false;    // monsters dead, capture pending
    int enemy_round = -1000;  // last round enemies were seen near it
};

class Bot {
public:
    void play_turn();

private:
    // setup
    void init();
    void read_kit();
    int body_slot() const;

    // per-turn
    void snapshot();
    void update_groups();
    void shop();
    void finish_turn();

    // decisions
    bool handle_enemies();
    bool should_recall();
    bool bank();
    void farm();
    void choose_target();
    bool execute_target(int g);
    bool attack_camps_in_range(int g);
    bool retreat_from(const std::vector<vec2>& threats);
    void idle();

    // helpers
    bool step_towards(bitboard goals, bool allow_dash);
    bitboard blocked_now() const;
    const bitboard& approach_tiles(int g);
    int group_value(int g) const;
    float my_camp_dps() const;
    bool use_reset_and_attack(const cambc::unit& target);
    bool cast_on_camp(const cambc::camp& c, bool will_move);
    void cast_support();
    float reach_of(const EnemyView& e) const;
    int home_dist(vec2 p) const;
    void write_claim(int g);
    std::array<int, 6> read_claims() const;

    bool inited_ = false;
    Plan plan_;
    int slot_ = 0;
    Kit kit_;
    std::vector<GroupKnow> gk_;
    std::vector<std::unique_ptr<bitboard>> approach_;
    cambc::distance_field home_field_;
    bitboard home_reached_;
    bool ranged_ = false;
    bool tavi_ = false;
    cambc::champion_type type_{};

    int target_ = -1;
    int target_since_ = 0;
    int last_eval_ = -100;
    int stall_ = 0;
    int stall_group_ = -1;
    bool shrink_ = false;
    int shards_ = 0;
    int last_enemy_round_ = -1000;
    vec2 last_enemy_pos_{-1, -1};
    int banking_ = 0;           // 1 while on a bank trip
    float last_hp_ = 0;
    int warns_ = 0;
    int branch_ = 0;

    // turn snapshot
    int round_ = 0;
    vec2 pos_{};
    float hp_ = 0, maxhp_ = 1, str_ = 0;
    int gems_ = 0, gold_ = 0, range_ = 1;
    bool damaged_ = false;
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    bool at_spawn_ = false;
    bitboard visible_, enemy_tiles_, ally_tiles_, camp_tiles_;
    std::vector<EnemyView> enemies_;
    std::vector<AllyView> allies_;
};
