#pragma once
#include "common.h"
#include "mapinfo.h"
#include "plan.h"
#include <memory>

struct EnemyView {
    vec2 pos{};
    float hp = 0, maxhp = 1, str = 0;
    float dps = 0, burst = 0;   // sustained damage per turn and opening spell damage
    bool dasher = false;
    int range = 1;
    int gems = 0;
    bool channel = false;   // tributing or recalling: cannot fight back this turn
    cambc::champion_type type{};
};

struct AllyView {
    vec2 pos{};
    float hp = 0, maxhp = 1, str = 0;
    float dps = 0;
    int range = 1;
};

struct AwakeCamp {
    vec2 pos{};
    float str = 0, hp = 0;
    int range = 0;
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
    float hp_seen = -1;       // monster health left when last seen alive (-1 = full)
};

bool dash_capable(cambc::champion_type t);
void champion_power(cambc::champion_type t, float str, float focus, float* dps, float* burst);

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
    void place_lock();

    // decisions
    bool handle_enemies();
    float fight_value(bool* i_die) const;
    bool flee();
    bool hold_spawn();
    bool engage();
    float danger_at(vec2 t, bool dash_reach) const;
    float camp_danger_at(vec2 t) const;
    bool should_recall();
    bool bank();
    void choose_altar();
    void farm();
    void choose_target(bool force);
    void clear_model(const GroupInfo& G, float hp_scale, float dps, float* time, float* dmg) const;
    bool execute_target(int g);
    bool attack_camps_in_range(int g);
    void idle();
    int raid_target() const;
    bool raid_step(int g);

    // helpers
    bool step_towards(bitboard goals, bool allow_dash, int key = -1);
    bool walk_to(vec2 t);
    void disengage_step();
    bitboard blocked_now() const;
    const bitboard& approach_tiles(int g);
    int group_value(int g) const;
    float my_camp_dps() const;
    bool use_reset_and_attack(const cambc::unit& target);
    bool cast_on_camp(const cambc::camp& c, bool will_move);
    void cast_support();
    int home_dist(vec2 p) const;
    void write_claim(int g);
    void update_squad();
    bool same_squad(int s) const {
        return slot_ < 5 && s < 5 && plan_.squad[static_cast<size_t>(s)] == plan_.squad[static_cast<size_t>(slot_)];
    }
    std::array<int, 6> read_claims() const;

    bool inited_ = false;
    Plan plan_;
    int slot_ = 0;
    Kit kit_;
    std::vector<GroupKnow> gk_;
    std::vector<std::unique_ptr<bitboard>> approach_;
    cambc::distance_field home_field_;
    cambc::distance_field nav_field_;
    bitboard nav_reached_;
    int nav_key_ = -1, nav_round_ = -1000;
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
    int bank_altar_ = -1, bank_choice_ = -100;
    std::vector<bool> altar_locked_;   // enemy lock last seen on each altar
    std::vector<int> altar_enemy_;     // last round enemies were seen near each altar
    float last_hp_ = 0;
    int warns_ = 0;
    int submitted_ = 0;
    std::array<std::uint64_t, 6> fuel_mark_{};
    std::uint64_t fuel_choose_ = 0, fuel_step_ = 0;
    int branch_ = 0;
    enum Act { kActDead, kActChannel, kActHeal, kActFight, kActFlee, kActBank, kActHit, kActWalk, kActCapture, kActIdle, kActWait, kActRecall, kActs };
    int act_ = 0;
    std::array<int, kActs> acts_{};

    // turn snapshot
    int round_ = 0;
    vec2 pos_{};
    float hp_ = 0, maxhp_ = 1, str_ = 0;
    int gems_ = 0, gold_ = 0, range_ = 1;
    bool damaged_ = false;
    float dmg_taken_ = 0;
    bool moved_ = false, attacked_ = false, cast_ = false, move_failed_ = false;
    bool at_spawn_ = false;
    bitboard visible_, enemy_tiles_, ally_tiles_, camp_tiles_;
    std::vector<EnemyView> enemies_;
    std::vector<AllyView> allies_;
    std::vector<AwakeCamp> awake_;
    std::vector<EnemyView> threats_;             // visible enemies plus those seen in the last 2 rounds
    std::array<EnemyView, 16> mem_{};
    std::array<int, 16> mem_round_{};
    int flee_until_ = -1;
    bool disengage_ = false;
    int rally_wait_ = 0;
    vec2 avoid_pos_{-100, -100};
    int avoid_until_ = -1;
    bool was_recalling_ = false, just_recalled_ = false;
    int last_hit_round_ = -100;
    int leader_slot_ = 0, leader_target_ = -1, squad_n_ = 1;
    float my_dps_ = 0, my_burst_ = 0;
};
